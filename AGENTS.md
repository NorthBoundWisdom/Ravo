# Ravo Repository Instructions

## 项目定位

Ravo 是当前唯一可构建的产品：C++20 Engine、`ravo` CLI 与 Ravo Studio。仓库根
`CMakeLists.txt` 只构建 `Ravo/`。冻结的 Darktable 0.9 全部放在 `legacy/`，只允许静态阅读；
不得配置、编译、运行或修改。

`Ravo/` 由它自己的 `AGENTS.md` 增加约束；`FreeCM/` 是独立子模块。所有
`build/dependency_*` 内容都是父仓库工作流生成的外部源码，不得把它们当作本仓库源码修改。

## 工程执行与交付

- 默认 fail-fast：保留真实错误、结构化失败和明确的不支持状态，不用静默 fallback、空 catch、兼容
  开关或复制实现掩盖所有权错误。
- 自动生成内容只通过拥有它的脚本更新；发现生成产物错误时修改模板/生成器并重新生成，不把手改产物
  当作最终修复。
- 跨 target、依赖、数据库 schema、线程或产品范围的改动先给出简短计划，写清 owner、生命周期、错误/
  取消路径和最小验证集，再实施。
- 交付必须说明行为变化、关键取舍、实际运行的验证与结果、未验证平台，以及是否引入 fallback；没有
  运行的检查不得写成通过。
- 执行型 TODO 只保留未完成工作、风险、依赖、验证命令和完成门槛；长期结论同步到架构、ADR、代码或
  测试真相源。路线改变时同一变更清理过期引用，不保留两份互相冲突的计划。

## 开始工作前

1. 运行 `git branch --show-current` 和 `git status --short --branch`，识别当前分支与用户已有改动并保留它们。
2. 阅读与任务直接相关的源文件和文档，不根据上游 darktable 习惯猜测当前行为。
3. 产品删减先读 `TODO_REWRITE.md` 的“0.9 冻结基线与 Ravo 承接项”；GPU 任务先读
   `DevDocs/GPU_Baseline.md`。
4. 跨层改动先明确所有权、生命周期、线程边界和最小验证集。

现有行为以冻结的 `legacy/` 和 `legacy/tests` fixture 为准；`DevDocs/` 是 Ravo 开发文档，
`legacy/docs/` 是旧源码地图。新产品边界以 `TODO_REWRITE.md` 为准。除非用户明确重新开放 0.9，
任务不得修改 `legacy/`。

## Ravo 下一代边界

- 当前第一产品门槛是 C++20 Ravo Engine、正式 `ravo` CLI 与最小 Ravo Studio 共用同一服务层，尽快
  跑通 SQLite catalog、JPEG/PNG/TIFF/RAW reference-only 导入和图片查看；以 `TODO_REWRITE.md` 的
  M1–M3 为当前执行顺序。
- M1 已允许创建 `domain`、`services` 和 Qt 6 Quick/QML `desktop` target；`Qt6::Qml`/`Qt6::Quick` 及
  `QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts` import 只能由 desktop 及其测试使用，Qt Sql/
  QSQLITE 只能留在私有 SQLite adapter，Qt Gui 可在私有 raster adapter 中用于 `QImageReader`。首版不引入
  Qt Widgets 或第二套界面架构。QML 只能展示状态和
  转发意图；SQL、codec、图像算法、业务状态和任务 owner 必须留在 C++ domain/services/engine/adapters。
- Ravo 生产代码不得依赖 `legacy/src/` 私有头、旧库、动态 IOP、GTK 类型或全局 `darktable` 状态。验证只能
  静态读取冻结源码与 fixture；不得配置、编译或执行旧 CLI/旧测试工程。
- 迁移期间 Ravo 与冻结的 `legacy/src` 独立并行；不创建 `legacy/src` → Ravo 或 Ravo → `legacy/src`
  的生产依赖。只有 Ravo 全产品达到切换门槛后，才在 M7 退役阶段删除旧实现、构建项、资源、配置和重复测试。
