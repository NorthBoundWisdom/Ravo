# 下一阶段：逐项从 legacy 迁入 Ravo

> 状态（2026-08-25）：当前执行队列。用户明确要求：用 C++20 在 Ravo 重写
> `legacy/` 能力，**一项完成后再删除对应旧实现**；最终 `legacy/` 只保留
> 「明确不迁移」清单。
>
> 产品边界仍以 [`TODO_REWRITE.md`](TODO_REWRITE.md)、[`SPEC.md`](SPEC.md)
> 和现有 ADR 为准。本文件只回答「下一个迁什么、怎样才算迁完、删哪些旧文件」。
>
> **一次只做队列里的第一项未完成工作。** 不要并行开下一项，也不要借机清
> GTK / OpenCL / 整棵 `legacy/src`。

## 1. 约定

- 重写，不搬运：读冻结 C 源与 fixture，在 `Ravo/` 用 C++20 重写算法与契约。
  生产代码仍不得 `#include` `legacy/src/`、链接旧库或加载旧 IOP。
- 仍禁止配置、编译、运行旧 0.9 工程、旧 CTest、旧 CLI 或 `legacy/tests/run`。
- QML 只展示和转发意图；SQL、codec、像素算法、任务 owner 留在 C++。
- 「Ravo 已有近似滑条」≠「已搬运该 IOP」。P1 的 exposure / filmic 风格
  contrast / 线性色温等，**不授权**删除 `exposure.c`、`filmicrgb.c`、
  `temperature.c`。只有本队列里该项达到第 2 节门槛后才删对应旧 owner。
- 共享基础设施（`imageio` 解码、LibRaw 入口、色彩配置文件）在最后一个
  消费者迁完之前不得删除。

## 2. 一项「Ravo 已验收」门槛

必须同时满足，才能把该项标为完成并进入删除：

1. 旧 owner、参数、线程、缓存、错误和只读 fixture 已盘点（写在该项条目里）。
2. Ravo 有 versioned schema / 值类型，未知输入 fail-fast。
3. CPU（或该能力所属的 service）实现可检查结果；没有 UI-only 假效果。
4. CLI 与 Studio 走同一 services/engine（纯算法项至少 CLI + 测试；用户流程项
   必须有 Studio 入口）。
5. 最小验证已**实际运行**，结果写回该项；未跑的平台如实写「未验证」。
6. [`Ravo/docs/phase0/capability-inventory.md`](Ravo/docs/phase0/capability-inventory.md)
   与 [`Ravo/MIGRATION.md`](Ravo/MIGRATION.md) ledger 已更新为 `Ravo 已验收`。
7. 同一变更删除第 3 节列出的旧文件，并更新 freeze / inventory 检查，使
   `ravo_freeze_reference`、`ravo_capability_inventory` 与
   `ravo_fixture_manifest` 对**剩余**树为真。

未达门槛：只改 Ravo，不删 `legacy/`。

## 3. 删除协议

删除范围仅限该项写明的实现、注册、GTK 面板入口和仅服务该能力的资源。

同一提交必须：

- 从 `legacy/src/iop/CMakeLists.txt`（或对应 `libs` / `imageio` / `views`）
  去掉注册；
- 全仓搜索确认 Ravo 生产代码没有引用被删符号；
- 更新 [`Ravo/tools/check_freeze_reference.py`](Ravo/tools/check_freeze_reference.py)
  与 [`Ravo/tools/freeze_legacy_manifest.py`](Ravo/tools/freeze_legacy_manifest.py)
  所保护的对象，使检查描述「剩余 leftover」，而不是假装 0.9 树从未改过；
- 保留仍被 Ravo 测试只读使用的 `legacy/tests/**` 原图 / XMP / 金样；只有
  Ravo 已有替代证据、且该 fixture 不再被任何测试引用时才删 fixture；
- 不追求剩余 `legacy/` 可构建。

第一项实际删除旧文件时，补一篇 ADR，记录「验收后增量删除」取代
ADR-0004「一律等到 M7 整包退役」的删除时点。Ravo ↔ `legacy/` 生产依赖禁令不变。

## 4. 已在 Ravo、本阶段不删旧实现

