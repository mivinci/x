/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * router.h - xpp::http::Router: composable routing + middleware.
 *
 * Aligned with the Rust ecosystem (tower/axum), not Go:
 *
 *   - The unified handler type is ASYNC: Request -> Promise<Result<Response>>
 *     (hyper's Service::call -> Future). A Router itself is a handler —
 *     `router(req)` dispatches — so routers nest and compose like axum
 *     (Router is a Service).
 *   - Middleware is a function Handler -> Handler (tower's Layer, direct
 *     function form). Registration order follows tower's ServiceBuilder:
 *     the first layer registered is the outermost.
 *   - nest() mounts a sub-router under a prefix and STRIPS it before the
 *     sub-router sees the path (axum::Router::nest semantics) — sub-routers
 *     are prefix-unaware and independently reusable.
 *   - fallback() names the unmatched-path handler (axum's name). Without
 *     one, unmatched paths answer 404; a path that matches some route's
 *     pattern but not its method answers 405.
 *   - Typed parameter injection is kept (axum's extractors prove it composes
 *     with the uniform boundary): `route("GET /users/:id", [](Request, String id))`
 *     binds `:id` by pattern order. Layers can also read parameters via
 *     `Request::param("id")` — matching happens before the layer chain runs.
 *
 * The Server mounts one Router as its sole route (the C resolver returns a
 * single entry; matching, params, 404/405, and layers all happen here).
 * A Router is also usable standalone — call `router(req)` directly with a
 * built Request to unit-test routing/middleware without sockets.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_ROUTER_H
#define XPP_HTTP_ROUTER_H

#include <functional>
#include <utility>

#include <xpp/http/method.h>
#include <xpp/http/request.h>
#include <xpp/http/response.h>
#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise.h>
#include <xpp/result.h>
#include <xpp/string.h>
#include <xpp/vec.h>

namespace xpp {
namespace http {

class Router;

namespace _ {

/* ── Signature traits (extract Ret/Args from a callable) ───────────── */

template <class T> struct function_traits;
template <class Ret, class... Args> struct function_traits<Ret(Args...)> {
  using Ret_t                   = Ret;
  static constexpr size_t arity = sizeof...(Args);
};
template <class Ret, class... Args>
struct function_traits<Ret (*)(Args...)> : function_traits<Ret(Args...)> {};
template <class Ret, class... Args>
struct function_traits<Ret (&)(Args...)> : function_traits<Ret(Args...)> {};
template <class Ret, class... Args>
struct function_traits<std::function<Ret(Args...)>> : function_traits<Ret(Args...)> {};
template <class Ret, class C, class... Args>
struct function_traits<Ret (C::*)(Args...) const> : function_traits<Ret(Args...)> {};
template <class Ret, class C, class... Args>
struct function_traits<Ret (C::*)(Args...)> : function_traits<Ret(Args...)> {};
template <class T> struct function_traits : function_traits<decltype(&T::operator())> {}; // lambdas

/* ── Handler return adaptation: Response / Result<Response> / Promise ── */

inline Promise<Result<Response>> adapt_handler_result(Response r) {
  return xpp::resolve(http::Result<Response>(xpp::ok, std::move(r)));
}
inline Promise<Result<Response>> adapt_handler_result(http::Result<Response> r) {
  return xpp::resolve(std::move(r));
}
inline Promise<Result<Response>> adapt_handler_result(Promise<http::Result<Response>> p) {
  return p;
}

/* ── Parameter injection (params[I] in pattern order, type String) ──── */

template <class H, size_t... I>
Promise<Result<Response>> invoke_with_params(H &h, Request req, const Vec<String> &params,
                                             std::index_sequence<I...>) {
  return adapt_handler_result(h(std::move(req), params[I]...));
}

/* ── Pattern parsing and path matching ─────────────────────────────── */

/**
 * @brief One parsed route: "METHOD /path/:param" → method + segments.
 *
 * Segments are literal text or ":name" (a parameter, in pattern order).
 * Matching follows libx's mux semantics: segment-exact or parameter,
 * empty segments skipped, query string ignored.
 */
struct RouteEntry {
  String      method;      ///< "" = any method (pattern had no METHOD part)
  Vec<String> segments;    ///< literal text or ":name"
  Vec<String> param_names; ///< ":name" segments in pattern order
  std::function<Promise<Result<Response>>(Request, Vec<String>)> invoke;
};

/// Parse "METHOD /path" or "/path" into method + segments + param names.
inline void parse_pattern(const char *pattern, String &method, Vec<String> &segments,
                          Vec<String> &param_names) {
  const char *p = pattern;
  // Optional method token before the first space.
  const char *sp = pattern;
  while (*sp && *sp != ' ')
    ++sp;
  if (*sp == ' ') {
    method = String::from_utf8(p, static_cast<size_t>(sp - p)).unwrap();
    p      = sp + 1;
  } else {
    method = String();
  }
  // Path segments; empty segments skipped (like the C mux).
  while (*p) {
    if (*p == '/') {
      ++p;
      continue;
    }
    const char *start = p;
    while (*p && *p != '/')
      ++p;
    String seg = String::from_utf8(start, static_cast<size_t>(p - start)).unwrap();
    auto   sb  = seg.as_bytes();
    if (sb.size() > 0 && sb.data()[0] == ':') {
      param_names.push(
        String::from_utf8(reinterpret_cast<const char *>(sb.data() + 1), sb.size() - 1).unwrap());
    }
    segments.push(std::move(seg));
  }
}

/// Split a request path into segments (query string stripped, empties skipped).
inline Vec<String> split_path(const uint8_t *p, size_t len) {
  Vec<String> segments;
  size_t      i = 0;
  while (i < len && p[i] != '?') {
    if (p[i] == '/') {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < len && p[i] != '/' && p[i] != '?')
      ++i;
    segments.push(String::from_utf8(reinterpret_cast<const char *>(p + start), i - start).unwrap());
  }
  return segments;
}

inline Vec<String> split_path(const char *url) {
  size_t len = 0;
  while (url[len])
    ++len;
  return split_path(reinterpret_cast<const uint8_t *>(url), len);
}

inline Vec<String> split_path(const String &s) {
  auto b = s.as_bytes();
  return split_path(b.data(), b.size());
}

/// Case-insensitive method equality (the C mux used strcasecmp).
inline bool method_equal(const String &a, const char *b) {
  auto   ab = a.as_bytes();
  size_t i  = 0;
  for (; i < ab.size() && b[i]; ++i) {
    char ca = static_cast<char>(ab.data()[i]);
    if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
    char cb = b[i];
    if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
    if (ca != cb) return false;
  }
  return i == ab.size() && b[i] == '\0';
}

} // namespace _

/**
 * @brief Composable HTTP router with middleware (tower/axum-aligned).
 *
 * A Router owns routes, nested sub-routers, a middleware stack, and a
 * fallback. Calling it with a Request dispatches: match (path + method,
 * parameters extracted) → middleware chain (outermost first) → handler.
 * A Router is itself a handler — hand one to `ServerBuilder::router()`,
 * nest it under a prefix, or call it directly in tests.
 *
 * @code
 *   Router r;
 *   r.layer(logging)                       // outermost
 *     .route("GET /users/:id", [](Request req, String id) { return Response::ok(id); });
 *
 *   Router api;
 *   api.route("/health", [] { ... });
 *   r.nest("/api", std::move(api));        // strips "/api" for `api`
 *
 *   auto server = Server::builder()
 *                   .router(std::move(r))
 *                   .bind("127.0.0.1", 8080).build().unwrap();
 * @endcode
 */
class Router {
public:
  /// Unified async handler type (hyper Service::call ↔): Request → response.
  using HandlerFn = std::function<Promise<Result<Response>>(Request)>;
  /// Middleware: Handler → Handler (tower Layer, direct function form).
  using LayerFn = std::function<HandlerFn(HandlerFn)>;

  Router()                              = default;
  Router(Router &&) noexcept            = default;
  Router &operator=(Router &&) noexcept = default;
  Router(const Router &)                = delete;
  Router &operator=(const Router &)     = delete;

  /**
   * @brief Register a route.
   *
   * @p pattern is "METHOD /path" or "/path" (any method); ":name"
   * segments become handler parameters, injected in pattern order as
   * `String`. Handler signature: `Ret(Request, String, ...)` with Ret one
   * of Response / Result<Response> / Promise<Result<Response>>.
   */
  template <class H> Router &route(const char *pattern, H &&handler) {
    String      method;
    Vec<String> segments;
    Vec<String> param_names;
    _::parse_pattern(pattern, method, segments, param_names);

    using H_t    = typename std::decay<H>::type;
    using Traits = _::function_traits<H_t>;
    static_assert(Traits::arity >= 1, "handler must take at least (Request)");
    XPP_ASSERT(Traits::arity == 1 + param_names.len(),
               "handler parameter count must match :param count in the pattern");

    H_t  stored        = std::forward<H>(handler);
    auto state         = Own<_::RouteEntry>(new _::RouteEntry());
    state->method      = std::move(method);
    state->segments    = std::move(segments);
    state->param_names = std::move(param_names);
    state->invoke      = [stored](Request req, Vec<String> params) -> Promise<Result<Response>> {
      return _::invoke_with_params(stored, std::move(req), params,
                                        std::make_index_sequence<Traits::arity - 1>());
    };
    m_routes.push(std::move(state));
    return *this;
  }

  /**
   * @brief Add a middleware layer.
   *
   * A middleware is `Handler -> Handler` where Handler is
   * `Request -> Promise<Result<Response>>`. Registration order follows
   * tower's ServiceBuilder: the FIRST layer registered is the OUTERMOST
   * (it sees the request first and the response last).
   */
  template <class M> Router &layer(M &&m) {
    m_layers.push(LayerFn(std::forward<M>(m)));
    return *this;
  }

  /**
   * @brief Mount a sub-router under a prefix, stripping it (axum nest).
   *
   * "/api/users/1" dispatches into @p sub as "/users/1" — the sub-router
   * is prefix-unaware and independently reusable. The prefix must be a
   * literal path (no ":params").
   */
  Router &nest(const char *prefix, Router sub) {
    // The sub-router is moved in — frozen from here on. Bake its layer
    // chain into its routes/fallback so nested dispatch applies them
    // (sub layers are INNER relative to this router's layers).
    sub.bake_layers();
    String p = String::from_utf8(prefix).unwrap();
    // Normalize: ensure a leading '/', no trailing '/' (except root).
    auto pb = p.as_bytes();
    if (pb.size() == 0 || pb.data()[0] != '/') {
      String slash = String::from_utf8("/").unwrap();
      slash.push_str(p);
      p = std::move(slash);
    }
    while (p.len() > 1) {
      auto b = p.as_bytes();
      if (b.data()[b.size() - 1] != '/') break;
      p = p.substr(0, p.len() - 1);
    }
    XPP_ASSERT(p.find(":").is_none(), "nest prefix must not contain :params");
    m_nested.push(Nested{std::move(p), Own<Router>(new Router(std::move(sub)))});
    return *this;
  }

  /**
   * @brief Merge another router's routes and nested routers (axum merge).
   *
   * Patterns keep their original paths (no prefix). A fallback set on
   * @p other is adopted only if this router has none.
   */
  Router &merge(Router other) {
    // `other` is moved in — bake its layers into its routes first.
    other.bake_layers();
    for (auto &r : other.m_routes)
      m_routes.push(std::move(r));
    for (auto &n : other.m_nested)
      m_nested.push(Nested{n.prefix, Own<Router>(new Router(std::move(*n.router)))});
    if (m_fallback.is_none() && other.m_fallback.is_some())
      m_fallback = std::move(other.m_fallback);
    return *this;
  }

  /**
   * @brief Set the unmatched-path handler (axum fallback).
   *
   * Replaces the default 404 for paths no route matches. Method
   * mismatches (path matches a pattern, method doesn't) still answer
   * 405 automatically.
   */
  template <class H> Router &fallback(H &&handler) {
    using H_t = typename std::decay<H>::type;
    static_assert(_::function_traits<H_t>::arity == 1, "fallback handler takes a single (Request)");
    H_t stored = std::forward<H>(handler);
    m_fallback = HandlerFn([stored](Request req) -> Promise<Result<Response>> {
      return _::adapt_handler_result(stored(std::move(req)));
    });
    return *this;
  }

  /**
   * @brief Dispatch a request (the Router is itself a handler).
   *
   * Matches the path, extracts parameters (also stored on the Request —
   * `req.param("id")`), applies the middleware chain around the matched
   * handler, and returns its eventual response. Usable standalone for
   * routing/middleware unit tests without a server. The URL's query
   * string is ignored; only the path participates in matching.
   */
  Promise<Result<Response>> operator()(Request req) {
    HandlerFn endpoint = compose(req);
    return endpoint(std::move(req));
  }

  /**
   * @brief Synchronously compose the dispatch for a request.
   *
   * Matches, extracts parameters (stored on @p req for `req.param()`),
   * and wraps the matched handler (or 405 / fallback / 404) with the
   * middleware chain. The returned HandlerFn is fully self-contained —
   * every capture is by value — so it stays callable after the Router
   * itself has been destroyed. The Server composes at request arrival
   * (while the server is alive) and spawns only the endpoint, since
   * xpp::spawn defers execution to a later loop iteration; do the same
   * if you defer dispatch yourself.
   */
  HandlerFn compose(Request &req) {
    const char *method = to_string(req.method());
    auto        ub     = req.url().as_bytes();
    Match       m      = match(ub.data(), ub.size(), method);

    // Endpoint: the matched handler (invoke copied by value — the
    // self-containment note above), or 405 / fallback / 404.
    HandlerFn endpoint;
    if (m.entry) {
      auto invoke = m.entry->invoke;
      auto params = m.params;
      endpoint    = HandlerFn([invoke, params](Request r) -> Promise<Result<Response>> {
        return invoke(std::move(r), params);
      });
    } else if (m.path_matched) {
      endpoint = HandlerFn([](Request) -> Promise<Result<Response>> {
        return _::adapt_handler_result(
          ResponseBuilder().status(StatusCode::MethodNotAllowed).body());
      });
    } else if (m.sub_fallback) {
      endpoint = *m.sub_fallback; // nested sub-tree fallback (axum nest)
    } else if (m_fallback.is_some()) {
      endpoint = m_fallback.unwrap();
    } else {
      endpoint = HandlerFn([](Request) -> Promise<Result<Response>> {
        return _::adapt_handler_result(ResponseBuilder().status(StatusCode::NotFound).body());
      });
    }

    // Path parameters: stored for req.param() (layers read them — matching
    // runs before the layer chain).
    if (m.entry && m.param_names) {
      for (size_t i = 0; i < m.param_names->len() && i < m.params.len(); ++i) {
        req.add_path_param((*m.param_names)[i], m.params[i]);
      }
    }

    // Wrap the middleware chain: outermost (first registered) first.
    for (size_t i = m_layers.len(); i-- > 0;)
      endpoint = m_layers[i](endpoint);

    return endpoint;
  }

  /* ── Internals (used by the Server's resolve trampoline + tests) ─── */

  struct Match {
    _::RouteEntry *entry        = nullptr; ///< matched route (method ok)
    bool           path_matched = false;   ///< some pattern matched the path
    Vec<String>    params;                 ///< extracted, pattern order
    Vec<String>   *param_names = nullptr;  ///< entry's names (null if none)
    /// Fallback of the deepest prefix-entered sub-router that has one
    /// (axum nest semantics: a nested router's fallback answers
    /// unmatched paths under its prefix). Null if none in the entered
    /// sub-tree — the outer fallback / 404 then applies.
    const HandlerFn *sub_fallback = nullptr;
  };

  /// Match a request path (query string ignored). First match wins.
  Match match(const uint8_t *url, size_t url_len, const char *method) const {
    Vec<String> segs = _::split_path(url, url_len);
    return match_segments(segs, method);
  }
  Match match(const char *url, const char *method) const {
    size_t len = 0;
    while (url[len])
      ++len;
    return match(reinterpret_cast<const uint8_t *>(url), len, method);
  }

private:
  struct Nested {
    String      prefix;
    Own<Router> router; ///< heap: Router is incomplete inside itself
  };

  /**
   * Fold the layer chain into every route invoker and the fallback.
   * Called when this router is moved into another (nest/merge) — it is
   * frozen afterwards, so the baked closures (layers copied by value,
   * self-contained) stay valid through later moves of the owner.
   */
  void bake_layers() {
    if (m_layers.len() == 0) return;
    wrap_subtree(m_layers);
    m_layers.clear();
  }

  /**
   * Wrap every route invoker, the fallback, AND every nested sub-tree
   * with @p layers (ours end up OUTER relative to already-baked inner
   * layers). The recursion is what makes depth-2+ nesting apply the
   * whole chain — bake_layers() on a router only fires when it is
   * moved into a parent, so intermediate routers' layers must be
   * pushed down here.
   */
  void wrap_subtree(const Vec<LayerFn> &layers) {
    if (layers.len() == 0) return;
    for (auto &r : m_routes) {
      auto invoke = r->invoke; // copy
      r->invoke   = [layers, invoke](Request req, Vec<String> params) -> Promise<Result<Response>> {
        HandlerFn endpoint = HandlerFn([invoke, params](Request rr) -> Promise<Result<Response>> {
          return invoke(std::move(rr), params);
        });
        for (size_t i = layers.len(); i-- > 0;)
          endpoint = layers[i](endpoint);
        return endpoint(std::move(req));
      };
    }
    if (m_fallback.is_some()) {
      HandlerFn wrapped = m_fallback.unwrap();
      for (size_t i = layers.len(); i-- > 0;)
        wrapped = layers[i](wrapped);
      m_fallback = xpp::some(std::move(wrapped));
    }
    for (auto &n : m_nested)
      n.router->wrap_subtree(layers);
  }

  Match match_segments(const Vec<String> &segs, const char *method) const {
    Match m;
    for (auto &r : m_routes) {
      Vec<String> params;
      if (segments_match(*r, segs, params)) {
        m.path_matched = true;
        if (r->method.len() == 0 || _::method_equal(r->method, method)) {
          m.entry       = r.get();
          m.params      = std::move(params);
          m.param_names = &r->param_names;
          return m;
        }
      }
    }
    for (auto &n : m_nested) {
      // The prefix must match on segment boundaries.
      Vec<String> psegs = _::split_path(n.prefix);
      if (psegs.len() > segs.len()) continue;
      bool ok = true;
      for (size_t i = 0; i < psegs.len(); ++i) {
        if (psegs[i] != segs[i]) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      Vec<String> rest;
      for (size_t i = psegs.len(); i < segs.len(); ++i)
        rest.push(segs[i]);
      Match sub = n.router->match_segments(rest, method);
      if (sub.entry) return sub;
      if (sub.path_matched) m.path_matched = true;
      // The prefix entered this sub-tree: its fallback (or a deeper
      // one already propagated into `sub`) answers unmatched paths.
      if (sub.sub_fallback) {
        m.sub_fallback = sub.sub_fallback;
      } else if (n.router->m_fallback.is_some()) {
        m.sub_fallback = &n.router->m_fallback.unwrap();
      }
    }
    return m;
  }

  /// Segment-wise pattern match (literal equal or ":param" capture).
  static bool segments_match(const _::RouteEntry &route, const Vec<String> &segs,
                             Vec<String> &params) {
    if (route.segments.len() != segs.len()) return false;
    for (size_t i = 0; i < segs.len(); ++i) {
      auto sb  = route.segments[i].as_bytes();
      bool isp = sb.size() > 0 && sb.data()[0] == ':';
      if (isp) {
        params.push(segs[i]);
      } else if (route.segments[i] != segs[i]) {
        return false;
      }
    }
    return true;
  }

  Vec<Own<_::RouteEntry>> m_routes;
  Vec<Nested>             m_nested;
  Vec<LayerFn>            m_layers;
  Option<HandlerFn>       m_fallback;
};

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_ROUTER_H