- 在 `Ravo/` 工作前必须阅读 `Ravo/AGENTS.md`、`Ravo/ARCHITECTURE.md`、`Ravo/MIGRATION.md` 和
  `Ravo/TESTING.md`。

## 实施原则

- 本项目没有维护 darktable 历史插件、Lua、旧格式或旧 UI ABI 的默认义务；不要为
  已明确移除的能力增加空壳、兼容开关或迁移 shim。
- Ravo 决定不支持某项旧功能时，先记录显式拒绝或迁移策略，不在冻结的 0.9 中提前删除。M7
  退役旧 owner 时才同步清理构建项、注册表、资源、配置、文档和测试，并用全仓搜索确认无消费者。
- 不维护或修改现有 GTK 前端与旧 C/C++ 核心；发现差异或缺陷时记录为 oracle 限制，不用修补 `src`
  推进 Ravo。
- 0.9 OpenCL 保持冻结，不在旧应用内替换为 Metal；它只在 M7 随整个旧实现退役。Ravo GPU 按
  `TODO_REWRITE.md` M6 独立实现，不复用 OpenCL API。
- 遵循相邻 C/C++/CMake 文件的现有风格，只格式化触及的代码。不要借任务批量格式化
  遗留源码。
- 新依赖、公共 API、线程模型、数据库 schema 和产品范围变化都应在同一变更中记录
  设计理由与验证方法。
- 使用 `apply_patch` 做人工源码修改。生成器和格式化器仅用于它们明确拥有的输出。

## 现有 UI 与核心边界

- GTK 前端、dtgtk/Bauhaus 控件、Lighttable、Darkroom、导入、导出、catalog、history、masks、
  色彩空间和 pixelpipe 保留为只读旧实现；当前 M1 catalog/import/viewer 与后续 UI/服务工作只进入
  Ravo，不移植或修改这些旧 owner。
- 不在旧 UI 中增加入口、运行时、工具包、adapter 或 Ravo 调用。需要的行为只通过只读源码研究和
  已提交 fixture 取证，不运行独立旧进程。

## FreeCM 与依赖源码

- 本仓 `.gitmodules` 跟踪 `FreeCM/master`。需要刷新 FreeCM 时先确认主仓与子模块都没有无关改动，
  保持用户当前主分支并只从主仓根运行 `git submodule update --remote --checkout FreeCM`；不得新建 agent
  update branch，也不得在通常 detached 的子模块中执行 `git -C FreeCM pull`。gitlink 未变化就是安静的
  no-op；变化后先验证兼容性，未经用户明确要求不自动 commit 或 push。
- FreeCM gitlink 变化后的最小检查是
  `python3 -m repomgrcpp.tools.repo_tool check-lock-compat --repo-root .`、
  `python3 FreeCM/tools/validate_repo_commands.py .`、source-root 只读检查，以及当前平台 Ravo
  configure/build。不得盲目 stage 一个带 `+` 前缀且尚未解释的 submodule checkout。
- 当前 checkout 的 source-root 真相源首先是被忽略的活动锁 `source_roots.lock.jsonc`；开始依赖排查前
  先运行 `python configs/source_roots.py show --format json`、`resolve --format json` 和 `verify`，
  不得只根据 committed template 猜测当前实际路径或 mode。
- `source_roots.lock.jsonc.in` 是受版本控制的直接依赖 pinned 基线。永久依赖、公共 CMake 默认值或
  source-root API 变化只修改模板、`configs/source_roots.py` 和消费它们的 CMake 源码；根锁只声明
  direct dependencies，transitive closure 从本地 seed 中的依赖模板递归解析。
- `pinned` 使用精确 commit；`latest` 只解析 seed 中本地可见的最新提交；`manual` 对非空的
  `depsManualPath.<dependency>` 使用真实 checkout，空项回退到受管解析。`manual`/`latest` 与机器路径
  只属于活动锁，不得进入发布模板。
- 联调依赖时不得修改 `build/dependency_source_roots/*`。先把活动锁切到 `depsMode=manual`，将对应
  `depsManualPath` 指向开发者提供的真实 checkout（或经明确确认、干净且被当作真实仓库使用的 seed），
  再运行 `python configs/source_root_workflow.py --update` 并用 `show`/`resolve`/`verify` 确认接线。
