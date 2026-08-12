/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * macros.h - XPP_SERDE declarative derive macro.
 *
 * Goal: one line of declaration auto-generates both Serialize<T> and
 * Deserialize<T> specializations for a plain struct, covering the 80%
 * case (no field attributes, default field-name mapping).
 *
 *   struct Person {
 *     xpp::String name;
 *     int32_t     age;
 *   };
 *   XPP_SERDE(Person, (name), (age));
 *
 * Field list syntax:
 *
 *   XPP_SERDE(Type, (field1), (field2), ...)
 *
 * Each field is a paren-wrapped name, comma-separated. This shape is
 * deliberate: Phase 3 will extend each `(name)` to `(name, attrs...)`
 * without breaking existing call sites.
 *
 * Implementation outline:
 *
 *   - XPP_FOR_EACH(macro, data, (a), (b)) iterates a paren-list by
 *     bounded unrolling (65 distinct macros, no recursion). Max 64
 *     fields. No Boost, no external code generator.
 *   - XPP_SERDE_EMIT_SER_FIELD(data, name) emits one
 *     `XPP_SERDE_TRY(scope.field("name", v.name));` statement.
 *   - XPP_SERDE_EMIT_DE_FIELD(data, name) emits one `else if (k == ...)`
 *     branch for the deserialize visitor.
 *
 * C++11-compatible. Header-only. No exceptions. No RTTI.
 */

#ifndef XPP_SERDE_MACROS_H
#define XPP_SERDE_MACROS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/error.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>
#include <xpp/void.h>

/* ═══ Preprocessor plumbing ═══ */

#define XPP_FE_CAT(a, b)  XPP_FE_CAT_(a, b)
#define XPP_FE_CAT_(a, b) a##b

/* Sentinel token — wrapped in parens at list tail: (XPP_FE_END).
 * Underscore-prefixed so it cannot collide with a user field name. */
#define XPP_FE_END _xpp_fe_end

/* XPP_FE_IS_END(x) — 1 if x is (XPP_FE_END), 0 otherwise.
 * x is a paren-wrapped token list. We compare the FIRST token inside
 * the parens to XPP_FE_END via token-paste (the remaining tokens are
 * left to UNWRAP later). This works because field tuples are always
 * `(field_name, ...)` — the first token is either a field name or
 * _xpp_fe_end. */
#define XPP_FE_IS_END(x)                   XPP_FE_IS_END_X x
#define XPP_FE_IS_END_X(first, ...)        XPP_FE_IS_END_DISPATCH(XPP_FE_IEP_##first)
#define XPP_FE_IEP__xpp_fe_end             ~, 1
#define XPP_FE_IS_END_DISPATCH(...)        XPP_FE_IS_END_DISPATCH_(__VA_ARGS__, 0)
#define XPP_FE_IS_END_DISPATCH_(a, b, ...) b

/* XPP_FE_IF(cond, then, else) — cond must be 0 or 1. */
#define XPP_FE_IF(cond, then, else) XPP_FE_CAT(XPP_FE_IF_, cond)(then, else)
#define XPP_FE_IF_0(then, else)     else
#define XPP_FE_IF_1(then, else)     then

/* XPP_FE_UNWRAP(x) — strip one paren layer: (name) -> name.
 * `XPP_FE_UNWRAP_ x` (space, not parens) is deliberate: when x is
 * substituted with `(name)`, the body becomes `XPP_FE_UNWRAP_ (name)`
 * which is a valid macro call. */
#define XPP_FE_UNWRAP(x)    XPP_FE_UNWRAP_ x
#define XPP_FE_UNWRAP_(...) __VA_ARGS__

/* XPP_FOR_EACH(macro, data, (a), (b), ...) — iterate, calling
 * `macro(data, name)` for each field. 65 distinct macros
 * (XPP_FE_0..XPP_FE_64) — no recursion, no painted-blue.
 * Max 64 fields. */
#define XPP_FOR_EACH(macro, data, ...) XPP_FE_0(macro, data, __VA_ARGS__, (XPP_FE_END))

