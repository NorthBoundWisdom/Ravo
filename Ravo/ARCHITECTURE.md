# Ravo Architecture

## 核心结论

Ravo 当前第一产品是可实际使用的本地照片浏览器：创建/打开 SQLite catalog，reference-only 导入
JPEG/PNG/TIFF/RAW，并在 Ravo Studio 中查看图片。`ravo` CLI 继续是受支持的无 UI 客户端；CLI 与
desktop 必须调用同一 application services 和 engine，不得拥有两套 catalog、import、preview 或 recipe。

```text
ravo CLI ───────────────┐
                        ▼
                 Application Services ◀────────── Ravo Studio
                 │ create/open                    │ Gallery
                 │ import/list                    │ viewer
                 │ request/cancel preview         │ visible errors
                 ├───────────────┐
                 ▼               ▼
           Catalog Domain     Ravo Engine Facade
                 ▲               ▲
                 │ implements    │ implements
          SQLite/FS Adapter   RAW/Raster/Cache Adapters

冻结 0.9 legacy/src/ ──只读源码与 fixture 证据──▶ Ravo tests
冻结 0.9 legacy/src/ ╳──────────────────────────▶ Ravo production
```

这条顺序由 [ADR-0007](docs/adr/0007-first-usable-catalog-viewer.md) 接受。它提前验证 catalog、导入、
preview、任务和窗口生命周期，但不恢复旧 GTK、动态 IOP ABI 或全局状态。

## Target 与依赖方向

| Target | 所有权 | 允许依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| `ravo_foundation` | errors、IDs、取消、基础资源契约 | 标准库、按需 QtCore | recipe、engine、catalog、UI |
| `ravo_recipe` | recipe、operation schema、版本升级 | foundation、按需 QtCore | codec、数据库、UI |
| `ravo_engine` | inspect、operation registry、CPU render/preview | foundation、recipe、engine ports、按需 QtCore | catalog、services、CLI、UI、旧 `src` |
| `ravo_domain` | Asset/Catalog、Import/Preview 状态、repository ports | foundation | SQLite、codec、engine 私有类型、UI |
| `ravo_services` | create/open/import/list/preview 用例与任务编排 | domain、engine facade | SQL、QML/presentation 类型、第三方 codec 类型 |
| `ravo_adapters` | SQLite、filesystem、RAW/raster codec、preview cache | 对应 ports、Qt Core/Gui/Sql、固定第三方依赖 | QML/UI 状态、旧核心 |
| `ravo_cli` | 参数、JSON、退出码、CLI composition | services、engine facade、adapters | 算法、SQL、UI |
| `ravo_desktop` | C++ composition/presenter、Qt Quick/QML 窗口、Gallery、viewer、文件选择 | services、只读 preview 资源、Qt Core/Gui/Qml/Quick、GeoControls | Qt Widgets、SQL、codec、算法私有状态 |

SQLite 由私有 Qt Sql/QSQLITE adapter 包装，raster 首版由私有 `QImageReader` adapter 包装；LibRaw 和平台
API 同样不越过 port。Qt 值类型可在有明确收益的 target 内使用，但 recipe、CLI JSON、catalog schema
和公开持久化契约不得序列化 Qt/C++ 对象内存布局。

Ravo Studio 只有一套 presentation 架构：C++ composition root 持有 services、任务与
`QQmlApplicationEngine`，desktop-owned QObject presenter/model 把不可变 service snapshot 和 commands
映射给 QML。QML/JavaScript 只拥有瞬时 view state、布局、绑定和输入，不实现 catalog/import/preview
业务规则。Studio 的菜单、快捷键、右键菜单和 inspector 控件都通过同一组 QML `Action` 调用
`StudioPresenter::executeCommand`；窗口对话框由该入口发出 `uiCommandRequested`，QML 只负责弹出。
Develop 预览由 presenter 做有界合并：同一时刻最多一个 in-flight 渲染，另加最多一份待保存
recipe 和一份待预览请求；过期结果丢弃，失败时保留上次已验证 preview。拖动时 presenter 只转发
内存中的 develop 参数，不写 recipe；服务层按 `kInteractivePreviewMaxEdge` 在已缓存的
scene-linear 工作图上套用效果，只返回内存像素、不写 PNG/cache。RAW unpack 与 demosaic 缓存在
CatalogService，highlight reconstruction 变化时失效；不得回退到 embedded JPEG。松手后再保存
并请求完整 preview。
Develop 裁剪在画布上交互：crop tool 预览去掉 crop 与 straighten，由 Qt Quick 旋转工作图；
描边与照片共用这一 GPU 变换，裁剪框保持屏幕轴向并内接旋转后的照片。框选和 Angle 拖动只更新
内存参数，松手后写入 recipe；导出仍走 CPU straighten，不是 engine GPU adapter。
删除照片默认只从 catalog 移除记录和 preview cache，不删除原片。显式的
“Delete from Disk” 命令在确认后删除原文件，再移除 catalog 记录。QML 资源通过 `qt_add_qml_module`
纳入构建和部署；首版不链接 Qt Widgets，也不提供混合 fallback。