- `python configs/source_root_workflow.py --init` 是唯一允许联网、克隆 seed 或准备远端资产的依赖步骤；
  `--update` 必须纯离线，只读取活动锁和现有 seed、解析/物化 closure、生成根 `CMakePresets.json`，
  不 fetch、不编译依赖，也不隐式配置或测试 Ravo。
- 日常 source-root 诊断默认只运行 `show`、`resolve`、`verify`、`graph`、`audit` 和
  `policy-check` 等只读命令。只有任务明确涉及首次准备、依赖刷新、锁切换或物化时，才运行会改变
  本地状态的 workflow：
  - `--refreshpin`：要求活动锁和模板已是 `pinned`，离线对齐 dependency commits；它不切 mode、
    不物化 roots，之后按需运行 `--update`；
  - `--pinlatest`：只从本地 seed 可见提交选择 latest 并执行离线 update，结果仍是本地候选，不能直接发布；
  - `--update`：按当前活动锁离线物化 source roots 并重新生成 host 文件；
  - `--cleanbuild --dry-run`：先列出保守清理目标；实际 `--cleanbuild` 只清构建输出，必须保留
    `build/dependency_seed_repos` 与 `build/dependency_source_roots`。
- 依赖整体刷新必须从干净主仓、干净 FreeCM 和干净 managed checkout 开始，并先保留原活动锁的私有
  快照；联网 `--init`、离线 `--pinlatest`、Ravo 快验、远端 SHA 确认、template 更新、仅把活动锁 mode
  显式切回 `pinned`、`--refreshpin`、`--update` 和再次验证按顺序完成。CLI `--pinlatest` 会把活动锁留在
  `latest`；候选失败时不修改 template，恢复原活动锁并用 `--update` 恢复物化状态后报告原始失败。
- `configs/freecm.commands.jsonc` 使用 manifest v2：Config 是显式活动上下文，Build/Test/Run/Package
  只绑定兼容 Config，不得隐式代跑 Configure。修改 manifest 后运行
  `python3 FreeCM/tools/validate_repo_commands.py .`。
- `source_roots.lock.jsonc`、`CMakePresets.json`、`.freecm/` 与 `build/dependency_*` 是本地状态，不提交，
  不手工修补为永久方案。活动锁或 manual path 变化后必须先 `--update`，再配置/编译 Ravo。
- 依赖仓提交必须先按拓扑顺序 push，并用 `git ls-remote <remote> <published-ref>` 确认已发布的
  branch/tag 返回目标 SHA，之后才能更新父仓库模板或 gitlink；禁止提交依赖尚未发布的本机 SHA。
- 不使用 `FetchContent`、CMake 网络下载、替代 submodule、源码复制或生成目录补丁绕过工作流。完整
  联调步骤见 `DevDocs/Dependency_Workflow.md`。

## 构建与验证

旧 0.9 构建、CTest、图像 runner 和打包 target 全部冻结，不再运行。仓库根 `CMakeLists.txt` 只构建 Ravo；
`legacy/` 是只读参考。FreeCM Config/Build/Run/Test 使用 `cmake --preset`，与 DwgParser/GeoDebugger
相同。

首次准备工作区时运行：

```text
git submodule update --init FreeCM
python configs/source_root_workflow.py --init
python configs/source_root_workflow.py --update
```

依赖已准备后：

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Windows 使用 `win_msvc_debug` / `win_msvc_release` preset；Linux 使用 `linux_clang_*`。
`Ravo/tools/freecm_project.py` 与 `.ps1` 只是同一组 cmake 命令的可选包装，不是 FreeCM 入口。

涉及跨平台构建、公共头或平台分支的改动，应在可用的 Windows、macOS 和 Linux 工具链上分别完成
Ravo configure/build；当前环境缺少相应工具链时，执行可行的静态检查并明确报告未验证的平台，不能
把单一平台结果表述为全平台通过。测试只运行 Ravo 自有 unit/contract 入口；不得运行旧 CTest、旧 CLI
或 `legacy/tests/run`。