这些是产品控件或基础设施，不是完整 IOP 搬运：

| 能力 | Ravo 现状 | 旧代码暂留原因 |
| --- | --- | --- |
| Catalog / 导入 / preview | SQLite + reference-only + 缓存 | 旧 catalog / imageio 解码仍是取证与共享解码入口 |
| 评分 / 色标 / 拒绝 / 筛选 | schema v2 + Studio | 旧 `libs/filtering.c` 等不是同一数据模型 |
| P1 全局调节 + crop/straighten | `DevelopParams` + CPU ops | 多为近似；完整 `filmicrgb` / `ashift` / `temperature` 未验收 |
| 会话内 undo / before-after | presenter 栈 | 不是持久 history / snapshots |
| CLI PNG 渲染 | 有界原子 PNG | 不是产品导出（尺寸/质量/JPEG/TIFF/元数据/ICC） |

## 5. 明确不迁移（最终 leftover）

这些默认留在 `legacy/`，除非产品另作 dated 决定：

- GTK Lighttable / Darkroom、dtgtk、Bauhaus、旧模块布局 ABI
- Lua、动态 IOP 加载、历史插件 ABI
- 0.9 OpenCL（Ravo GPU 只走 M6 独立 adapter）
- 旧 catalog 文件格式、styles 二进制、未证明的全量 XMP history 重放
- map / tethering / 打印 / 幻灯片 / 远程发布
- 诊断 overlay：`overexposed`、`rawoverexposed`
- 未选显示变换：`filmicrgb`、`agx`（2026-08-25 选择 Sigmoid 为唯一默认）
- 专项创意模块（无新决定前不进队列）：`liquify`、`retouch`、`watermark`、
  `overlay`、`censorize`、`negadoctor`、`colorharmonizer`、`colorchecker`、
  `colormapping`、`colorize`

## 6. 迁移队列

只把第一项未完成条目当作当前任务。完成后勾选、删旧代码、再开始下一项。

### 1. 本地导出 JPEG / PNG / TIFF / 原片复制 — **完成**

- 用户结果：从 Studio 与 CLI 把当前 recipe 导出到用户指定路径；支持 JPEG
  质量、PNG、TIFF、原片字节复制；已存在目标、磁盘满、取消均结构化失败且
  不留下假成功文件。
- 旧 owner：`legacy/src/libs/export.c`、`export_metadata.c`；
  `legacy/src/imageio/` 中**仅编码/写出**侧。解码路径不动。
- Ravo owner：`ravo_services` 导出用例、`ravo_engine` / raster adapter 编码、
  desktop `executeCommand` 对话框、`ravo` CLI。
- 取证：只读 `legacy/src/libs/export.c`、`imageio/format/`、HC-06。
- 不做：批量任务持久化、远程发布、ICC 配置编辑器、旧导出预设 ABI。
- 验证（本机 macOS Debug）：
  - [x] `cmake --build --preset mac_clang_debug --target ravo_contract_tests ravo_catalog_tests ravo_studio`
  - [x] `CatalogServiceTest.ExportJpegPngOriginalCopyConflictAndCancel`
  - [x] `CliTest.CatalogCreateImportListPreviewAndDevelop`（含 export + conflict）
  - [ ] Studio 手工：选一张图导出 JPEG，确认原片未改、目标文件可读
- 删除：已删 `legacy/src/libs/export.c`、`export_metadata.c`。未动 `imageio`
  解码。`libs/CMakeLists.txt` 仍点名这两个文件（leftover 不可构建）。
- 风险：Windows / Linux staged codec deployment 未验证。Qt imageformat targets 已设为 required；磁盘满仅映射 `ENOSPC`，
  本机未注入满盘。Studio 手工导出未在本轮点过。

### 2. 色调曲线 — **完成**

- 用户结果：Develop Light 里一条可编辑的全局 tone curve（点列表），写入 recipe。
- 旧 owner：`legacy/src/iop/tonecurve.c`（`rgbcurve.c` 本项不做）。
- Ravo owner：`ravo.core.tonecurve` schema、`ravo_engine` CPU、Inspector
  `ToneCurveEditor`。
