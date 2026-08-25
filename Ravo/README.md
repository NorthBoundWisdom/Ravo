# Ravo

Ravo 是当前仓库中唯一可构建的照片软件。当前产品目标是尽快交付一个跨平台第一版：
创建/打开本地 SQLite 图库、reference-only 导入 JPEG/PNG/TIFF/RAW，并在 Ravo Studio 中浏览图片。
现有 C++20 Engine 和 `ravo` CLI 是这个软件的底层与无 UI 客户端，desktop 不会另写一套业务或算法。

当前实现状态：

- 已完成 foundation/recipe/engine/adapters/CLI/test 骨架和版本化 JSON/错误契约；
- `ravo inspect` 可读取 LibRaw 支持的首个 16-bit Bayer RAW 切片；
- `ravo render` 可执行 nop 与 `ravo.core.exposure`，完成裁剪、black/white 归一化、camera WB、
  LibRaw camera-to-sRGB、基础 3×3 Bayer 插值、sRGB 编码和原子 PNG 输出；
- legacy XMP 仅支持空 history、严格 nop 基线和已证明的 schema-6/v5 手动 singleton exposure 子集；
- Catalog 纵切片已落地：reference-only JPEG/PNG/RAW 导入、库外 preview cache、
  `ravo_studio` Qt Quick 窗口，控件来自 source-root `GeoControls`。
- Browse & Review：catalog schema v2、评分/色标/拒绝、Gallery（grid/loupe）与 Edit 分栏、filmstrip（与 grid 相同的 contain 整图，letterbox 显示序号/评分/标记）、可折叠文件夹树、左侧 Import/Export、Fit/Fill/100%、筛选排序、右侧面板 RGB 直方图/parade 示波器。
- Basic Develop：catalog schema v4 每张图一份 canonical recipe，外加标签/可写 metadata 与
  持久 history/snapshot；CPU 含 RAW 高光重建（默认 opposed）、wavelets+Y0U0V0 降噪、
  lensfun poly3/vignette、dt UCS `colorequal`、渐变滤镜和 9 带 toneequal。Studio 有
  Edit 面板、before/after 与会话内 undo/redo。这些能力尚未完成交互预览缓存与
  Studio 保存/重开收口。macOS Debug 已跑通无 UI create/import/preview/recipe/reopen。
- RAW preview/export 以 `ravo.display.sigmoid` v1 作为唯一 Standard SDR 显示变换；
  recipe 可调 contrast/skew/hue preservation，默认基线不标记为用户编辑。
  Configure 强制要求 JPEG/GIF/WebP/TIFF imageformat plugins 与 QSQLITE driver；缺失直接阻断。

当前 legacy 迁移执行顺序见
[TODO_LEGACY_MIGRATION.md](../TODO_LEGACY_MIGRATION.md)，方向变化见
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
`CMakePresets.json`。打包运行时路径由 active lock 的
`RAVO_PACKAGE_RUNTIME_SEARCH_PATHS` 提供，模板只保存三平台示例。普通 Build/Test/Run 不隐式代跑
Config 或依赖更新。

macOS Debug：

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Windows 使用 `win_msvc_debug` / `win_msvc_release`，Linux 使用 `linux_clang_*`。FreeCM Config/Build/Run/Test
调用同一组 cmake preset 命令。`Ravo/tools/freecm_project.py` 与 `.ps1` 只是可选包装。

Release staged install：

```text
cmake --preset mac_clang_release
cmake --build --preset mac_clang_release
cmake --install build/mac_clang_release --prefix install/mac_clang_release
```

Release package with FreeCM runtime deployment:

```text
cmake --preset mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

The same target produces a Windows ZIP with `win_msvc_release` and a Linux
AppDir tar.gz with `linux_clang_release`. `RavoPackage` includes Ravo Studio,
the `ravo` CLI, Qt/QML runtime dependencies, and the license. Output paths and
CI artifact ownership are documented in
[Packaging](../DevDocs/Packaging.md).

FreeCM Package follows the active Config, so both Debug and Release Configs
have matching Package variants. Run Config before Package; tagged CI releases
always use Release.

仓库根 CMake 只构建 Ravo，不得配置、编译或运行冻结 0.9（`legacy/src/`）。
Windows/MSVC 与本机 macOS/Clang 曾验证当前 engine/CLI 图；Linux 仍需在目标主机验证。第一版增加
Qt Gui/Qml/Quick/Sql、QML modules、runtime plugins 和 desktop 后必须重新建立三平台结果。

## FreeCM 项目工作流

`configs/freecm.commands.jsonc` 使用 manifest v2。Debug/Release 和 Windows/macOS/Linux 是独立 Config；
Build、Run、Test、Package 显式绑定兼容 Config，不会暗中 Configure。Release Package 直接调用
`RavoPackage`，与 GitHub Actions 共用 FreeCM 部署路径。

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
ravo catalog create --path <library.sqlite> --json
ravo catalog import --catalog <library.sqlite> --input <file-or-folder> --json
ravo catalog list --catalog <library.sqlite> --json
ravo catalog preview --catalog <library.sqlite> --asset-id <id> --json
ravo catalog rate --catalog <library.sqlite> --asset-id <id> --rating 0-5 --json
ravo catalog develop --catalog <library.sqlite> --asset-id <id> --exposure-ev N --json
ravo catalog recipe --catalog <library.sqlite> --asset-id <id> --json
ravo catalog export --catalog <library.sqlite> --asset-id <id> --output <file> --format png|jpeg|tiff|original [--quality 90] --json
```

已有输出路径返回结构化 `conflict`，不会被隐式覆盖。catalog 命令调用与 Studio 相同的
services，作为无 UI 验收客户端。

## 名称与目录

| 名称/目录 | 用途 |
| --- | --- |
| Ravo Engine / `engine/` | RAW/raster、CPU preview/render、色彩和 operation |
| `ravo` / `cli/` | 正式 CLI 与机器 JSON 客户端 |
| `foundation/` | error、ID、取消和资源契约 |
| `recipe/` | versioned recipe/operation schema |
| `adapters/` | filesystem、codec、SQLite catalog、raster JPEG/PNG、preview cache |
| `domain/` | Asset/Catalog/Import/Preview 状态与 ports |
| `services/` | create/open/import/list/preview 用例 |
| Ravo Studio / `desktop/` | C++ presenter + Qt Quick/QML Gallery 与 viewer |
| `tests/` | unit、contract、catalog integration、fixture 和后续 desktop smoke |

Debug 构建后的 Studio 入口是
`build/mac_clang_debug/Ravo/desktop/ravo_studio.app`（Windows 为
`build/win_msvc_debug/Ravo/desktop/ravo_studio.exe`，Linux 为
`build/linux_clang_debug/Ravo/desktop/ravo_studio`）。可带
`--catalog <library.sqlite>` 直接打开已有图库。FreeCM 的 Run 与 GeoDebugger/DwgParser 一样：先
`cmake --build --preset … --target ravo_studio`，再直接启动这个 GUI。
第一版手工闭环：Create Library → Import `legacy/tests/0000-nop/expected.png` 与
`legacy/tests/images/mire1.cr2` → 选择资产 → Fit / 100%。

## 与冻结 `legacy/src/` 的关系

`legacy/src/` 是 0.9 行为的只读事实来源；Ravo 是唯一增长方向。Ravo 可以静态读取源码和 fixture，但生产
target 不得包含旧私有头、链接旧库、加载旧 IOP 或访问全局 `darktable`。冻结应用也不得增加 Ravo
adapter。只有 Ravo 满足根 TODO 的发行切换与回滚门槛后，才处理剩余旧应用。

## 文档入口

- [AGENTS.md](AGENTS.md)：Ravo 子树实施约束；
- [ARCHITECTURE.md](ARCHITECTURE.md)：target、数据、ownership 与线程边界；
- [MIGRATION.md](MIGRATION.md)：单向迁移、ledger 与退役规则；
- [TESTING.md](TESTING.md)：第一版 catalog/import/viewer 与冻结 fixture 验收；
- [ADR 索引](docs/adr/README.md)：长期架构决定；
- [根 legacy 迁移 TODO](../TODO_LEGACY_MIGRATION.md)：仅记录未完成执行项与门槛。

仓库整体采用 GPLv3，详见根目录 [LICENSE](../LICENSE)。