## 核心数据契约

### Catalog

catalog 是单个用户选择的 SQLite 文件。schema v1 至少保存：

- schema 版本与迁移元数据；
- 稳定 asset ID、规范化本地 URI、媒体类型、源文件 size/mtime/可选指纹；
- 尺寸、orientation、基础拍摄元数据、import 状态和结构化错误摘要；
- preview contract 版本、cache key、尺寸、状态和最近成功时间。

规范化 URI 在一个 catalog 内唯一。数据库不保存原片或完整 preview blob，不保存 presentation 状态、recipe
对象布局、codec 句柄或数据库行地址。schema v2 在 `asset` 上保存 rating/color/reject；schema v3 用
`asset_recipe` 保存每张图最多一份 canonical recipe JSON；schema v4 增加
`asset_tag`、`asset_metadata`（只读 capture EXIF + catalog-only 可写字段）和
`asset_recipe_history`（history/snapshot）。新库和每次 migration 使用事务；未知更高
schema 版本 fail-fast。

首版不读取或迁移冻结 0.9 catalog。未来兼容工作必须有独立产品决定、备份/回滚和 fixture。

### Import

`ImportRequest` 携带 catalog ID、文件/目录输入、递归与格式策略、资源预算、取消 token 和 correlation
ID。`ImportItemResult` 对每个输入返回 imported、duplicate、unsupported 或 failed；批次不能因部分
成功丢失失败明细。

导入首版只登记原文件，不复制、移动、改名、改写 metadata 或删除。格式由 codec 探测确认，扩展名只做
候选过滤。先验证可信 metadata，再通过事务发布可见 asset；取消停止未派发工作，已提交结果仍保持有效。

### Preview

`PreviewRequest` 显式携带 asset ID、目标像素尺寸、方向/颜色策略、backend、内存/线程预算、取消 token
和 request revision。`PreviewResult` 返回可信的只读 preview 资源或结构化失败。

RAW 通过 Ravo CPU engine，JPEG/PNG/TIFF 通过 raster adapter；两条路径统一 orientation、颜色、alpha、
缩放、有限值与错误契约。preview 在数据库外原子写入受控缓存，cache key 包含源指纹、目标尺寸和 contract
版本。缓存损坏或缺失时可从只读原片重建。
preview contract v4 对 RAW 使用完整 CPU decode/render，并在 scene-linear 工作缓冲末端应用
`ravo.display.sigmoid` 基线；不再把 embedded JPEG 当作可编辑的 scene-linear 数据。基线 operation
不产生 `asset_recipe` 行，也不标记 `has_edits`；用户覆盖参数后才持久化。已有 JPEG/PNG/TIFF 是
display-referred 输入，不隐式重复应用 Sigmoid。

Qt raster adapter 实际接受 PNG/JPEG/BMP/GIF/WebP/TIFF，并导出 PNG/JPEG/TIFF；因此对应
JPEG/GIF/WebP/TIFF plugin targets 与 catalog 使用的 QSQLITE driver 都是 configure-time required。
TGA/WBMP/ICO 和其他 SQL drivers 没有产品消费者，不进入 required 集合。

### Recipe 与 operation

既有 canonical recipe、operation descriptor、`RenderRequest`/`RenderResult` 和显式 colour contract
继续有效。第一版 viewer 只依赖生成可信 preview 所需的最小 CPU 链，不要求先迁完全部旧 operation。
后续编辑 UI 只能映射 versioned schema，不拥有第二套算法或 history 格式。
色调曲线是 `ravo.core.tonecurve`：冻结 C 默认 RGB linked（Lab D50 → ProPhoto，
`preserve_colors=average`），0–1 点列表、`interpolation=monotone_hermite`。
`working_space=lab|xyz|lab_independent` 是显式 C mode。Inspector 只转发点；求值在 recipe/engine。
唯一默认显示变换是 `ravo.display.sigmoid` v1：`working_space=linear_srgb`、
`color_processing=per_channel`，显式保存 middle-grey contrast、skew、Standard SDR
black/white target 与 hue preservation。它是 RAW 基线与 scene-referred operation 链的末端，
随后才做 sRGB encoding。RAW Studio 的 Contrast 由 Sigmoid 拥有；`ravo.core.contrast`
继续服务 display-referred raster 输入和旧 recipe。highlights/shadows/whites/blacks 仍是
transform 之前的 scene controls。RAW 路径可在 demosaic 前运行 `ravo.raw.highlights`；
默认降噪、镜头校正、dt UCS `colorequal`、渐变滤镜和 9 带 toneequal 走同一 recipe/engine。

