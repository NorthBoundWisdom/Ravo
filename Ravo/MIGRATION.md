# Ravo Migration Policy

## 目标

Ravo 最终取代 `legacy/src/`。catalog/import/viewer 与 Basic Develop 已在 Ravo 落地。下一阶段按
根 [`TODO_LEGACY_MIGRATION.md`](../TODO_LEGACY_MIGRATION.md) 逐项重写；
一项达到「Ravo 已验收」后删除对应旧 owner。
新所有权只进入 `Ravo/`。0.9 仍禁止配置、编译、运行。

## 单向边界

允许：

- Ravo 测试只读 `legacy/tests/` 的原图、XMP 和金样；
- Ravo 静态阅读 `legacy/src/` 的算法、catalog、导入和 UI 调用链；
- Ravo 通过 FreeCM 直接消费固定第三方依赖；
- Ravo 自己实现 SQLite catalog、导入 services、preview pipeline 和 C++-backed Qt Quick/QML desktop。

禁止：

- Ravo 生产 target 包含 `legacy/src/` 私有头、链接 `libdarktable`、加载旧 IOP 或读取全局 `darktable`；
- 配置、编译或运行旧 CLI、旧 CTest、`legacy/tests/run` 或旧打包 target；
- 让冻结应用调用 Ravo，或为迁移在旧 GTK 应用中增加 adapter、入口和构建依赖；
- 通过旧 CLI 生成即时 oracle，或把旧 catalog/GTK 类型包成新 API；
- 让 QML/JavaScript 发 SQL、直接解码图片、拥有 engine 任务或复制 services 业务逻辑；
- 用永久 shim、静默 fallback 或复制实现掩盖尚未决定的数据兼容性。

生产依赖始终保持完全独立，不形成 `src → Ravo` 或 `Ravo → src`。

## 第一版迁移单位

当前迁移单位是可由用户和自动测试共同观察的纵切片：

1. **取证**：列出冻结 owner、输入格式、数据/线程/错误行为和只读 fixture。
2. **定义契约**：写 catalog schema、Asset/Import/Preview 值类型、ports、生命周期、取消和失败语义。
3. **实现 Ravo owner**：domain/services/engine/adapters/desktop 各自只拥有本层职责。
4. **无 UI 验证**：service integration 完成 create → import → preview → reopen。
5. **桌面验收**：Ravo Studio 完成创建/打开、导入、列表、选择和查看。
6. **资源与恢复**：覆盖重复、损坏、缺失、取消、磁盘/数据库失败、关闭和重启。
7. **记录状态**：更新 roadmap、ADR、ledger、实际验证和未覆盖平台。

一次变更不必完成整个纵切片，但不得把“target 已创建”“数据库能打开”或“窗口能显示”单独标为第一版完成。

## 后续算法迁移单位

编辑 capability、共享算法或 operation 仍按以下顺序：

1. 盘点旧 owner、注册、调用者、参数、线程、缓存、GPU、资源和 fixture；
2. 静态冻结真实 RAW/XMP/像素/metadata 证据，不运行旧 CPU 路径；
3. 定义 canonical schema、输入输出、ownership、取消/失败和不兼容项；
4. 只迁所需数学与行为，去除 GUI、旧生命周期、全局状态、动态 ABI 和 OpenCL 类型；
5. 运行 unit、synthetic、legacy mapping、真实 RAW/golden、错误/取消和资源验证；
6. 让 CLI/Studio 通过同一 services/engine 成为正式消费者；
7. 按根 active migration TODO 在该项 Ravo 已验收后删除对应旧 owner，并同步
   freeze/inventory 检查。

## “已被 Ravo 吃掉”的定义

“Ravo 已验收”与“旧实现已删除”是两个状态。一项能力只有同时满足以下条件才最终完成：

- Ravo 是受支持实现，并拥有数据、CPU/UI 行为、错误、取消和资源契约；
- 承诺 fixture、service/desktop 测试和平台门槛达到阈值；
- 历史数据迁移或显式拒绝策略已记录并测试；
- 发行切换完成，生产构建没有第二份可达旧实现；
- `src` 对应源码、构建、注册、配置、资源和入口已按 active migration TODO 删除；
- 文档、搜索和链接图没有意外消费者或反向依赖。

## 迁移顺序

1. 已验收的 catalog/review/develop/export 基线保持可回归；
2. 严格按根 active migration TODO 一次迁一个 legacy capability，并同步删除旧 owner；
3. 队列后的 mask、额外 RAW/颜色、styles/catalog 兼容先做 dated 产品决定；
4. 横向可靠性、三平台安装与可选 GPU 按 TODO 门槛验收；
5. 队列清空后证明发行切换/回滚，再处理明确 leftover 的归档或最终清理。

## 明确不迁移的 leftover

除非另有 dated 产品决定，以下只保留为冻结证据，不进入 Ravo：

- GTK Lighttable/Darkroom、dtgtk、Bauhaus 和旧模块布局/UI ABI；
- Lua、动态 IOP 加载、历史插件 ABI；
- 0.9 OpenCL；Ravo GPU 不复用该 API；
- 旧 catalog/styles 二进制和未证明的全量 XMP history 重放；
- map、tethering、打印、幻灯片、远程发布；
- 诊断 overlay：`overexposed`、`rawoverexposed`；
- 未选显示变换：`filmicrgb`、`agx`；
- 专项创意模块：`liquify`、`retouch`、`watermark`、`overlay`、`censorize`、
  `negadoctor`、`colorharmonizer`、`colorchecker`、`colormapping`、`colorize`；