#define XPP_FE_0(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_0)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_0(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_1(m, d, __VA_ARGS__)
#define XPP_FE_1(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_1)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_1(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_2(m, d, __VA_ARGS__)
#define XPP_FE_2(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_2)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_2(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_3(m, d, __VA_ARGS__)
#define XPP_FE_3(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_3)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_3(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_4(m, d, __VA_ARGS__)
#define XPP_FE_4(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_4)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_4(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_5(m, d, __VA_ARGS__)
#define XPP_FE_5(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_5)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_5(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_6(m, d, __VA_ARGS__)
#define XPP_FE_6(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_6)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_6(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_7(m, d, __VA_ARGS__)
#define XPP_FE_7(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_7)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_7(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_8(m, d, __VA_ARGS__)
#define XPP_FE_8(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_8)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_8(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_9(m, d, __VA_ARGS__)
#define XPP_FE_9(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_9)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_9(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_10(m, d, __VA_ARGS__)
#define XPP_FE_10(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_10)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_10(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_11(m, d, __VA_ARGS__)
#define XPP_FE_11(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_11)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_11(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_12(m, d, __VA_ARGS__)
#define XPP_FE_12(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_12)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_12(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_13(m, d, __VA_ARGS__)
#define XPP_FE_13(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_13)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_13(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_14(m, d, __VA_ARGS__)
#define XPP_FE_14(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_14)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_14(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_15(m, d, __VA_ARGS__)
#define XPP_FE_15(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_15)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_15(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_16(m, d, __VA_ARGS__)
#define XPP_FE_16(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_16)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_16(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_17(m, d, __VA_ARGS__)
#define XPP_FE_17(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_17)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_17(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_18(m, d, __VA_ARGS__)
#define XPP_FE_18(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_18)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_18(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_19(m, d, __VA_ARGS__)
#define XPP_FE_19(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_19)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_19(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_20(m, d, __VA_ARGS__)
#define XPP_FE_20(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_20)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_20(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_21(m, d, __VA_ARGS__)
#define XPP_FE_21(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_21)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_21(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_22(m, d, __VA_ARGS__)
#define XPP_FE_22(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_22)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_22(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_23(m, d, __VA_ARGS__)
#define XPP_FE_23(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_23)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_23(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_24(m, d, __VA_ARGS__)
#define XPP_FE_24(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_24)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_24(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_25(m, d, __VA_ARGS__)
#define XPP_FE_25(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_25)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_25(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_26(m, d, __VA_ARGS__)
#define XPP_FE_26(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_26)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_26(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_27(m, d, __VA_ARGS__)
#define XPP_FE_27(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_27)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_27(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_28(m, d, __VA_ARGS__)
#define XPP_FE_28(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_28)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_28(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_29(m, d, __VA_ARGS__)
#define XPP_FE_29(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_29)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_29(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_30(m, d, __VA_ARGS__)
#define XPP_FE_30(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_30)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_30(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_31(m, d, __VA_ARGS__)
#define XPP_FE_31(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_31)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_31(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_32(m, d, __VA_ARGS__)
#define XPP_FE_32(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_32)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_32(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_33(m, d, __VA_ARGS__)
#define XPP_FE_33(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_33)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_33(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_34(m, d, __VA_ARGS__)
#define XPP_FE_34(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_34)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_34(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_35(m, d, __VA_ARGS__)
#define XPP_FE_35(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_35)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_35(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_36(m, d, __VA_ARGS__)
#define XPP_FE_36(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_36)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_36(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_37(m, d, __VA_ARGS__)
#define XPP_FE_37(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_37)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_37(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_38(m, d, __VA_ARGS__)
#define XPP_FE_38(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_38)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_38(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_39(m, d, __VA_ARGS__)
#define XPP_FE_39(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_39)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_39(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_40(m, d, __VA_ARGS__)
#define XPP_FE_40(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_40)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_40(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_41(m, d, __VA_ARGS__)
#define XPP_FE_41(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_41)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_41(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_42(m, d, __VA_ARGS__)
#define XPP_FE_42(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_42)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_42(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_43(m, d, __VA_ARGS__)
#define XPP_FE_43(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_43)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_43(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_44(m, d, __VA_ARGS__)
#define XPP_FE_44(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_44)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_44(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_45(m, d, __VA_ARGS__)
#define XPP_FE_45(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_45)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_45(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_46(m, d, __VA_ARGS__)
#define XPP_FE_46(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_46)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_46(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_47(m, d, __VA_ARGS__)
#define XPP_FE_47(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_47)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_47(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_48(m, d, __VA_ARGS__)
#define XPP_FE_48(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_48)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_48(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_49(m, d, __VA_ARGS__)
#define XPP_FE_49(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_49)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_49(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_50(m, d, __VA_ARGS__)
#define XPP_FE_50(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_50)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_50(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_51(m, d, __VA_ARGS__)
#define XPP_FE_51(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_51)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_51(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_52(m, d, __VA_ARGS__)
#define XPP_FE_52(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_52)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_52(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_53(m, d, __VA_ARGS__)
#define XPP_FE_53(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_53)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_53(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_54(m, d, __VA_ARGS__)
#define XPP_FE_54(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_54)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_54(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_55(m, d, __VA_ARGS__)
#define XPP_FE_55(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_55)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_55(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_56(m, d, __VA_ARGS__)
#define XPP_FE_56(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_56)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_56(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_57(m, d, __VA_ARGS__)
#define XPP_FE_57(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_57)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_57(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_58(m, d, __VA_ARGS__)
#define XPP_FE_58(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_58)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_58(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_59(m, d, __VA_ARGS__)
#define XPP_FE_59(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_59)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_59(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_60(m, d, __VA_ARGS__)
#define XPP_FE_60(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_60)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_60(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_61(m, d, __VA_ARGS__)
#define XPP_FE_61(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_61)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_61(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_62(m, d, __VA_ARGS__)
#define XPP_FE_62(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_62)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_62(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_63(m, d, __VA_ARGS__)
#define XPP_FE_63(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_63)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_63(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_64(m, d, __VA_ARGS__)
#define XPP_FE_64(m, d, x, ...) \
  XPP_FE_IF(XPP_FE_IS_END(x), XPP_FE_END_MARKER, XPP_FE_STEP_64)(m, d, x, __VA_ARGS__)