所有 decode/preview/render 边界仍显式携带像素格式、alpha、源/目标颜色描述和 profile 状态；UI、文件名
或无标记 buffer 不得隐式选择色彩策略。完整约束见
[ADR-0006](docs/adr/0006-explicit-colour-contract.md)。

## Services

第一版 services 至少提供：

- `CreateCatalog` / `OpenCatalog`：创建、校验、迁移并返回不可变 catalog snapshot；
- `ImportAssets`：枚举输入、调用 codec/engine、事务提交资产并调度 preview；
- `ListAssets` / `ObserveCatalog`：返回稳定排序和 revision，不暴露 SQL cursor；
- `RequestPreview` / `CancelPreview`：按 viewport 请求有界 preview，丢弃过期结果；
- `CloseCatalog`：停止相关任务、释放连接和缓存句柄后完成。

CLI 和 desktop composition root 可以选择不同的 presenter，但必须装配同一 services、ports 和 adapter。

## 所有权、生命周期与线程

- composition root 创建 catalog repository、codec、engine、cache、executor、services 和客户端，并在任务
  全部终止后按反向顺序销毁。
- UI 主线程拥有 `QQmlApplicationEngine`、窗口和 desktop presentation state；扫描、metadata、decode、
  preview、cache 与数据库 I/O 只在 C++ owner 管理的任务中运行，禁止 detached thread。
- catalog、asset、recipe、descriptor 和跨线程结果是不可变快照；写操作产生新 revision。
- database connection 不跨线程裸共享；adapter 使用明确的串行 owner 或每 worker 受控连接，并在关闭前
  完成/回滚事务。
- pixel buffer 和 preview 临时文件有唯一写 owner；只读共享必须绑定资源句柄和清晰有效期。
- 每个异步完成事件携带 catalog/asset/request revision；取消、catalog 关闭或选择变化后的旧结果丢弃。
- codec、数据库、缓存或内存失败只能从可信输入重试或返回失败，不能继续展示部分输出。
- 用户原片始终只读；数据库和 preview 只在事务/原子提交成功后对客户端可见。

## Desktop 边界

Ravo Studio 第一版负责：

- 创建/打开 catalog、选择文件/目录；
- Gallery 列表、loading/ready/missing/unsupported/failed 状态；
- 选择图片、Gallery（grid/放大）与 Edit 分栏、适应窗口、100% 和平移；
  grid 与 filmstrip 用 contain 整图显示，letterbox 空白叠加序号、评分、格式/尺寸与标记；
- Gallery/Edit 共用右侧面板上方示波器：冻结 C 的 256 档 RGB 直方图（linear Y）与 RGB parade 分量图；
- 进度、取消和可恢复错误呈现；
- 窗口、焦点、键盘、HiDPI 和基本可访问性。

QML view 只向 desktop-owned C++ presenter 发送 intent，并观察带 revision 的不可变 view state。可见控件
使用 GeoControls（按钮、标签、列表项、分段开关、状态栏、文件对话框）。Gallery/`Image` 只消费受控
preview 资源，不直接打开用户原片。QML 中不得出现 SQL、文件枚举、codec 探测、任务调度或与
services 重复的业务状态机。文件夹选择 GeoControls 没有对应控件，因此由 desktop 的
`FolderDialogPage.qml` 按同一对话框契约封装 `FolderDialog`。

Ravo Studio 不负责：

- SQL、文件枚举、codec 探测、RAW 处理、preview 缓存或 recipe 求值；
- 直接持有 engine/SQLite/LibRaw 裸对象；
- 通过 shell 启动 `ravo`；
- 复刻旧 GTK widget、引入 Qt Widgets fallback、旧配置键或动态 IOP 生命周期。

## CLI 边界

CLI 继续负责参数、stdin/stdout、版本化 JSON、稳定退出码和 headless composition。现有
catalog create/import/list/preview/develop/export 命令验证同一 services；人类日志不能污染 JSON stdout。

## 当前非目标

- 本地 JPEG/PNG/TIFF/原片复制导出已由 CatalogService 拥有；完整 metadata/ICC、批量任务和旧导出预设仍不在范围内。
- 第一版不实现完整 history/styles、mask/blend、全部 operation 或旧 catalog 迁移。
- CPU 正确性和 viewer 资源门槛完成前不实现 GPU backend。
- 不为尚无消费者的网络、云同步、公共插件 ABI 或复杂查询语言冻结接口。
- 不修改冻结 0.9 来调用 Ravo，也不让 Ravo 生产代码调用冻结应用。