- 取证：只读 `legacy/src/iop/tonecurve.c` `process()`。旧实现是 Lab 三通道 +
  `MONOTONE_HERMITE` / autoscale RGB。Ravo **没有**抄 Lab 或 GTK 控件状态。
- 契约：`working_space` 为 `srgb`（默认）或 `linear_rgb`；
  `interpolation=monotone_hermite`；`channel_mode=rgb`（同一曲线作用于 R/G/B）；
  `points` 为 2–16 个 `{x,y}`，x 严格递增且覆盖 0..1。未知空间/插值 fail-fast。
- 验证（本机 macOS Debug）：
  - [x] `RecipeTest.ToneCurveRoundTripAndRejectsUnknownColourPolicy`
  - [x] `EngineFacadeTest.ToneCurveMapsSyntheticRasterAndRejectsLab`
  - [x] `ravo_freeze_reference` / `ravo_capability_inventory`
  - [ ] Studio 手工：拖点后重开 catalog 曲线还在
- 删除：已删 `legacy/src/iop/tonecurve.c` 及其 `add_iop`。`rgbcurve.c` 仍在。
- 风险：Windows / Linux 未验证。Studio 手工未在本轮点过。v1 把工作缓冲
  0–1 外的样本夹到端点，不做旧 Lab unbound 外推。

### 3. 显示变换（Sigmoid）— **完成**

- 产品决定：2026-08-25 选择 `sigmoid` 为唯一默认 display transform，记录于
  product-decision-register。`filmicrgb` / `agx` 进入第 5 节 leftover，无运行时
  selector 或 fallback。
- 用户结果：RAW preview/export 默认经过 Standard SDR Sigmoid；Develop Light
  提供 Contrast、Skew、Preserve Hue，参数随 recipe 重开。JPEG/PNG/TIFF 已是
  display-referred，不重复套 transform。
- 旧 owner：`legacy/src/iop/sigmoid.c`。只读取证其 generalized log-logistic、
  per-channel、negative desaturation 与 hue-energy preservation；未搬 GTK/OpenCL、
  presets、RGB-ratio 或 primaries 编辑器。
- Ravo owner：`ravo.display.sigmoid` v1 schema、`ravo_engine` CPU、CatalogService
  RAW baseline、Inspector。契约固定 `working_space=linear_srgb`、
  `color_processing=per_channel`，未知模式/非有限值 fail-fast。
- 重叠决定：RAW Studio Contrast 改由 Sigmoid 拥有；`ravo.core.contrast` 继续服务
  display-referred raster 输入和旧 recipe。Highlights/Shadows/Whites/Blacks
  继续是 transform 前的 scene controls。
- 验证（本机 macOS Debug）：
  - [x] `RecipeTest.SigmoidRoundTripRequiresExplicitFiniteColorPolicy`
  - [x] `EngineFacadeTest.SigmoidMapsSyntheticPixelsAndPreservesHueByPolicy`
  - [x] `EngineFacadeTest.SigmoidHasARealRawReference`
  - [x] `CatalogServiceTest.RawSigmoidBaselinePersistsOnlyUserOverrides`
  - [x] `ravo_unit_tests` / `ravo_contract_tests` / `ravo_catalog_tests` / `ravo_studio` build
  - [ ] Studio 手工：RAW 调整并重开 catalog
- 删除：已删 `legacy/src/iop/sigmoid.c` 及其 `add_iop`。`filmicrgb.c` / `agx.c`
  未动。
- 风险：Windows / Linux 与 Studio 手工未验证。RAW interactive preview 为保证
  scene-linear 正确性走完整 CPU decode，而不是 embedded-JPEG fallback；后续性能只
  能用明确的 linear working-image cache 优化。

### 4. 色彩校准（通道混合）

- 用户结果：比色温/色调更完整的 RAW/线性校准（至少 RGB 混合）。
- 旧 owner：`legacy/src/iop/channelmixerrgb.c`。
- 验证：recipe 往返 + 合成光栅；Studio 有入口。
- 删除：`channelmixerrgb.c` 与 chart 辅助若已无其他消费者。
- 风险：不要把整个 CAT16 / 色卡工作流一次做完。

### 5. RAW 高光重建

