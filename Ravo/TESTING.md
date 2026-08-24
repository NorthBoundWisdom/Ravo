# Ravo Testing Strategy

## 当前证据基线

冻结的 0.9 资产只用于静态取证：

- `darktable-tests/` 有 158 组 XMP + `expected.png` fixture 和 5 个原图；
- fixture 覆盖 68 个 operation 名；数字是资产盘点，不是 Ravo 覆盖率；
- 旧工程、旧 CLI、旧 CTest、旧打包 target 和 `darktable-tests/run` 全部禁止运行。

边界检查：

```text
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
```

当前 dependency-boundary checker 已覆盖 M1 target 图：`Qt6::Sql` 仅 adapters，`Qt6::Gui` 仅 raster
adapters/desktop，`Qt6::Qml`/`Qt6::Quick`、`QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts` 与
`GeoControls`/`GeoControls.AppShell` import 与 production `.qml` 仅 desktop；所有 Ravo target 禁止 Qt
Widgets。不能删除检查来放行新依赖。

当前 Ravo Debug 图有 16 个 `ravo-unit`、30 个 `ravo-contract` 与 4 个 `ravo-catalog` 测试。unit/contract
覆盖 foundation/recipe/executor、CLI JSON/退出码、有限 XMP 映射、真实 `mire1.cr2` inspect/render。
catalog 测试覆盖 schema create/reopen/newer-version reject、PNG/JPEG/RAW 幂等导入、目录跳过 sidecar、
原片哈希不变、preview 缓存和缺失/不支持输入。它们不替代本机 Studio 手工 Fit/100% 验收。

## Test framework 与 target 边界

所有 Ravo C++ unit、contract 和 integration test 使用 GoogleTest；port 交互需要时可使用 GoogleMock。
CMocka 只属于冻结 `src/tests`，不得链接 Ravo target。测试依赖从现有 FreeCM/CMake 工具链发现，不使用
`FetchContent` 或 CMake 网络下载。

测试 target 只能链接 Ravo 新 target。Qt Core/Gui/Qml/Quick/Sql 与 SQLite 进入第一版后，测试仍不得
包含 GTK、Qt Widgets、旧 `src` 头、旧数据库类型或动态 IOP；`QSqlDatabase` 和 `QImageReader` 只出现
在 adapter 实现与对应 contract test，QML source 和 Qt Quick Test 只出现在 desktop/test owner。

## 第一版测试层次

| 层次 | 目标 | M1 典型内容 |
| --- | --- | --- |
| Unit | 纯值类型/算法 | schema version、URI 规范化、asset ID、状态机、cache key |
| Port contract | 实现与抽象契约 | SQLite repository、filesystem、codec、preview cache |
| Service integration | 无 UI 真实用例 | create → import PNG/RAW → list → preview → reopen |
| Engine reference | 像素/metadata | RAW/raster 尺寸、orientation、颜色、有限值、有界输出 |
| Failure/recovery | 可信状态 | duplicate、unsupported、missing、取消、事务失败、cache 损坏 |
| Desktop acceptance | 最小产品闭环 | 创建/打开、导入、列表、选择、fit/100%、重启 |
| Resource/performance | 可交付性 | import-to-preview、峰值内存、长列表、关闭窗口、缓存预算 |
| Platform/package | 真实部署 | Windows/macOS/Linux configure/build，staged install 运行闭环 |

UI 测试不能替代 service integration。M1 可以使用最小手工桌面验收，但 catalog、import、preview 和失败
路径必须先有无 UI 自动测试。QML component 可使用 Qt Quick Test 验证 binding、intent 转发和状态呈现；
业务结果仍由 GoogleTest service/contract 测试验证。

## Catalog contract

SQLite adapter 至少测试：

