# Ravo 第一版可用软件执行计划

> 状态（2026-08-24）：这是当前唯一执行路线图。它以尽快交付“创建图库数据库 → 导入本地图片
> （包含 RAW）→ 浏览图片”的真实桌面纵切片为第一优先级，取代此前“阶段 3 无头验收完成后才开始
> catalog/UI”的排期。既有 Phase 0 文档继续作为历史取证与契约来源，不再决定当前实施顺序。
>
> 已实现基线仍然有效：C++20 foundation/recipe/engine/CLI、版本化 JSON、有限 legacy XMP 映射、
> LibRaw 16-bit Bayer RAW inspect/render、CPU exposure、原子 PNG 输出和 39 项 unit/contract 测试。
> 这些能力是第一版软件的底层，不代表数据库、导入服务或桌面浏览器已经实现。

## 第一版产品结果

第一版必须让用户在一个独立 Ravo 应用中完成以下闭环：

1. 启动 Ravo Studio，创建新的本地图库数据库，或重新打开已有数据库。
2. 选择一个或多个本地文件，或选择目录导入；首版支持 JPEG、PNG、TIFF 和 LibRaw 可解码 RAW，
   验收至少覆盖仓库内一张 PNG 与真实 `mire1.cr2`。
3. 导入过程登记文件、读取基础元数据并生成有界预览；原文件保持只读，不复制、移动、改名或删除。
4. Gallery 显示已导入资产及加载/失败状态；选择资产后能查看图片，至少支持适应窗口、100% 和平移。
5. 关闭并重新打开应用后，同一数据库仍能列出资产并重新显示预览。
6. 损坏文件、重复导入、文件消失、数据库不可写和取消都返回可见、可恢复的错误，不产生伪成功记录。

第一版明确不包含：

- 非破坏编辑工作流、完整旧 XMP/history 重放、mask/blend、styles、批量导出或完整 IOP 迁移；
- 旧 catalog 数据库迁移、托管原片复制、网络同步、云服务、移动端或插件 ABI；
- GPU/Metal/OpenCL 加速；第一版预览以 Ravo CPU 路径为正确性参考；
- 对冻结 0.9 GTK 界面的复刻。Ravo Studio 只实现完成上述闭环所需的最小桌面交互。

## 已确认的技术与产品决策

- Ravo 第一方代码继续使用 C++20、CMake 与 FreeCM；冻结的 `legacy-0.9/` 不进入新生产链接图。
- 第一版桌面明确采用 Qt 6 Quick/QML，C++20 负责 composition、application services、presentation model
  和资源生命周期，QML 负责布局、展示、绑定与输入。desktop 链接 `Qt6::Qml`/`Qt6::Quick`，首版 QML
  import allowlist 是 `QtQuick`、`QtQuick.Controls`、`QtQuick.Dialogs` 与 `QtQuick.Layouts`；不引入 Qt
  Widgets 或 Widgets/QML 混合架构。raster 解码仍可在私有 adapter 使用 Qt Gui，SQLite 仍只在私有
  adapter 使用 Qt Sql；业务状态与图像算法不得进入 QML/JavaScript。
- SQLite 通过私有 Qt Sql/QSQLITE adapter 使用；公开 domain/services 契约不暴露 `QSqlDatabase`、
  SQL 文本、Qt model 索引或数据库行地址。连接名、线程归属与销毁顺序由 adapter 明确管理。
- 导入首版为 reference-only：数据库保存规范化本地 URI 和必要指纹，原片仍由用户管理且始终只读。
- JPEG/PNG 首版通过包装 `QImageReader` 的 raster codec adapter 解码；TIFF 在 M2 以同一 port 接入并验证
  实际插件/codec 部署。RAW 复用 Ravo Engine 的 LibRaw CPU 路径。codec 裸类型不得泄漏到 domain、
  services 或 UI。
- 预览是可重建缓存，不是 catalog 真相源。数据库只保存 cache key、状态和版本；图像字节原子写入
  数据库外的受控缓存目录。
- `ravo` CLI 继续是受支持客户端和无 UI 验收入口。CLI 与 Ravo Studio 必须调用同一
  application services/engine facade，不维护两套导入、catalog 或预览逻辑。
- catalog schema、迁移、machine JSON、recipe 和 operation schema 全部显式版本化；未知的新版本
  fail-fast，不猜测兼容性。
- 所有运行时能力离线工作；缺少网络不能触发下载、登录或静默降级。

方向变化由 [ADR-0007](Ravo/docs/adr/0007-first-usable-catalog-viewer.md) 记录；它仅替代旧 ADR 中
“desktop/catalog 必须等待完整无头出口”的排期，继续保留独立 engine、CLI、CPU 参考路径和单向迁移。

## 目标架构与依赖方向