验证应与风险成比例：

- 纯 Markdown/代理说明：检查链接、命令、路径与 diff；无需强行完整编译。
- CMake/依赖图：至少运行离线 `--update`、Ravo configure 和受影响 target 的 build。
- C/C++ 核心：构建受影响 Ravo target；任务要求行为验证时运行相关 Ravo unit/contract，公共头或广泛
  改动运行完整 Ravo 测试集。
- CLI/服务：验证结构化错误、取消、输出冲突和资源销毁路径，不启动旧进程作即时 oracle。
- Catalog/import/desktop：验证 schema create/reopen/migrate、事务失败、重复导入、损坏/缺失文件、取消、
  preview 原子缓存、晚到结果丢弃和窗口关闭后的资源销毁；UI 手工结果不能替代 service contract。
- GPU/图像算法：除 Ravo 单元/fixture 验证外，遵循 `DevDocs/GPU_Baseline.md` 的 CPU 金样和性能门槛。

如果环境缺依赖或测试数据，应先做所有仍可执行的静态/局部验证，并准确报告未运行项
及原因；不要把“未运行”写成“通过”。

## 文档与生成文件

- 架构、构建命令、依赖约定或产品范围变化时，同步更新 `README.md`、相关根级文档和
  `DevDocs/` 索引。
- 不编辑生成的 `CMakePresets.json` 作为最终修复；修改锁模板/生成逻辑后运行 `--update`。
- 不提交构建树、临时报告、本地活动锁、IDE 文件或依赖 checkout。
- 链接必须指向仓库中真实存在的文件；删除/重命名文档时用全仓搜索修复引用。

## Git 与交付

- 用户要求提交（含“提交全部 diff”）时必须执行 `review-and-commit` skill：
  Grok 读 `.grok/skills/review-and-commit/SKILL.md`，Codex 读
  `.codex/skills/review-and-commit/SKILL.md`。不要只根据主仓 `git status` 提交。
- 若本轮改了 seed / source-root（当前为 GeoControls、LibRaw），必须先在该依赖仓
  提交并 push 到其 remote 默认分支，用 `git ls-remote` 确认 SHA 已在远端，再把
  `source_roots.lock.jsonc.in` 钉到该 SHA，最后才提交主仓。禁止先提交主仓、把
  seed 留在本机，也禁止把未发布 SHA 写入模板。依赖仓 push 属于该提交流程；主仓
  仍默认不自动 push。
- 提交钩子实现属于 `FreeCM/hooks/`。本仓只拥有 `hooks/path.ini.sample`、
  `hooks/install.py` 与 `hooks/README.md`。
- 首次克隆后创建被忽略的 `hooks/path.ini` 并运行 `python3 hooks/install.py`。
  pre-commit 只对 `Ravo/` 下暂存 C/C++ 跑 clang-format，配置了 qmlformat 时才格式化
  QML/JS；`legacy/` 与 `FreeCM/` 不在格式化范围内。
- 提交说明使用 `[type]: description`。合法 type 见 `hooks/README.md`。
- 钩子对任务范围内文件的纯格式化改动视为预期输出，不是外部脏改或阻断；重新暂存后继续。
- 未经明确要求不要提交、amend、rebase 或 push 主仓。
- 提交前检查 `git diff --check`、完整 diff 和 `git diff --cached`；不要意外提交本地锁、
  预设、构建产物、密钥或绝对个人路径。
- 一个提交应表达一个可回退意图。
- 交付说明列出结果、主要文件、实际运行的验证和仍存在的风险，不把计划中的功能说成
  已实现。

## 仓库级技能

- Grok：`.grok/skills/`（会自动发现并按 description 调用）。
- Codex：`.codex/skills/`（`$build-repo`、`$review-and-commit`、`$context-handoff`）。
- 两套 skill 正文应对齐。调用时仍以本文件和更深层 `AGENTS.md` 为最高仓库约束。