- 用户结果：裁切高光可重建或明确 unsupported，而不是 silently clip。
- 旧 owner：`legacy/src/iop/highlights.c`。
- 验证：`mire1.cr2` 或专用高光 fixture；失败路径结构化。
- 删除：`highlights.c`。
- 依赖：现有 `ravo.raw.prepare` / demosaic 切片。

### 6. 降噪（先选定一个）

- 产品门禁：默认迁 `denoiseprofile`（旧 DEFAULT_VISIBLE）。`nlmeans` /
  `atrous` / `bilateral` / `rawdenoise` 未选定前不进本项。
- 旧 owner：`legacy/src/iop/denoiseprofile.c`。
- 验证：合成噪声 + 一张真实 RAW；内存/时间预算写进测试。
- 删除：`denoiseprofile.c`。
- 风险：无共享 denoise 设施时只做这一个完整模块，不抽半套框架。

### 7. 镜头校正

- 用户结果：按镜头数据库做畸变/TCA/暗角，或对缺失标定 fail-fast。
- 旧 owner：`legacy/src/iop/lens.cc`。
- Ravo owner：engine + 显式 lensfun（或后继）source-root adapter。
- 验证：有标定与无标定两条；不得静默跳过。
- 删除：`lens.cc`。依赖仓按 source-root 流程发布后再钉模板。
- 风险：新依赖必须进 `source_roots.lock.jsonc.in`，禁止 FetchContent。

### 8. 标签与可写 metadata

- 用户结果：关键字/标签持久化；基础 EXIF/IPTC 字段可查看，允许的字段可写回
  catalog（不写原片，除非该项明确做 sidecar 且单独验收）。
- 旧 owner：`legacy/src/libs/tagging.c`、`metadata.c`、`metadata_view.c`。
- 验证：schema 迁移、重开、Unicode 路径；Studio 可打标签并筛选。
- 删除：上述 libs（确认无其他 GTK 入口强依赖后再删）。
- 不做：人脸、地图、GPS 写回、旧 sidecar 全量兼容。

### 9. 持久化 history / 快照

- 用户结果：不只是会话 undo；可回到先前 recipe 快照。
- 旧 owner：`legacy/src/libs/history.c`、`snapshots.c`、`copy_history.c`。
- 验证：保存/重开/失败回滚；Studio 列表与 CLI。
- 删除：上述 libs。
- 风险：不要导入旧 XMP history 图；未知旧 history 继续 `unsupported`。

### 10. 颜色均衡（HSL 分区）

- 产品门禁：`colorequal` 与 `colorzones` 二选一。
- 旧 owner：被选中的 IOP。
- 验证：recipe + 合成色块；Studio 分区滑条。
- 删除：被选中的 IOP。另一个进 leftover 或另开项。

### 11. 渐变滤镜（第一个局部调整）

- 用户结果：线性渐变改变曝光/密度，不引入完整 mask 图。
- 旧 owner：`legacy/src/iop/graduatednd.c`。
- 依赖：若必须通用 mask，先停下来写 mask 契约，不要在本项里偷做
  `mask_manager`。
- 验证：合成渐变 + 取消。
- 删除：`graduatednd.c`。

### 12. 影调均化

- 旧 owner：`legacy/src/iop/toneequal.c`。
- 验证：合成 + 一张 RAW。
- 删除：`toneequal.c`。
- 依赖：第 3 项显示变换已选定，避免再做一套冲突的全局影调。

## 7. 队列之后（现在不要拆开做）

需要单独产品决定后再入队：

- 通用 mask / blend 图（`mask_manager`、drawn/parametric masks）
- `cacorrect` / `cacorrectrgb` / `hotpixels` / `rawdenoise`
- `lut3d`、`borders`、`colorbalancergb`（若与第 3/4 项不重复）
- styles / presets 商店
- 旧 catalog 迁移、managed copy、写回 sidecar

## 8. 当前开工命令

第 3 项已完成。下一项是色彩校准（`channelmixerrgb`）。开始前：

```text
git branch --show-current
git status --short --branch
```

只读取证 `legacy/src/iop/channelmixerrgb.c` 与对应 fixture；不要把 CAT16、色卡
和完整 chromatic-adaptation UI 一次带入。