- schema v1 create、空库 reopen、逐版本 migration 和 unknown-newer-version reject；
- transaction commit/rollback、foreign key、唯一 URI 和并发/串行连接 owner；
- duplicate import 幂等，失败 item 不产生 ready asset；
- 数据库只读、不可写、损坏、磁盘满/提交失败时保持可重新打开的可信状态；
- close 等待任务结束并释放 statement/connection；异常不穿过 target ABI。

测试使用临时目录中的独立数据库，不读取或覆盖用户 catalog。schema fixture 随 migration 版本提交，
不得通过直接改旧 fixture 伪造升级成功。

## Import 与原片安全

第一条 integration 同时覆盖仓库 PNG 与 `darktable-tests/images/mire1.cr2`。之后扩展 JPEG/TIFF、目录、
损坏文件和更多 RAW。

- 导入前后比较原片 hash/size/mtime，证明 reference-only 路径不修改原文件；
- 格式由 codec 探测确认，测试错误扩展名与 unsupported 内容；
- 每项结果区分 imported/duplicate/unsupported/failed，部分失败不丢明细；
- 目录枚举、排序和批次边界在确定性模式固定；
- 取消停止未派发工作，已提交可信资产保持有效；
- 源文件移走后 catalog 保留 missing 状态，viewer 不显示上一张图片冒充结果。

## Preview 与 viewer

- preview cache 写入临时文件后原子提交；已有可信文件不被失败请求覆盖；
- cache key 包含源指纹、尺寸和 contract version；损坏/缺失 cache 可重建；
- RAW 与 raster 共同验证 orientation、目标尺寸、alpha、颜色描述、NaN/Inf 和内存预算；
- 快速切换资产时，旧 request revision 的晚到结果被丢弃；
- 窗口关闭/关闭 catalog 后没有 detached task、晚到 UI 更新、未提交事务或临时 preview；
- viewer 手工验收至少覆盖 loading/ready/missing/unsupported/failed、fit、100% 与平移。

## 冻结 fixture 复用

- `tests/fixtures/fixture_classification_ledger.json` 继续与 legacy manifest 的 fixture ID 集合完全一致；
- 已提交 RAW、XMP、`expected.png` 是只读输入；Ravo 自有 float/golden、metadata 摘要和容差另存；
- Ravo CPU 与冻结资产比较像素、NaN/Inf、尺寸/ROI、alpha、颜色、metadata 和错误状态；
- 已删除产品能力只有在兼容性决定记录后才能排除，并测试可读的结构化拒绝；
- 一张 8-bit PNG 不能单独满足 operation 或颜色验收。

## 确定性模式

测试必须能固定：

- CPU backend、worker 数、调度和内存预算；
- catalog schema、URI normalization 和目录枚举顺序；
- preview 尺寸、orientation、插值、颜色与 metadata 策略；
- cache root、cache contract version 和源文件指纹输入；
- engine、recipe、operation 和第三方依赖版本；
- 随机种子（若算法需要）。

浮点允许有逐项记录的容差，但 geometry、orientation、operation 顺序、mask、离散状态和 catalog 事务
语义不能以浮点差异为理由变化。

## 本地标签与验证节奏

现有标签：

- `ravo-unit`：快速纯逻辑；
- `ravo-contract`：facade、adapter、CLI 和冻结边界。

随 M1 增加：

- `ravo-catalog`：schema/repository/service integration；
- `ravo-desktop-smoke`：可自动化的窗口生命周期与 composition smoke；不取代手工验收。

后续再建立 `ravo-regression`、`ravo-sanitizer` 和 `ravo-performance`。不存在的标签不得描述为已通过。
文档改动无需强行构建；CMake/依赖/公共头变化至少 configure/build；catalog/import/desktop 行为变化运行
相关标签和真实纵切片；公共调度或 schema 变化运行完整 Ravo test set。

每次依赖或公共构建图升级都按实际可用主机分别复验；其他平台的历史结果与本次未验证状态必须分开
报告，不能把单一平台写成全平台通过。
