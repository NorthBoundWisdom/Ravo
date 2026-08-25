# Ravo Migration Policy

## 目标

Ravo 最终取代 `legacy/src/`。catalog/import/viewer 与 P1 develop 已在 Ravo 落地。下一阶段按
[`../TODO.md`](../TODO.md) 逐项重写；一项达到「Ravo 已验收」后删除对应旧 owner。
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
7. 按 [`../TODO.md`](../TODO.md) 在该项 Ravo 已验收后删除对应旧 owner，并同步
   freeze/inventory 检查。

## “已被 Ravo 吃掉”的定义

“Ravo 已验收”与“旧实现已删除”是两个状态。一项能力只有同时满足以下条件才最终完成：

- Ravo 是受支持实现，并拥有数据、CPU/UI 行为、错误、取消和资源契约；
- 承诺 fixture、service/desktop 测试和平台门槛达到阈值；
- 历史数据迁移或显式拒绝策略已记录并测试；
- 发行切换完成，生产构建没有第二份可达旧实现；
- `src` 对应源码、构建、注册、配置、资源和入口已按 [`../TODO.md`](../TODO.md) 删除；
- 文档、搜索和链接图没有意外消费者或反向依赖。

## 迁移顺序

1. M1–M3：SQLite catalog、reference-only import、CPU preview、Gallery/viewer 与第一版加固。
2. M4：metadata、评分/标签、筛选、照片版本、history 和本地工作流。
3. M5：完整 recipe 编辑、保留 operation、mask/blend、色彩/ROI/分块与本地导出。
4. M6：CPU 门槛后的可选 GPU adapter、性能与跨平台发布优化。
5. M7：发行切换、数据回滚证明，以及 [`../TODO.md`](../TODO.md) leftover 清理。

## 迁移 ledger

| Capability | 旧 owner | Ravo owner | 状态 | 当前证据 / 下一门槛 |
| --- | --- | --- | --- | --- |
| 基础错误/取消 | `src/common`, `src/control` | foundation | 实现中 | cancellation/deadline 与 SerialExecutor submit/wait_idle 已测 |
| Recipe/schema | IOP params/XMP | recipe | 实现中 | versioned round-trip 与有限 exposure mapping 已测 |
| RAW inspect/decode | imageio/LibRaw | engine + codec adapter | 实现中 | `mire1.cr2` inspect/render 已测；格式与 sensor 覆盖有限 |
| CPU preview/pixelpipe | `src/develop` | engine | 实现中 | bounded PNG、取消、exposure 亮度已测；完整颜色/ROI 未完成 |
| SQLite catalog | common/database | domain + SQLite adapter | 实现中 | schema v1 create/reopen/newer-version reject 已测；无旧 catalog 迁移 |
| Reference-only import | common/imageio/import | services + adapters | 实现中 | PNG/JPEG + LibRaw RAW（含 ARW）与目录递归已测；JPEG/GIF/WebP/TIFF plugin targets 已设为 required |
| Preview cache | mipmap/cache/imageio | services + adapters | 实现中 | 库外原子 PNG 缓存与 reopen 重建已测 |
| Gallery/viewer | lighttable/darkroom | desktop + services | 实现中 | Studio 可创建/打开/导入/fit/100%；平移与长列表仍属 M2 |
| Catalog metadata/workflow | common/libs | domain + services | 延后 | M4 产品与数据契约 |
| Mask/blend/operations | develop/iop | recipe + engine | 延后 | M5 每项 operation 验收 |
| 本地导出 | imageio / `libs/export.c` | services + raster encoder + CLI/Studio | 旧实现已删除 | JPEG/PNG/原片复制与 conflict/cancel 已测；TIFF plugin target 已设为 required；metadata/ICC 未做。旧 `libs/export*.c` 已删 |
| 色调曲线 | `iop/tonecurve.c` | `ravo.core.tonecurve` + Develop Inspector | 旧实现已删除 | 一条链接 RGB 点曲线；schema 显式 `working_space=srgb\|linear_rgb`。不是 Lab 三通道移植。`rgbcurve` 未做 |
| 默认显示变换 | `iop/sigmoid.c` | `ravo.display.sigmoid` + RAW baseline + Develop Inspector | 旧实现已删除 | per-channel generalized log-logistic；线性 sRGB、Standard SDR target、合成/真实 RAW/catalog reopen 已测。`filmicrgb`/`agx` 明确留作 leftover |
| CLI | `src/cli` | cli | 实现中 | engine/recipe JSON 已测；catalog 命令计划在 M2 |
| GPU | OpenCL/pixelpipe | engine adapter | 延后 | M6，CPU goldens 和收益证明后开始 |

状态只使用“未开始 / 基线冻结 / 实现中 / Ravo 已验收 / 旧实现已删除 / 延后 / 不支持”。旧 owner
的物理删除以 [`../TODO.md`](../TODO.md) 该项验收为准，不是整包等到 M7。
