# Ravo Repository Instructions

本文件适用于 `Ravo/` 整个子树，并补充根目录 `AGENTS.md`。两者冲突时，以更严格且更具体的约束为准。

## 开工前

1. 运行 `git status --short --branch`，保留用户已有改动。
2. 阅读 `Ravo/README.md`、`ARCHITECTURE.md`、`MIGRATION.md`、`TESTING.md` 和相关 ADR。
3. 涉及旧行为或算法时，阅读对应 `legacy/src/` 实现和 fixture；不得根据上游 darktable 习惯猜测。
4. 确认本次工作属于 `../TODO_REWRITE.md` 当前 M1–M3 第一版纵切片。现在允许创建
   `domain/services/desktop` target；desktop 明确使用 Qt 6 Quick/QML，Qt Sql 只进入私有 SQLite adapter。
   Qt Widgets、第二套 presentation 架构和旧 GTK adapter 仍未获准。
5. 写明所有权、生命周期、线程边界、错误/取消路径和最小验证集后再跨层修改。

## 当前技术边界

- 第一方新代码统一使用 C++20、CMake 与 FreeCM；不加入 Rust/Cargo 构建图。
- CPU 是正确性参考和可靠回退。GPU 只能在 M6、第一版软件和对应 CPU 编辑路径门槛完成后作为 adapter
  引入。
- `ravo` CLI 与 Ravo Studio 都是正式客户端；算法必须在 engine 中，catalog/import/preview 编排必须在
  services 中，CLI/UI 只负责输入输出、进度、选择和错误呈现。
- operation 首版为内建注册，不恢复旧动态 IOP ABI、GTK ABI 或插件兼容层。
- recipe、operation ID、参数 schema 和机器 JSON 必须版本化；不得序列化对象内存布局或 UI 状态。

## 依赖规则

- 第三方 source-root 状态由仓库根活动锁管理。修改或排查依赖前先按
  `../DevDocs/Dependency_Workflow.md` 运行 `show`、`resolve` 和 `verify`；不得只看模板或直接修改
  `../build/dependency_source_roots/*`。
- 本地依赖联调只在被忽略的 `../source_roots.lock.jsonc` 中使用 `depsMode=manual` 与对应
  `depsManualPath`，运行根 `configs/source_root_workflow.py --update` 确认接线后再 configure/build。
  依赖提交必须先发布并验证远端 SHA，之后才能更新 tracked template。
- `foundation` 不依赖 recipe、engine、CLI、catalog、UI 或平台实现。
- `recipe` 只依赖 foundation；它不知道像素执行器、数据库或 UI。
- `engine` 依赖 foundation/recipe 及自己声明的端口；可以直接使用 QtCore，其他第三方裸类型优先留在
  私有 adapter。
- `domain` 依赖 foundation，拥有 Asset/Catalog/Import/Preview 状态和 repository port；不知道 SQLite、
  codec、QML/presentation 类型或 engine 私有类型。
- `services` 依赖 domain 与 engine facade，拥有 create/open/import/list/preview 用例和任务编排；不得
  发 SQL 或持有 QML object/presentation model。
- `adapters` 实现 SQLite、filesystem、RAW/raster codec 和 preview cache port；`QSqlDatabase`、
  `QImageReader` 与其他第三方句柄保持私有。
- `cli` 依赖 services/engine facade 和 adapter composition；不得包含算法源码、SQL 或 UI。
- `desktop` 由 C++ composition root、desktop-owned QObject presenter/model 和 QML views 组成，只依赖
  services 与只读 preview 资源契约；QML/JavaScript 只做展示、绑定和输入转发，不得直接访问数据库、
  codec、engine 私有状态或拥有业务规则。
- Ravo 生产代码不得包含 `legacy/src/` 头、链接旧库、`dlopen` 旧模块或读取旧全局状态。测试只能读取已冻结
  fixture 与源码；不得配置、编译或运行旧 CLI、旧 CTest 或 `legacy/tests/run`。
- 冻结的旧应用不复用 Ravo，也不增加 adapter；生产依赖必须保持完全独立，直到 Ravo 达到发行切换
  门槛后整体退役旧应用。