- 未选 HSL 分区：`colorzones`（2026-08-25 选择 `colorequal`）。

不得把 leftover 做成空壳迁移，也不得在 active migration TODO 清空前批量删除。

## 迁移 ledger

| Capability | 旧 owner | Ravo owner | 状态 | 当前证据 / 下一门槛 |
| --- | --- | --- | --- | --- |
| 基础错误/取消 | `src/common`, `src/control` | foundation | 实现中 | cancellation/deadline 与 SerialExecutor submit/wait_idle 已测 |
| Recipe/schema | IOP params/XMP | recipe | 实现中 | versioned round-trip 与有限 exposure mapping 已测 |
| RAW inspect/decode | imageio/LibRaw | engine + codec adapter | 实现中 | `mire1.cr2` inspect/render 已测；格式与 sensor 覆盖有限 |
| CPU preview/pixelpipe | `src/develop` | engine | 实现中 | bounded PNG、取消、exposure 亮度已测；完整颜色/ROI 未完成 |
| SQLite catalog | common/database | domain + SQLite adapter | 实现中 | schema v4 create/reopen/migrate/newer-version reject 已测；tags/writable metadata/history 已测；无旧 catalog 迁移 |
| Reference-only import | common/imageio/import | services + adapters | 实现中 | PNG/JPEG + LibRaw RAW（含 ARW）与目录递归已测；JPEG/GIF/WebP/TIFF plugin targets 已设为 required |
| Preview cache | mipmap/cache/imageio | services + adapters | 实现中 | 库外原子 PNG 缓存与 reopen 重建已测 |
| Gallery/viewer | lighttable/darkroom | desktop + services | 实现中 | Studio 可创建/打开/导入/fit/fill/100%；长列表资源门槛仍待验收 |
| Catalog metadata/workflow | common/libs | domain + services | 旧实现已删除 | Unicode 标签筛选、catalog-only 可写 title/creator/copyright、只读 capture EXIF、持久 history/snapshot；faces/map/GPS 写回未做。旧 `libs/tagging.c`/`metadata*.c`/`history.c`/`snapshots.c`/`copy_history.c` 已删 |
| Mask/blend/operations | develop/iop | recipe + engine | 实现中 | `ravo.effect.graduatednd` 是第一个局部调整，梯度即 mask；通用 mask 图仍未做 |
| RAW 高光重建 | `iop/highlights.c` | `ravo.raw.highlights` | 旧实现已删除 | Bayer 2×2 clip/inpaint；非 Bayer 与 raster 路径 structured unsupported |
| 默认降噪 | `iop/denoiseprofile.c` | `ravo.detail.denoiseprofile` | 旧实现已删除 | 线性 RGB 多尺度阈值；`nlmeans`/`atrous`/`bilateral`/`rawdenoise` 不在本项 |
| 镜头校正 | `iop/lens.cc` | `ravo.geometry.lens` | 旧实现已删除 | 显式 Brown-Conrady + 版本化 lookup 表；无匹配 fail-fast。lensfun source-root 仍是生产数据库后继，本轮未钉依赖 |
| 颜色均衡 | `iop/colorequal.c` | `ravo.color.colorequal` | 旧实现已删除 | 8 带 HSL；`colorzones` leftover |
| 渐变滤镜 | `iop/graduatednd.c` | `ravo.effect.graduatednd` | 旧实现已删除 | 线性密度/硬度/旋转/偏移 |
| 影调均化 | `iop/toneequal.c` | `ravo.core.toneequal` | 旧实现已删除 | Sigmoid 前 5 带 log-luminance EV |
| 本地导出 | imageio / `libs/export.c` | services + raster encoder + CLI/Studio | 旧实现已删除 | JPEG/PNG/原片复制与 conflict/cancel 已测；TIFF plugin target 已设为 required；metadata/ICC 未做。旧 `libs/export*.c` 已删 |
| 色调曲线 | `iop/tonecurve.c` | `ravo.core.tonecurve` + Develop Inspector | 旧实现已删除 | 一条链接 RGB 点曲线；schema 显式 `working_space=srgb\|linear_rgb`。不是 Lab 三通道移植。`rgbcurve` 未做 |
| 默认显示变换 | `iop/sigmoid.c` | `ravo.display.sigmoid` + RAW baseline + Develop Inspector | 旧实现已删除 | per-channel generalized log-logistic；线性 sRGB、Standard SDR target、合成/真实 RAW/catalog reopen 已测。`filmicrgb`/`agx` 明确留作 leftover |
| CLI | `src/cli` | cli | 实现中 | engine/recipe/catalog/develop/export JSON 均走正式 services/engine |
| GPU | OpenCL/pixelpipe | engine adapter | 延后 | active TODO / GPU baseline：CPU goldens 和端到端收益证明后开始 |

状态只使用“未开始 / 基线冻结 / 实现中 / Ravo 已验收 / 旧实现已删除 / 延后 / 不支持”。旧 owner
的物理删除以 active migration TODO 的该项验收为准，不再等待整包退役。
