# ADR-0001: C++20 headless engine and CLI first

- Status: Partially superseded by ADR-0007
- Date: 2026-07-21

ADR-0007 replaces the rule that catalog/services/desktop must wait for the
complete headless exit. The C++20 engine, supported CLI, CPU reference path,
and prohibition on wrapping the frozen core remain in force.

## Context

0.9 将 GTK、IOP 生命周期、pixelpipe、数据库、任务和 OpenCL 类型混在同一编译图。现有算法与关键
依赖主要是 C/C++，而真正有价值的回归资产是 CLI 图像金样，不是足够覆盖重写的细粒度 UT。

同时改写架构、全部数值算法和语言会把数值一致性、FFI、所有权、构建及 UI 风险叠加在一起。桌面 UI
也不应成为验证新图像内核的前提。

## Decision

- Ravo 第一方实现统一使用 C++20、CMake 与 FreeCM；首个可交付版本不加入 Rust/Cargo。
- 第一产品是无 UI 的 Ravo Engine 和正式 `ravo` CLI。
- CPU 是参考实现；旧图像 fixture 通过 legacy XMP adapter 和 CLI 差分复用。
- 原决定要求 catalog、services 和 Ravo Studio 等到无头阶段验收后开始；该排期已被
  ADR-0007 取代，M1 现在允许最小纵切片并继续复用同一 engine/CLI 契约。
- 未来 UI 直接调用 engine/services API，不启动 CLI 子进程；CLI 继续作为受支持批处理工具。

## Consequences

- 可以在保留数值语义的同时重建所有权、线程、错误和数据契约。
- 第一阶段没有 GUI 演示，但可以更早获得可自动验证、可脚本使用的真实产品。
- C++ 不自动消除内存/并发问题，因此必须使用 RAII、明确 owner、sanitizer 和严格依赖边界。
- UI 框架选择延后，不影响 engine 进度。

## Rejected alternatives

- **纯 Rust 首版**：长期安全性有吸引力，但会同时引入算法再实现、第三方 FFI 和双构建体系风险。
- **Rust 上层 + C++ engine 同时开工**：过早冻结 FFI，并增加两套工具链与所有权协议。
- **先重写 UI**：无法解决 engine 耦合，也不能利用现有无头图像回归作为主验收入口。
- **把旧核心包成新库**：会保留 GTK/IOP/global state 泄漏，不构成 clean-slate 架构。
