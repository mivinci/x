# TODO: Router 查找从 O(n) 线性扫描升级为 segment 前缀树

> 来源：`libxpp/bench` 压测 + 路由实现审阅（2026-08-26）——`match_segments`
> 每请求全量扫 `m_routes` + 递归 `m_nested`
> 优先级：低——bench（3 路由）下无感；路由表变大后才会线性放大

## 动机

`Router::match_segments`（`libxpp/xpp/http/router.h:485`）对每个请求：

1. 线性扫描 `m_routes`，`segments_match` 逐 segment 比较（`:param` / 字面量）
2. 未命中时递归 `m_nested` 子树，每个子树再全量扫
3. `Router::compose`（`router.h:366`）每请求调用 `match`，所以查找成本 × 请求数

复杂度 **O(总路由数 × 平均 segment 数)**，且随 nest 深度叠加。bench 场景
（3 条路由）无感知，但大型路由表 + 高 QPS 时是明确的 CPU 热点，与
`todos/http-server-per-request-allocations.md` 的「compose 缓存」互为表里。

## 内容

- **segment 前缀树（radix tree / trie）**：按 path segment 建树，匹配
  O(路径段数)；对齐 Go httprouter / Rust matchit 的做法。
- **方法分组**：先按 method 过滤（GET/POST/...）再查树，缩小搜索空间。
- **保持语义**：
  - first-match-wins（先注册优先）
  - `:param` 捕获 + `Request::param()` / 类型注入
  - 路径匹配但方法不匹配 → 405（`path_matched` 单独记账）
  - `nest` / `fallback` / `sub_fallback` 传播
  - 查询字符串忽略（只匹配 path）

### 关键约束

当前匹配逻辑（`match_segments`）和 `bake_layers` 的 layer 折叠是解耦的——
layer 已烘焙进 `RouteEntry::invoke`，匹配只负责选 entry + 收 params。
所以替换匹配内核时**不需要动 layer 链**，只需保证 `Match{entry, params,
path_matched, sub_fallback}` 的产出一致。这是低风险改动的关键。

## 非目标

- 不改 `route`/`nest`/`merge`/`fallback`/`layer` 的公开 API
- 不引入第三方路由库
- 不做正则路由（`{id}` 风格）——保持 `:param` 语法

## 触发条件

- 路由表规模达到数百条，或 nest 深度 ≥3，且 profile 显示 `match_segments`
  占 CPU 可观比例
- 或用户在真实业务路由表上复现 QPS 随路由数下降

此时先做基准（固定路由表规模下 QPS 曲线），再动手替换匹配内核。