#define XPP_FE_STEP_64(m, d, x, ...) m(d, XPP_FE_UNWRAP(x)) XPP_FE_65(m, d, __VA_ARGS__)
#define XPP_FE_END_MARKER(...)
#define XPP_FE_65(m, d, x, ...) /* recursion limit reached */

/* ═══ Field-type helper ═══ */

namespace xpp {
namespace serde {
namespace _ {

/* `field_type<Host>(&Host::field)` returns the declared type of a
 * pointer-to-member. Used by XPP_SERDE_EMIT_DE_FIELD so that
 * `next_value<T>` knows T without the user spelling it out. */
template <class Host, class T> T field_type(T Host::*);

} // namespace _
} // namespace serde
} // namespace xpp

/* ═══ XPP_SERDE field emitters ═══ */

/* Stringify / cat helpers — indirection forces macro argument expansion
 * before # / ##, so `name` can itself be a macro call (like
 * XPP_FE_UNWRAP((field))). */
#define XPP_SERDE_STR_I(x)    #x
#define XPP_SERDE_STR(x)      XPP_SERDE_STR_I(x)
#define XPP_SERDE_CAT_I(a, b) a##b
#define XPP_SERDE_CAT(a, b)   XPP_SERDE_CAT_I(a, b)

/* ── Attribute macros (public API) ────────────────────────────────────
 *
 * Each attribute expands DIRECTLY into the C++ code appropriate for
 * the current emit context. The emit macros use a 3-stage tuple pack
 * + EVAL3 + unpack pattern so attr macros get fully expanded before
 * being pasted into the dispatch call.
 *
 * Syntax for XPP_SERDE call sites:
 *
 *   XPP_SERDE(Type,
 *     (plain_field),
 *     (renamed_field,   XPP_FIELD_RENAME(renamed_field, "jsonName")),
 *     (defaulted_field,  XPP_FIELD_DEFAULT(defaulted_field, 80)),
 *     (skipped_field,    XPP_FIELD_SKIP(skipped_field)))
 */

#define XPP_FIELD_RENAME(field, json) XPP_SERDE_ATTR_RENAME, field, json
#define XPP_FIELD_DEFAULT(field, val) XPP_SERDE_ATTR_DEFAULT, field, val
#define XPP_FIELD_SKIP(field)         XPP_SERDE_ATTR_SKIP, field