## C++ 实施规则

- 使用值语义、不可变快照、RAII 和明确 owner；拥有资源的裸指针不得跨公开边界。
- view/span/string_view 必须有可证明且写入接口文档的生命周期。
- 异步工作使用受 owner 管理的执行器、任务句柄与取消令牌；禁止 detached thread。
- 错误使用可检查结果和结构化错误；异常不得穿过 target ABI、C 回调、任务或未来 FFI 边界。
- 不引入等价于全局 `darktable` 的可写服务集合，也不以 singleton 绕过依赖注入。
- 只格式化触及的代码；公共 API、依赖、线程或数据格式变化必须同时更新 ADR/架构和验证说明。
  已安装的仓库提交钩子会格式化 `Ravo/` 下暂存的 C/C++（以及已配置 qmlformat 时的
  QML/JS）；不要再手改同一批纯格式差异，也不要格式化 `legacy/`。

## 算法迁移

- 迁移单位是可由 CLI 观察的 capability/operation 批次，不是目录或行数。
- 直接阅读冻结 C 源码并重写 C++20 行为；用聚焦 Ravo UT 覆盖旧实现的关键分支，但不得编译或运行旧 UT。
- 可以移植可理解的数学思想或纯算法，但不得连带复制 GUI、旧 module lifecycle、配置 shim、
  动态注册、OpenCL 类型或无消费者代码。
- Ravo 通过完整产品验收并完成发行切换后，才在 M7 删除 `src` 所有权。删除必须覆盖 CMake、
  资源、配置、文档与测试，并以全仓搜索证明没有可达消费者；迁移期间不逐项修改冻结的旧实现。
- 旧 OpenCL 保持冻结并随 0.9 整体退役；Ravo GPU 只能在自身 CPU 路径验收后以独立 adapter 实现。

## 验证与交付

- Windows 上的新工程辅助工具只使用 Python 或 PowerShell；不得引入旧应用 runner。CMake/MSVC 只配置
  和编译 `Ravo/` target。

- 纯文档：检查真实路径、相对链接、命令、术语、diff 和 `git diff --check`。
- 新 C++ 单元：只链接 Ravo target，并运行相关 unit/contract 标签和 sanitizer 可行集。
- Catalog/import：覆盖 schema create/reopen/migrate、URI 幂等、事务回滚、部分失败、取消、源文件消失、
  preview cache 原子提交和资源销毁；原片测试前后哈希必须一致。
- 新 target 落地时同步更新 `tools/check_ravo_dependency_boundary.py`：扫描 CMake target 与 QML imports；
  `Qt6::Sql` 仅 adapters 可用，`Qt6::Gui` 仅 raster adapters/desktop 可用，`Qt6::Qml`/`Qt6::Quick`、
  `QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts` import 及 production `.qml` 仅 desktop 可用，
  `Qt6::QuickTest` 和 QML test 仅 desktop test target 可用；所有 Ravo target 禁止 Qt Widgets，并继续
  拒绝冻结 `src` 与 GTK 依赖。
- Desktop：先用 service integration test 验证业务，再做创建/打开、导入、选择、fit/100% 的最小手工验收；
  Qt Quick Test 和 UI smoke 不得替代 domain/service contract。
- operation：运行参数/schema、合成边界、旧 XMP 映射，并把 Ravo 输出与已提交 RAW/PNG/metadata
  fixture 比较；不启动旧进程生成即时差分。
- 公共头或调度广泛改动：运行完整 Ravo unit/contract、保留 fixture 和跨平台可行构建。
- 不把未运行测试写成通过，不把 macOS 构建写成全平台通过。
- 未经明确要求，不提交、amend、rebase 或 push 主仓。含 seed 改动的提交必须走根目录
  `review-and-commit` skill：先 push 依赖仓并钉已发布 SHA，再提交主仓。

Ravo 的 Windows/MSVC 构建命令和 FreeCM 项目命令分别记录在 `README.md` 与
`../configs/freecm.commands.jsonc`；这些入口只能配置、构建、运行和测试 Ravo。