```text
ravo CLI ───────────────┐
                        │
Ravo Studio ────────────┴──▶ ravo_services
                               │ create/open catalog
                               │ import files/directories
                               │ list assets
                               │ request/cancel preview
                               ▼
                       ravo_domain + ravo_engine
                           ▲              ▲
                           │ implements   │ implements
                    SQLite/FS adapter   RAW/raster/preview adapter
```

| Target | 第一版所有权 | 允许依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| `ravo_foundation` | error、ID、取消、资源契约 | 标准库、按需 QtCore | catalog、engine、UI |
| `ravo_recipe` | recipe 与 operation schema | foundation、按需 QtCore | 数据库、codec、UI |
| `ravo_engine` | inspect、CPU RAW/raster 处理、preview render | foundation、recipe、engine ports | catalog、CLI、UI、旧 `src` |
| `ravo_domain` | Asset、Catalog、Import/Preview 状态、repository port | foundation | SQLite、codec、QML/presentation 类型 |
| `ravo_services` | create/open、import、list、preview 用例与任务编排 | domain、engine facade | SQL、QML/presentation 类型、codec 裸类型 |
| `ravo_adapters` | SQLite、filesystem、RAW/raster codec、preview cache | 对应 ports、Qt Core/Gui/Sql、固定第三方依赖 | UI 状态、旧核心 |
| `ravo_cli` | 参数、JSON、退出码、composition | services、engine、adapters | 算法、SQL、UI |
| `ravo_desktop` | C++ composition/presenter、QML 窗口、Gallery、viewer、文件选择和状态呈现 | services、只读 preview 资源、Qt Core/Gui/Qml/Quick | Qt Widgets、SQL、codec、算法私有状态 |

composition root 创建数据库 adapter、codec、engine、service、任务执行器和 UI，并在任务全部停止后按
反向顺序销毁。UI 主线程只提交意图与显示不可变快照；扫描、metadata、decode、preview 和数据库 I/O
在 owner 管理的任务中执行，不使用 detached thread。每个结果携带 catalog/asset/request revision，
取消或被新选择替代的旧结果直接丢弃。

## 第一版数据与服务契约

### Catalog schema v1

第一版 schema 至少包含：

- `schema_info`：schema 版本和创建/迁移信息；
- `asset`：稳定 asset ID、规范化 URI、媒体类型、文件大小、mtime、可选内容指纹、尺寸、导入状态、
  错误摘要和创建时间；规范化 URI 在单 catalog 内唯一；
- `preview`：asset ID、preview contract 版本、cache key、尺寸、状态和最近成功时间；
- 必需的事务、唯一性约束和外键；不把原片、完整 preview blob、Qt 对象或 C++ 内存布局写入数据库。

新数据库在一个事务中创建。迁移必须逐版本、可测试、失败时保持原文件可重开；未知更高版本拒绝打开。
第一版不承诺读取冻结 0.9 catalog。

### Import

`ImportRequest` 显式携带 catalog、文件/目录输入、递归策略、支持格式策略、任务预算、取消 token 和
correlation ID。`ImportResult` 为每个候选返回 imported / duplicate / unsupported / failed，
不能因部分成功丢失失败明细。

- 格式判断以 codec 探测为准，扩展名只用于候选过滤；
- 重复导入按规范化 URI 幂等，不创建重复资产；
- 先验证文件和 metadata，再提交可见 asset；失败记录不得伪装成可查看图片；
- 目录枚举顺序固定，批次提交边界明确；取消停止未派发工作并保留已提交的可信结果；
- 原文件只读，任何缓存或数据库写入都使用临时文件/事务后原子提交。

### Preview 与 viewer

`PreviewRequest` 携带 asset ID、目标像素尺寸、颜色/方向策略、CPU backend、资源预算、取消 token 和
request revision。`PreviewResult` 只返回可信 preview 资源或结构化失败。

- RAW preview 使用现有 Ravo CPU engine；raster preview 通过 adapter 解码后进入相同颜色/缩放契约；
- orientation、颜色描述和 alpha 显式处理，不能由文件名、QML view 或未标记 buffer 隐式决定；
- Gallery 只消费缩略 preview，viewer 请求适合当前视口的独立 preview；缓存 key 包含源指纹和契约版本；
- viewer 至少实现 loading、ready、missing、unsupported、failed 状态，不能把上一张图片留作新选择结果。

## 当前风险与开工约束

- 当前 `check_ravo_dependency_boundary.py` 仍冻结 headless target 图。M1 必须先扩展逐层 allowlist；不能
  删除检查、放宽所有目录或让 SQLite/Qt UI 类型借机进入 engine/domain/services。
- Qt Sql/QSQLITE、platform、imageformat plugin 的三平台 staged deployment 尚未验证。M1 只承诺已选
  PNG/RAW 纵切片；M2 的 JPEG/TIFF 在实际 codec/plugin 与许可、部署测试通过后才算支持。