/* ── HAS = 0/1 detector ──────────────────────────────────────────── */

#define XPP_SERDE_FIELD_HAS(...)                                    XPP_SERDE_FIELD_HAS_I(, ##__VA_ARGS__)
#define XPP_SERDE_FIELD_HAS_I(...)                                  XPP_SERDE_FIELD_HAS_II(XPP_SERDE_FIELD_HAS_NARG(__VA_ARGS__))
#define XPP_SERDE_FIELD_HAS_NARG(...)                               XPP_SERDE_FIELD_HAS_NARG_(__VA_ARGS__, 1, 1, 1, 1, 1, 0)
#define XPP_SERDE_FIELD_HAS_NARG_(_1, _2, _3, _4, _5, _6, has, ...) has
#define XPP_SERDE_FIELD_HAS_II(n)                                   n

/* ── EVAL — forces macro expansion of args ──────────────────────── */

#define XPP_SERDE_EVAL(...)  __VA_ARGS__
#define XPP_SERDE_EVAL2(...) XPP_SERDE_EVAL(XPP_SERDE_EVAL(__VA_ARGS__))
#define XPP_SERDE_EVAL3(...) XPP_SERDE_EVAL2(XPP_SERDE_EVAL2(__VA_ARGS__))

/* ── Field emitters — tuple-based dispatch ──────────────────────── */

