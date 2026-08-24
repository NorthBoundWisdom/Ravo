# Ravo

Ravo 是 DarkTableNext 仓库中的下一代照片软件。当前产品目标是尽快交付一个跨平台第一版：
创建/打开本地 SQLite 图库、reference-only 导入 JPEG/PNG/TIFF/RAW，并在 Ravo Studio 中浏览图片。
现有 C++20 Engine 和 `ravo` CLI 是这个软件的底层与无 UI 客户端，desktop 不会另写一套业务或算法。

当前实现状态：

- 已完成 foundation/recipe/engine/adapters/CLI/test 骨架和版本化 JSON/错误契约；
- `ravo inspect` 可读取 LibRaw 支持的首个 16-bit Bayer RAW 切片；
- `ravo render` 可执行 nop 与 `ravo.core.exposure`，完成裁剪、black/white 归一化、camera WB、
  LibRaw camera-to-sRGB、基础 3×3 Bayer 插值、sRGB 编码和原子 PNG 输出；
- legacy XMP 仅支持空 history、严格 nop 基线和已证明的 schema-6/v5 手动 singleton exposure 子集；
- SQLite catalog、import services、raster codec、preview cache 和 Ravo Studio 尚未实现，是当前 M1 工作。

完整执行顺序见 [TODO_REWRITE.md](../TODO_REWRITE.md)，方向变化见
[ADR-0007](docs/adr/0007-first-usable-catalog-viewer.md)。

## 第一版闭环

第一版必须完成：

1. 在 Ravo Studio 创建或打开图库数据库；
2. 导入本地文件/目录，至少用一张 PNG 和真实 `mire1.cr2` 贯穿测试；
3. Gallery 显示资产，选择后可适应窗口、100% 和平移查看；
4. 重启后重新打开同一 catalog 并查看；
5. 重复、损坏、缺失、不可写和取消具有可见、可恢复的结构化结果；
6. 原片始终只读，preview 是数据库外可重建的原子缓存。

第一版桌面采用 Qt 6 Quick/QML：C++20 composition/presenter 持有 services、任务和资源，QML 只负责
布局、展示、绑定与输入。SQLite 由私有 QSQLITE adapter 持有，JPEG/PNG 首版由私有 `QImageReader`
adapter 解码。UI 只消费 presenter 暴露的 service state 和只读 preview；SQL、codec、RAW 处理、任务和
缓存不进入 QML。首版不链接 Qt Widgets，也不维护 Widgets fallback。

## 构建与测试

先从仓库根检查本机活动 source-root 状态：

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

首次准备工作区或活动锁变化后，按授权运行：

```text
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

`--init` 是唯一允许联网的依赖动作；`--update` 离线物化 source roots 并生成根
`CMakePresets.json`。普通 Build/Test/Run 不隐式代跑 Config 或依赖更新。

Windows/MSVC：

```powershell
& .\Ravo\tools\freecm_project.ps1 -Action Configure -Configuration Debug
& .\Ravo\tools\freecm_project.ps1 -Action Build -Configuration Debug
& .\Ravo\tools\freecm_project.ps1 -Action Test -Configuration Debug
```

macOS/Linux：

```text
python3 Ravo/tools/freecm_project.py --action Configure --configuration Debug
python3 Ravo/tools/freecm_project.py --action Build --configuration Debug
python3 Ravo/tools/freecm_project.py --action Test --configuration Debug
```

Release staged install：

```text
python3 Ravo/tools/freecm_project.py --action Configure --configuration Release
python3 Ravo/tools/freecm_project.py --action Install --configuration Release
```

Windows 使用对应 PowerShell 入口。当前项目脚本只操作 `Ravo/`，不得配置、编译或运行冻结 0.9。
Windows/MSVC 与本机 macOS/Clang 曾验证当前 engine/CLI 图；Linux 仍需在目标主机验证。第一版增加
Qt Gui/Qml/Quick/Sql、QML modules、runtime plugins 和 desktop 后必须重新建立三平台结果。

## FreeCM 项目工作流

`configs/freecm.commands.jsonc` 使用 manifest v2。Debug/Release 和 Windows/macOS/Linux 是独立 Config；
Build、Run、Test、Package 显式绑定兼容 Config，不会暗中 Configure。

维护动作：

```text
python3 configs/source_root_workflow.py --refreshpin
python3 configs/source_root_workflow.py --pinlatest
python3 configs/source_root_workflow.py --update
python3 configs/source_root_workflow.py --cleanbuild --dry-run
```

`--pinlatest` 只使用本地 seed 可见提交，并把活动锁留在 `latest`；它是依赖刷新候选而非发布基线。
`--cleanbuild` 保留 `build/dependency_seed_repos` 与 `build/dependency_source_roots`。完整授权边界、
FreeCM gitlink 更新、manual 联调和发布顺序见
[Dependency Workflow](../DevDocs/Dependency_Workflow.md)。

## CLI 当前能力

```text
ravo inspect <input> --json
ravo recipe import-xmp <legacy.xmp> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe validate <recipe> --json
ravo render <input> --recipe <recipe> --output <png> --backend cpu [--width N] [--height N] --json
```

已有输出路径返回结构化 `conflict`，不会被隐式覆盖。M2 计划让 CLI 增加 catalog
create/import/list/preview 命令，作为同一 services 的无 UI 验收客户端。

## 名称与目录

| 名称/目录 | 用途 |
| --- | --- |
| Ravo Engine / `engine/` | RAW/raster、CPU preview/render、色彩和 operation |
| `ravo` / `cli/` | 正式 CLI 与机器 JSON 客户端 |
| `foundation/` | error、ID、取消和资源契约 |
| `recipe/` | versioned recipe/operation schema |
| `adapters/` | filesystem、codec；M1 增加 SQLite 与 preview cache |
| `domain/`（M1） | Asset/Catalog/Import/Preview 状态与 ports |
| `services/`（M1） | create/open/import/list/preview 用例 |
| Ravo Studio / `desktop/`（M1） | C++ presenter + Qt Quick/QML Gallery 与 viewer |
| `tests/` | unit、contract、integration、fixture 和后续 desktop smoke |

M1 新目录在实际 target 落地时创建；本次规划不把计划中的 target 描述为已实现。

## 与冻结 `src/` 的关系

`src/` 是 0.9 行为的只读事实来源；Ravo 是唯一增长方向。Ravo 可以静态读取源码和 fixture，但生产
target 不得包含旧私有头、链接旧库、加载旧 IOP 或访问全局 `darktable`。冻结应用也不得增加 Ravo
adapter。只有 Ravo 全产品满足发行切换与回滚门槛后，才在 M7 整体删除旧应用。

## 文档入口

- [AGENTS.md](AGENTS.md)：Ravo 子树实施约束；
- [ARCHITECTURE.md](ARCHITECTURE.md)：target、数据、ownership 与线程边界；
- [MIGRATION.md](MIGRATION.md)：单向迁移、ledger 与退役规则；
- [TESTING.md](TESTING.md)：第一版 catalog/import/viewer 与冻结 fixture 验收；
- [ADR 索引](docs/adr/README.md)：长期架构决定；
- [根执行计划](../TODO_REWRITE.md)：M0–M7 路线图。

仓库整体采用 GPLv3，详见根目录 [LICENSE](../LICENSE)。