- 现有 RAW 路径只证明首个 16-bit Bayer 切片。其他 sensor/layout 在扩展 fixture 前必须返回可见
  unsupported，不能因 LibRaw 能打开文件就宣称 viewer 已正确支持。
- catalog schema 一旦进入用户文件就只能版本化迁移。v1 只保存第一版闭环必需字段，不提前加入编辑、
  history、搜索 DSL 或旧 catalog 兼容列。
- 当前 foundation 尚无完整 owned executor。M1 需要一个可停止、可等待、可取消的最小任务 owner；不得
  用 UI 线程同步解码，也不得用 detached thread 换取演示速度。

## 执行里程碑

### M0：冻结基线与现有 engine/CLI（已完成）

- [x] 冻结 0.9 源码与 158 组静态 fixture，禁止配置、编译或运行旧工程。
- [x] 建立 foundation/recipe/engine/adapters/CLI/test target 与版本化机器契约。
- [x] 跑通真实 `mire1.cr2` inspect、有限 XMP import、CPU RAW→PNG 和 exposure 最小切片。
- [x] 建立 FreeCM source-root、显式 Config/Build/Test/Run 和 Windows/macOS 构建入口。

M0 是可复用底层，不是第一版软件完成状态。

### M1：单图库、单图片、真实桌面纵切片（当前最高优先级）

- [x] 重写当前路线图并用 ADR 接受 catalog/desktop 提前进入第一版。
- [x] 创建 `ravo_domain`、`ravo_services`、`ravo_desktop` target；更新依赖图和安装图。
- [x] 扩展 Qt 组件与 runtime/plugin/QML module 部署，并更新 dependency-boundary checker：`Qt6::Sql`
  仅 adapters 可用，`Qt6::Gui` 仅 raster adapters/desktop 可用，`Qt6::Qml`/`Qt6::Quick`、
  `QtQuick.Controls`/`QtQuick.Dialogs`/`QtQuick.Layouts` import 与 production `.qml` 仅 desktop 可用，
  `Qt6::QuickTest` 和 QML test 仅 desktop test target 可用；所有 target 禁止 Qt Widgets，新增 target 和
  QML import 全部进入扫描。
- [x] 定义 Catalog/Asset/Import/Preview 值类型、repository ports、错误和取消语义。
- [x] 增加最小 owned executor/task handle，证明取消、关闭 catalog/window、等待与晚到结果丢弃。
- [x] 实现 SQLite catalog schema v1、创建/打开、空库重开、事务和版本拒绝测试。
- [x] 实现 reference-only 单文件导入：`darktable-tests/0000-nop/expected.png` 与
  `darktable-tests/images/mire1.cr2`，包括 metadata 和幂等重复导入。
- [x] 实现数据库外原子 preview cache；RAW 走 engine，PNG 走 raster adapter。
- [x] 建立最小 Qt Quick/QML Ravo Studio：C++ presenter 暴露不可变 view state 和 commands，QML 完成
  创建/打开数据库、导入文件、资产列表、选择图片、适应窗口与 100% 查看。
- [x] 为同一用例增加无 UI service integration test。macOS Debug 已跑通 create/import/preview/reopen；
  桌面窗口可启动，手工 Create/Import/Fit/100% 仍需在本机验收。

出口：在一个全新目录中创建数据库，导入 PNG 和真实 RAW，二者均可在桌面 viewer 显示；重启后可再次
打开并查看；原文件哈希不变；所有行为不依赖冻结应用或网络。

### M2：可用的本地导入与浏览

- [x] JPEG/PNG 与 LibRaw RAW（含 ARW）导入和预览；目录递归导入跳过 XMP 等非图像。TIFF 仍取决于
  Qt imageformat plugin，未部署时保持 unsupported。
- [x] 增加目录递归、稳定排序、重复检测；损坏/不支持项不阻塞其余成功项。文件消失、取消和部分失败
  加固仍待补齐。
- [ ] Gallery 增加异步缩略图、占位/失败状态、基本滚动虚拟化和快速选择；viewer 增加平移与缩放。
- [ ] 数据库连接、worker、preview 内存与磁盘缓存建立明确预算；关闭窗口时无悬空任务或晚到 UI 写入。
- [ ] CLI 增加 catalog create/import/list/preview 的版本化 JSON，用作 services 的无 UI 验收客户端。

出口：代表性小目录可重复导入、浏览和重开；重复执行不产生重复资产；损坏文件不阻塞其余成功项。

### M3：第一版交付加固