#define XPP_SERDE_EMIT_SER_FIELD(data, name, ...) \
  XPP_SERDE_EMIT_SER_FIELD_I(data, name, ##__VA_ARGS__)
#define XPP_SERDE_EMIT_SER_FIELD_I(data, name, ...) \
  XPP_SERDE_EMIT_SER_FIELD_II(                      \
    (data, name, XPP_SERDE_FIELD_HAS(__VA_ARGS__), XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_SERDE_EMIT_SER_FIELD_II(tuple) XPP_SERDE_EMIT_SER_FIELD_III tuple
#define XPP_SERDE_EMIT_SER_FIELD_III(data, name, has, ...) \
  XPP_SERDE_SER_##has(data, name, ##__VA_ARGS__)

#define XPP_SERDE_EMIT_DE_FIELD(data, name, ...) \
  XPP_SERDE_EMIT_DE_FIELD_I(data, name, ##__VA_ARGS__)
#define XPP_SERDE_EMIT_DE_FIELD_I(data, name, ...) \
  XPP_SERDE_EMIT_DE_FIELD_II(                      \
    (data, name, XPP_SERDE_FIELD_HAS(__VA_ARGS__), XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_SERDE_EMIT_DE_FIELD_II(tuple) XPP_SERDE_EMIT_DE_FIELD_III tuple
#define XPP_SERDE_EMIT_DE_FIELD_III(data, name, has, ...) \
  XPP_SERDE_DE_##has(data, name, ##__VA_ARGS__)

#define XPP_SERDE_INIT_GOT(data, name, ...) XPP_SERDE_INIT_GOT_I(data, name, ##__VA_ARGS__)
#define XPP_SERDE_INIT_GOT_I(data, name, ...) \
  XPP_SERDE_INIT_GOT_II(                      \
    (data, name, XPP_SERDE_FIELD_HAS(__VA_ARGS__), XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_SERDE_INIT_GOT_II(tuple)                 XPP_SERDE_INIT_GOT_III tuple
#define XPP_SERDE_INIT_GOT_III(data, name, has, ...) XPP_SERDE_INIT_##has(data, name, ##__VA_ARGS__)

#define XPP_SERDE_EMIT_FIELD_NAME(data, name, ...) \
  XPP_SERDE_EMIT_FIELD_NAME_I(data, name, ##__VA_ARGS__)
#define XPP_SERDE_EMIT_FIELD_NAME_I(data, name, ...) \
  XPP_SERDE_EMIT_FIELD_NAME_II(                      \
    (data, name, XPP_SERDE_FIELD_HAS(__VA_ARGS__), XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_SERDE_EMIT_FIELD_NAME_II(tuple) XPP_SERDE_EMIT_FIELD_NAME_III tuple
#define XPP_SERDE_EMIT_FIELD_NAME_III(data, name, has, ...) \
  XPP_SERDE_NAME_##has(data, name, ##__VA_ARGS__)

#define XPP_SERDE_CHECK_REQUIRED(data, name, ...) \
  XPP_SERDE_CHECK_REQUIRED_I(data, name, ##__VA_ARGS__)
#define XPP_SERDE_CHECK_REQUIRED_I(data, name, ...) \
  XPP_SERDE_CHECK_REQUIRED_II(                      \
    (data, name, XPP_SERDE_FIELD_HAS(__VA_ARGS__), XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_SERDE_CHECK_REQUIRED_II(tuple) XPP_SERDE_CHECK_REQUIRED_III tuple
#define XPP_SERDE_CHECK_REQUIRED_III(data, name, has, ...) \
  XPP_SERDE_CHECK_##has(data, name, ##__VA_ARGS__)

/* ── Per-emit, per-attr implementations ──────────────────────────── */

/* Serialize: emit `scope.field("name", v.name);` */
#define XPP_SERDE_SER_0(data, name, ...)       XPP_SERDE_TRY(scope.field(XPP_SERDE_STR(name), v.name));
#define XPP_SERDE_SER_1(data, name, kind, ...) XPP_SERDE_SER_##kind(data, name, ##__VA_ARGS__)
#define XPP_SERDE_SER_XPP_SERDE_ATTR_RENAME(data, name, _field, _json) \
  XPP_SERDE_TRY(scope.field(_json, v.name));
#define XPP_SERDE_SER_XPP_SERDE_ATTR_DEFAULT(data, name, _field, _val) \
  XPP_SERDE_TRY(scope.field(XPP_SERDE_STR(name), v.name));
#define XPP_SERDE_SER_XPP_SERDE_ATTR_SKIP(data, name, _field) /* omitted from serialize output */

/* Deserialize: emit one `else if (k == "name")` branch. */
#define XPP_SERDE_DE_0(data, name, ...)                                                      \
  else if (k == XPP_SERDE_STR(name)) {                                                       \
    XPP_SERDE_TRY_VAR(                                                                       \
      v, m.template next_value<decltype(::xpp::serde::_::field_type<data>(&data::name))>()); \
    out.name                  = std::move(v);                                                \
    XPP_SERDE_CAT(got_, name) = true;                                                        \
  }
#define XPP_SERDE_DE_1(data, name, kind, ...) XPP_SERDE_DE_##kind(data, name, ##__VA_ARGS__)
#define XPP_SERDE_DE_XPP_SERDE_ATTR_RENAME(data, name, _field, _json)                        \
  else if (k == _json) {                                                                     \
    XPP_SERDE_TRY_VAR(                                                                       \
      v, m.template next_value<decltype(::xpp::serde::_::field_type<data>(&data::name))>()); \
    out.name                  = std::move(v);                                                \
    XPP_SERDE_CAT(got_, name) = true;                                                        \
  }
#define XPP_SERDE_DE_XPP_SERDE_ATTR_DEFAULT(data, name, _field, _val)                        \
  else if (k == XPP_SERDE_STR(name)) {                                                       \
    XPP_SERDE_TRY_VAR(                                                                       \
      v, m.template next_value<decltype(::xpp::serde::_::field_type<data>(&data::name))>()); \
    out.name                  = std::move(v);                                                \
    XPP_SERDE_CAT(got_, name) = true;                                                        \
  }
#define XPP_SERDE_DE_XPP_SERDE_ATTR_SKIP(data, name, _field) \
  /* no branch — field keeps its default-constructed value */

/* got_X init */
#define XPP_SERDE_INIT_0(data, name, ...)       bool XPP_SERDE_CAT(got_, name) = false;
#define XPP_SERDE_INIT_1(data, name, kind, ...) XPP_SERDE_INIT_##kind(data, name, ##__VA_ARGS__)
#define XPP_SERDE_INIT_XPP_SERDE_ATTR_RENAME(data, name, _field, _json) \
  bool XPP_SERDE_CAT(got_, name) = false;
#define XPP_SERDE_INIT_XPP_SERDE_ATTR_DEFAULT(data, name, _field, _val) \
  bool XPP_SERDE_CAT(got_, name) = false; /* CHECK applies default if still false */
#define XPP_SERDE_INIT_XPP_SERDE_ATTR_SKIP(data, name, _field) /* no got_X var needed */

/* Field-name array entry */
#define XPP_SERDE_NAME_0(data, name, ...)                               XPP_SERDE_STR(name),
#define XPP_SERDE_NAME_1(data, name, kind, ...)                         XPP_SERDE_NAME_##kind(data, name, ##__VA_ARGS__)
#define XPP_SERDE_NAME_XPP_SERDE_ATTR_RENAME(data, name, _field, _json) _json,
#define XPP_SERDE_NAME_XPP_SERDE_ATTR_DEFAULT(data, name, _field, _val) XPP_SERDE_STR(name),
#define XPP_SERDE_NAME_XPP_SERDE_ATTR_SKIP(data, name, _field)          /* not in field-name array */

/* Missing-field check */
#define XPP_SERDE_CHECK_0(data, name, ...)                                                 \
  if (!XPP_SERDE_CAT(got_, name)) {                                                        \
    return err(error(ErrorKind::MissingField, "missing field '" XPP_SERDE_STR(name) "'")); \
  }
#define XPP_SERDE_CHECK_1(data, name, kind, ...) XPP_SERDE_CHECK_##kind(data, name, ##__VA_ARGS__)
#define XPP_SERDE_CHECK_XPP_SERDE_ATTR_RENAME(data, name, _field, _json)     \
  if (!XPP_SERDE_CAT(got_, name)) {                                          \
    return err(error(ErrorKind::MissingField, "missing field '" _json "'")); \
  }
#define XPP_SERDE_CHECK_XPP_SERDE_ATTR_DEFAULT(data, name, _field, _val) \
  if (!XPP_SERDE_CAT(got_, name)) {                                      \
    out.name                  = _val;                                    \
    XPP_SERDE_CAT(got_, name) = true;                                    \
  }
#define XPP_SERDE_CHECK_XPP_SERDE_ATTR_SKIP(data, name, _field) /* no required check */

/* ═══ XPP_SERDE main macro ═══ */

#define XPP_SERDE(Type, ...) XPP_SERDE_(Type, XPP_FIELD_COUNT(__VA_ARGS__), __VA_ARGS__)

#define XPP_SERDE_(Type, N, ...)                                             \
  namespace xpp {                                                            \
  namespace serde {                                                          \
  template <> struct Serialize<Type> {                                       \
    template <class S> static Result<Void, Error> run(const Type &v, S &s) { \
      XPP_SERDE_TRY_VAR(scope, s.serialize_struct(#Type, N));                \
      XPP_FOR_EACH(XPP_SERDE_EMIT_SER_FIELD, _, __VA_ARGS__)                 \
      return scope.end();                                                    \
    }                                                                        \
  };                                                                         \
  template <> struct Deserialize<Type> {                                     \
    template <class D> static Result<Type, Error> run(D &d) {                \
      struct Visitor {                                                       \
        Result<Type, Error> visit_map(typename D::MapAccess &m) {            \
          Type out{};                                                        \
          XPP_FOR_EACH(XPP_SERDE_INIT_GOT, _, __VA_ARGS__)                   \
          while (true) {                                                     \
            XPP_SERDE_TRY_VAR(key, m.next_key());                            \
            if (key.is_none()) break;                                        \
            const xpp::String &k = key.unwrap();                             \
            if (false) {}                                                    \
            XPP_FOR_EACH(XPP_SERDE_EMIT_DE_FIELD, Type, __VA_ARGS__)         \
            else {                                                           \
              XPP_SERDE_TRY(m.next_value_ignored());                         \
            }                                                                \
          }                                                                  \
          XPP_FOR_EACH(XPP_SERDE_CHECK_REQUIRED, _, __VA_ARGS__)             \
          return ok(std::move(out));                                         \
        }                                                                    \
      };                                                                     \
      static const char *const kFields[] = {                                 \
        XPP_FOR_EACH(XPP_SERDE_EMIT_FIELD_NAME, _, __VA_ARGS__) nullptr};    \
      return d.deserialize_struct(#Type, kFields, N, Visitor{});             \
    }                                                                        \
  };                                                                         \
  }                                                                          \
  }

/* XPP_FIELD_COUNT(...) — count paren-wrapped fields. */
#define XPP_FIELD_COUNT(...)                                                                       \
  XPP_FIELD_COUNT_(__VA_ARGS__, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49,    \
                   48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, \
                   28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9,  \
                   8, 7, 6, 5, 4, 3, 2, 1, 0)
#define XPP_FIELD_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, \
                         _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30,  \
                         _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44,  \
                         _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58,  \
                         _59, _60, _61, _62, _63, _64, N, ...)                                  \
  N

/* ════════════════════════════════════════════════════════════════════
 * XPP_ENUM_SERDE — tagged-variant derive for Enum<Ts...> / Variant<Ts...>
 *
 * Two strategies: external (default) and adjacent.
 *
 * External:
 *   JSON:     {"circle": {...payload...}}
 *   Binary:   [u32 tag_index][payload bytes]
 *
 *   XPP_ENUM_SERDE(Shape,
 *     (ShapeCircle, "circle")
 *     (ShapeSquare, "square"))
 *
 * Adjacent:
 *   JSON:     {"tag":"circle","content":{...payload...}}
 *   Binary:   [tag_string bytes][payload bytes]
 *
 *   XPP_ENUM_SERDE_ADJACENT(Shape, "tag", "content",
 *     (ShapeCircle, "circle")
 *     (ShapeSquare, "square"))
 * ════════════════════════════════════════════════════════════════════ */

/* ── External strategy ── */

/* The 3-stage tuple pack/unpack pattern (same as XPP_SERDE field emitters):
 * XPP_FE_UNWRAP in the XPP_FOR_EACH call site doesn't expand before the
 * macro argument list is collected, so we EVAL3 inside a tuple and
 * unpack in a second stage. */

#define XPP_ENUM_SER(data, ...) XPP_ENUM_SER_I((data, XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_ENUM_SER_I(tuple)   XPP_ENUM_SER_II tuple
#define XPP_ENUM_SER_II(data, alt, tag)                                   \
  if (v.template is<alt>()) {                                             \
    XPP_SERDE_TRY_VAR(scope, s.serialize_variant(#data, v.index(), tag)); \
    XPP_SERDE_TRY(scope.payload(v.template get<alt>()));                  \
    return scope.end();                                                   \
  } else

#define XPP_ENUM_DE(data, ...) XPP_ENUM_DE_I((data, XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_ENUM_DE_I(tuple)   XPP_ENUM_DE_II tuple
#define XPP_ENUM_DE_II(data, alt, tag)              \
  if (std::strcmp(tag_str, tag) == 0) {             \
    XPP_SERDE_TRY_VAR(v, Deserialize<alt>::run(d)); \
    return ok(data(std::move(v)));                  \
  } else

#define XPP_ENUM_TAG(data, ...)   XPP_ENUM_TAG_I((XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_ENUM_TAG_I(tuple)     XPP_ENUM_TAG_II tuple
#define XPP_ENUM_TAG_II(alt, tag) tag,

#define XPP_ENUM_SERDE(Type, ...) XPP_ENUM_SERDE_(Type, XPP_FIELD_COUNT(__VA_ARGS__), __VA_ARGS__)

#define XPP_ENUM_SERDE_(Type, N, ...)                                                          \
  namespace xpp {                                                                              \
  namespace serde {                                                                            \
  template <> struct Serialize<Type> {                                                         \
    template <class S> static Result<Void, Error> run(const Type &v, S &s) {                   \
      XPP_FOR_EACH(XPP_ENUM_SER, Type, __VA_ARGS__)                                            \
      return err(error(ErrorKind::InvalidValue, "variant index out of range"));                \
    }                                                                                          \
  };                                                                                           \
  template <> struct Deserialize<Type> {                                                       \
    template <class D> static Result<Type, Error> run(D &d) {                                  \
      static const char *const kTags[] = {XPP_FOR_EACH(XPP_ENUM_TAG, _, __VA_ARGS__) nullptr}; \
      struct Visitor {                                                                         \
        Result<Type, Error> visit_variant(size_t tag_index, D &d) {                            \
          (void)tag_index;                                                                     \
          const char *tag_str = kTags[tag_index];                                              \
          XPP_FOR_EACH(XPP_ENUM_DE, Type, __VA_ARGS__)                                         \
          return err(error(ErrorKind::UnknownField, "unknown variant tag"));                   \
        }                                                                                      \
      };                                                                                       \
      return d.deserialize_variant(#Type, kTags, N, Visitor{});                                \
    }                                                                                          \
  };                                                                                           \
  }                                                                                            \
  }

/* ── Adjacent strategy ──
 *
 * Produces `{"tag_field":"tag_string","content_field":{...payload...}}`.
 *
 * The trick: tag_field and content_field are stored as local static
 * const char* in Serialize<Type>::run so the XPP_FOR_EACH'd emit macro
 * can reference them by name without needing a data-tuple unpack.
 * The alternative tuple (alt, tag) is unpacked via the 3-stage
 * EVAL3 + tuple pack/unpack pattern (same as Phase 3). */

#define XPP_ENUM_SER_ADJ(data, ...) XPP_ENUM_SER_ADJ_I((data, XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_ENUM_SER_ADJ_I(tuple)   XPP_ENUM_SER_ADJ_II tuple
#define XPP_ENUM_SER_ADJ_II(data, alt, tag)                                      \
  if (v.template is<alt>()) {                                                    \
    XPP_SERDE_TRY_VAR(scope, s.serialize_struct(#data, 2));                      \
    XPP_SERDE_TRY(scope.field(kTagField, xpp::String::from_utf8(tag).unwrap())); \
    XPP_SERDE_TRY(scope.field(kContentField, v.template get<alt>()));            \
    return scope.end();                                                          \
  } else

#define XPP_ENUM_DE_ADJ(data, ...) XPP_ENUM_DE_ADJ_I((data, XPP_SERDE_EVAL3(__VA_ARGS__)))
#define XPP_ENUM_DE_ADJ_I(tuple)   XPP_ENUM_DE_ADJ_II tuple
#define XPP_ENUM_DE_ADJ_II(data, alt, _tag)             \
  if (tag == _tag) {                                    \
    XPP_SERDE_TRY_VAR(v, m.template next_value<alt>()); \
    return ok(data(std::move(v)));                      \
  } else

#define XPP_ENUM_SERDE_ADJACENT(Type, tag_field, content_field, ...)                     \
  XPP_ENUM_SERDE_ADJACENT_(Type, tag_field, content_field, XPP_FIELD_COUNT(__VA_ARGS__), \
                           __VA_ARGS__)

#define XPP_ENUM_SERDE_ADJACENT_(Type, tag_field, content_field, N, ...)                    \
  namespace xpp {                                                                           \
  namespace serde {                                                                         \
  template <> struct Serialize<Type> {                                                      \
    template <class S> static Result<Void, Error> run(const Type &v, S &s) {                \
      static const char *const kTagField     = tag_field;                                   \
      static const char *const kContentField = content_field;                               \
      XPP_FOR_EACH(XPP_ENUM_SER_ADJ, Type, __VA_ARGS__)                                     \
      return err(error(ErrorKind::InvalidValue, "variant index out of range"));             \
    }                                                                                       \
  };                                                                                        \
  template <> struct Deserialize<Type> {                                                    \
    template <class D> static Result<Type, Error> run(D &d) {                               \
      static const char *const kFields[] = {tag_field, content_field, nullptr};             \
      struct Visitor {                                                                      \
        Result<Type, Error> visit_map(typename D::MapAccess &m) {                           \
          XPP_SERDE_TRY_VAR(k1, m.next_key());                                              \
          if (k1.is_none())                                                                 \
            return err(error(ErrorKind::MissingField, "missing tag field in variant"));     \
          XPP_SERDE_TRY_VAR(tag, m.template next_value<xpp::String>());                     \
          XPP_SERDE_TRY_VAR(k2, m.next_key());                                              \
          if (k2.is_none())                                                                 \
            return err(error(ErrorKind::MissingField, "missing content field in variant")); \
          XPP_FOR_EACH(XPP_ENUM_DE_ADJ, Type, __VA_ARGS__)                                  \
          XPP_SERDE_TRY(m.next_value_ignored());                                            \
          return err(error(ErrorKind::UnknownField, "unknown variant tag"));                \
        }                                                                                   \
      };                                                                                    \
      return d.deserialize_struct(#Type, kFields, 2, Visitor{});                            \
    }                                                                                       \
  };                                                                                        \
  }                                                                                         \
  }

#endif // XPP_SERDE_MACROS_H
