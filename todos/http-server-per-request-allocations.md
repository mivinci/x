# TODO: xpp HTTP server 每请求堆分配优化

> 来源：`libxpp/bench` 压测（2026-08-26）——xpp echo server 单核 77.9k QPS，但
> 已赢 Go 多核（72.9k @ 240% CPU），分配开销不是当前瓶颈
> 优先级：低——正确性优先；在出现下述"触发条件"时再做

## 动机

`srv_on_request_cb`（`libxpp/xpp/http/server.h`）对**每个请求**固定执行多次堆分配：

1. `sync::mpsc::channel<Bytes>(256)` —— 每请求建一条 body channel
2. `Own<ReqState>(new ReqState())` —— per-request 状态
3. `impl->m_reqs.emplace(ctx_key, ...)` —— map 插入（`Own` 转移）
4. `Router::compose(req)` —— **每次请求重新做路由匹配 + middleware 链构建**
5. `Arc<Request>::make(std::move(req))` —— 请求本体搬上堆
6. `Arc<ConnLifetime>::make()` + `m_conns.emplace(...)` —— UAF 修复引入的连接生命周期
7. `xpp::spawn(...)` —— defer node 堆分配

78k QPS ≈ 每秒 ~80 万次分配。压测结论（`libxpp/bench/http/README.md`）显示
当前单核效率已碾压 Go（等量 QPS 下核用量约 Go 的 1/2.6），分配开销**尚未**成为
吞吐瓶颈——但它是单核 QPS 上限（~78k）的构成因素之一，且会随路由/middleware
复杂度线性放大。

## 内容

- **`Router::compose` 结果缓存**：静态路由表 + 固定 middleware 链在 server 构建时
  预组合成最终 endpoint，请求到达时免匹配直接调用。需处理动态部分（path params、
  按请求状态变化的 layer）——只缓存结构，参数注入保留在调用时。
- **固定尺寸对象池**：`ReqState`、`ConnLifetime`（若保留独立对象）尺寸已知，可用
  xbase slab 池化，消除每请求 `new`/Arc 分配。
- **请求路径分配盘点**：用 `malloc` 追踪/采样量化每请求实际分配次数与字节数，
  找出大头，避免凭感觉优化。mpsc channel 是否可复用、`Arc<Request>` 是否可用
  更轻的持有方式（如 arena）逐一评估。

### 与单核瓶颈的关系

单核 ~78k 是**单事件循环**架构上限，需多进程/多 loop 才能突破——与本项正交。
本项只压低单核内的每请求成本，不改变"单核有上限"这一事实。

## 非目标

- 不改单事件循环架构（多进程/多 loop 是独立的扩展方向，另立 todo）
- 不动 handler 层 API（`route`/`layer`/`compose` 语义保持）
- 不为优化牺牲正确性（尤其 ConnLifetime 的连接关闭防护）
- 不引入第三方内存管理库

## 触发条件

- 单机吞吐真实需求逼近单核上限（>70k QPS），且实测确认分配占 CPU 大头
- 路由表 / middleware 链变大后 QPS 明显随复杂度下降
- 或有用户报告高 QPS 下 CPU profile 显示 `srv_on_request_cb` 分配路径为热点

此时先做"分配盘点"（内容第 3 项）确定方向，再动手缓存/池化。