- [ ] 覆盖数据库不可写/损坏、缓存损坏、磁盘满、源文件移动、批量取消、崩溃后重开和缓存重建。
- [ ] 建立 schema migration 夹具、导入/首帧资源门槛和长期浏览内存稳定性测试。
- [ ] 在可用的 Windows、macOS、Linux 工具链分别 configure/build/test；验证 Qt/SQLite/codec runtime 部署。
- [ ] 产出可安装的 Ravo Studio + `ravo` CLI staged package，并从安装目录完成第一版闭环。
- [ ] 完成键盘、焦点、HiDPI、基本可访问性和三平台手工验收清单。

出口：数据库、导入和查看功能在承诺平台可安装、可恢复、可重复使用；这是“第一版软件完成”的门槛。

### M4：本地照片工作流

- [ ] 增加 metadata 查看/编辑、评分、色标、标签、筛选、排序、照片版本和 history repository。
- [ ] 明确 managed-copy、移动/重链接、备份与旧 catalog 迁移产品策略后再实现对应能力。
- [ ] 保持 Gallery/viewer 只通过 desktop C++ presentation 层消费 services，不让 QML 访问 SQL、codec
  或 engine 私有状态。

### M5：编辑、recipe 与本地导出

- [ ] 按产品保留清单和 pixelpipe 依赖顺序迁移 operation，共享色彩、ROI/分块、mask/blend、插值、
  缓存和内存预算。
- [ ] Ravo Studio 参数编辑与 `ravo` CLI 使用同一 recipe/engine；结果具备合成、真实 RAW、金样和错误测试。
- [ ] 完成 JPEG/PNG/TIFF/原文件复制导出、metadata/ICC、冲突、磁盘满和取消。

### M6：性能、GPU 与跨平台发布

- [ ] CPU 正确性和资源门槛先满足；只有测量证明端到端收益后才增加后端中立 GPU adapter。
- [ ] GPU 类型不进入 recipe、domain、services 或 UI；失败从可信输入回退 CPU。
- [ ] 按 [GPU baseline](DevDocs/GPU_Baseline.md) 验证 preview、viewer 和 export 的正确性、性能与能耗。

### M7：发行切换与冻结实现退役

- [ ] Ravo 覆盖冻结产品范围、迁移/回滚可用并完成并行试用后，才切换默认发行物。
- [ ] 切换决定完成后统一删除旧 `src`、GTK、OpenCL、构建、资源、配置和重复测试。
- [ ] 用全仓搜索、链接图和发布验收证明不存在 `src → Ravo` 或 `Ravo → src` 生产依赖。

## 最小验证矩阵

| 层次 | M1 必须覆盖 | M3 第一版门槛 |
| --- | --- | --- |
| Unit | schema、值类型、URI、重复策略、cache key | migrations、损坏状态、资源预算 |
| Contract | repository/codec/cache ports、结构化错误、取消 | 磁盘满、源丢失、重建、任务销毁 |
| Integration | create → import PNG/RAW → preview → reopen | 目录/批量/部分失败/崩溃恢复 |
| Engine reference | RAW 有界 preview、方向、颜色、有限像素 | raster/RAW 代表集与容差 |
| Desktop | 创建/打开、导入、列表、选择、fit/100% | 键盘、焦点、HiDPI、可访问性、长浏览 |
| Platform | 当前开发平台 Debug | Windows/macOS/Linux Release 与 staged install |

所有自动测试只运行 Ravo target。冻结 fixture 可只读复用，但不得运行旧 CLI、旧 CTest、
`darktable-tests/run` 或旧打包图。单一平台结果不能表述为全平台通过。

## 0.9 冻结基线与 Ravo 承接项

0.9 继续是只读事实来源，不再维护或重构：

- 保留为静态参考：GTK Lighttable/Darkroom、catalog/history、masks、色彩、pixelpipe、OpenCL、配置和 fixture；
- Ravo 第一版承接：本地 SQLite catalog、JPEG/PNG/TIFF/RAW reference-only 导入、Gallery 和图片 viewer；
- 后续承接：history/styles、非破坏编辑、保留 operation、本地导出、完整 metadata/ICC；
- 明确不承接：Lua、历史插件/UI ABI、map/tethering、打印、幻灯片、远程发布和已删除旧格式兼容；
- 旧 OpenCL 不在 0.9 中替换为 Metal；它只在 M7 随冻结应用整体退役。

迁移期间两套生产实现完全独立。Ravo 测试可以读取冻结源码和 fixture，生产代码不得包含 `legacy-0.9/` 私有头、
链接旧库、加载旧 IOP 或访问全局 `darktable` 状态。

## 下一次开工的最小批次

1. Gallery 异步缩略图、占位/失败状态、滚动虚拟化；viewer 平移与连续缩放。
2. CLI catalog create/import/list/preview JSON，以及导入取消/部分失败/源文件消失加固。
3. 不要在这个批次扩展编辑、全量 IOP 或通用框架。
