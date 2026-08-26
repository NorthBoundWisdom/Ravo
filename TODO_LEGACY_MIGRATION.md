# Ravo Legacy Migration TODO

> **状态：in progress**
>
> **更新日期：2026-08-26**
>
> **当前执行焦点：C3 RAW 白平衡 `temperature`。** 不得并行迁
> `colorin`、`colorout`、`cacorrectrgb` 或通用 mask graph。

本文只记录尚未完成的执行工作、风险、依赖、验证命令和完成门槛。当前能力、
架构、迁移政策、leftover 边界和测试合同分别以
[`Ravo/README.md`](Ravo/README.md)、
[`Ravo/ARCHITECTURE.md`](Ravo/ARCHITECTURE.md)、
[`Ravo/MIGRATION.md`](Ravo/MIGRATION.md) 和
[`Ravo/TESTING.md`](Ravo/TESTING.md) 为真相源。尚未具备执行门槛的能力只记录在
[`DevDocs/ProductRoadmap.md`](DevDocs/ProductRoadmap.md)。

## 1. 执行规则

- 一次只推进队列中的第一项；未完成前不得并行开下一项。
- 已迁入 engine/catalog 但未达到本节门槛的能力，不算「Ravo 已验收」。
- 每项先静态读取旧 owner/fixture，再定义 Ravo owner、生命周期、失败/取消路径和最小验证集。
- 算法项必须复刻冻结 C 的默认 CPU 路径（公式、色彩空间、滤波器、默认 mode），再按下线门槛删除旧 owner。
  不得用简化替代算法充当「已迁入」或删除旧实现的理由。允许去掉 GUI、旧 lifecycle、OpenCL 与动态 ABI；
  不允许替换核心数学。
- 「Ravo 已验收」和旧 owner 删除门槛统一按 `Ravo/MIGRATION.md`；门槛未满足只能改 Ravo。
- 完成一项时，把长期结论同步到 README/ARCHITECTURE/MIGRATION/TESTING/ADR/代码或测试，
  然后从本文删除该项；不要留下 `[x]` 历史或归档 TODO。
- 可靠性 finding 可以阻断当前项，但不得借机批量清理 GTK/OpenCL/共享 imageio/fixture。
- 未运行的平台和手工检查必须写成「未验证」，不得复用历史结果冒充本轮通过。

## 2. 迁移队列

### C3. RAW 白平衡：`temperature`（当前）

目标：把冻结 `temperature.c` 的逐 CFA channel scaling 迁到 Ravo-owned RAW preprocess，
消除当前 decoder 内隐藏 as-shot 乘法与 demosaic 后 Kelvin/tint 近似的双重 ownership，让 CLI、
CatalogService、export 与 Studio 共用同一白平衡合同。

范围：

- versioned schema 必须显式区分 `as_shot`、`camera_reference`、
  `as_shot_to_reference` 与 resolved manual coefficients；冻结 red/green/blue/fourth
  `[0,8]` channel 参数和 mode/preset 语义不得被二参数 Kelvin RGB 近似替代；
- RAW 默认 as-shot 系数来自已验证 LibRaw metadata；camera reference 只能由明确 camera matrix/
  versioned preset 解析，缺失时 fail-fast，不保留冻结硬编码 2/1/1.5 安全网；
- CPU 保留 Bayer、X-Trans 与 non-mosaiced 三条 channel-scaling 数学；Ravo 当前不支持的 sensor/
  demosaic 组合仍返回结构化 unsupported，不能在 RGB 后补救；
- 必须先决定 `DecodedRaw::white_balance`、`ravo.color.channelmixerrgb` 与 late
  reference correction 的唯一 owner，证明 RAW as-shot 不会在 decode 与 recipe 各乘一次；
- GTK spot picker、相机 preset combobox/彩色 slider、OpenCL、动态 IOP ABI 与旧 preset/XMP ABI 不迁；
  spot 结果若保留能力，只能保存为已解析的 canonical coefficients。

owner / 生命周期 / 失败：

- coefficients/mode schema 属于 recipe；camera/as-shot metadata 和 CFA 执行属于 engine raw pipeline；
  services 只持久/编排，QML 只展示 resolved state 并转发意图；
- 每次 decode 得到不可变 source metadata，recipe 产生 owned CFA 输出并进入 preprocess cache key；
  不缓存 LibRaw、GTK、旧 preset 表或全局 chroma 指针；
- 系数缺失/零值/非有限/越界、未知 camera reference、非法 mode/sensor、内存失败和取消都在发布前返回
  结构化错误；不得静默使用 generic 系数或发布部分 CFA。

验证与完成门槛：

- [ ] 全 schema round-trip；未知字段、非法 mode、非有限/零/越界系数与缺失 camera reference fail-fast；
- [ ] Bayer/X-Trans/non-mosaiced channel mapping、第四通道、逐行取消、输入 immutability 与 cache-key UT；
- [ ] 静态解码 `0000-nop`/代表性 RAW history 的 temperature blob，迁移前后
  `mire1.cr2` 默认 as-shot reference 保持在记录容差内，并覆盖 camera reference/manual look；
- [ ] `channelmixerrgb` 组合 UT 证明 WB 不重复，late reference mode 的 stage/参数 ownership 明确；
- [ ] Studio White Balance Inspector 保存/reopen 由 presenter/QML smoke + catalog integration 自动验证；
- [ ] 删除 `legacy/src/iop/temperature.c` 及注册；共享 preset/color science 只在消费者清零后退役；
- [ ] 完整 Ravo unit/contract/catalog 测试和 freeze/inventory/boundary 检查通过；
- [ ] Windows/Linux 未跑时明确写成未验证。

最小命令：

```text
cmake --build --preset mac_clang_debug --target \
  ravo_unit_tests ravo_contract_tests ravo_catalog_tests ravo_desktop_command_tests ravo_studio

./build/mac_clang_debug/Ravo/tests/ravo_unit_tests
./build/mac_clang_debug/Ravo/tests/ravo_contract_tests
./build/mac_clang_debug/Ravo/tests/ravo_catalog_tests
./build/mac_clang_debug/Ravo/tests/ravo_desktop_command_tests

python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
```

## 3. 完整剩余模块清单与串行顺序

本节是当前 working tree 的执行清单，不是长期 capability 真相源。快照基线：

- `legacy/src/iop/CMakeLists.txt` 仍注册 65 个 IOP；每个都在 3.2 单独列出；
- `legacy/src/libs/CMakeLists.txt` 有 23 个仍有源码的模块/工具，另有已退役源码的失效注册；
- `legacy/src/views` 有 `darkroom` / `lighttable` 两个旧 view；imageio 有 4 个 format、1 个 storage 和
  9 类 dispatcher/decoder owner；
- `common` / `control` / `develop` / GUI 与 host 资源按 3.3–3.7 的 ownership 单元列出；
- `legacy/tests` 的 158 组 fixture 与 5 个原图在算法迁移期间保持只读，旧 runner 不运行。

状态约定：`当前` 只有 C3；`排队` 必须等表中依赖和所有更早项；`删除` 表示不移植 UI/ABI；`保留证据`
表示迁移完成前不得移动。任何模块达到门槛后先同步稳定真相源，再从本节删除该行。

通用完成门槛：

- **ALG**：静态读 owner/fixture → versioned schema/工作空间/ROI → 完整默认 CPU 数学 → synthetic + 真实
  fixture + 错误/取消/资源 UT → CLI/Studio/service 正式消费 → 删除源码、注册、专用 helper/kernel/resource；
- **CORE**：列出所有消费者，能力进入 foundation/domain/services/engine 私有 owner；线程/缓存/事务生命周期
  有 contract；消费者清零后删除共享 C/全局状态；
- **DELETE**：全仓证明没有未验收算法消费者，删除 CMake/动态加载/GTK 资源/配置键/文档引用，不创建 Qt
  fallback 或空壳；
- **DATA**：先把仍需的 fixture/schema/resource 迁到 Ravo 真相源并校验 hash/round-trip，再删除旧 runner/资产。

### 3.1 已退役 owner 的残留清理

| ID | 模块 / owner | 动作 | 依赖与完成门槛 |
| --- | --- | --- | --- |
| D0.1 | `iop/hlreconstruct/*` | 删除 | `highlights.c` 已退役；确认无消费者，补入 retired list，freeze check 通过 |
| D0.2 | `libs/CMakeLists.txt` 中 `export`、`copy_history`、`tagging`、`metadata*`、`history`、`snapshots` 失效项 | 删除注册 | 不修改其他冻结模块；全仓确认对应源码已在 retired list |
| D0.3 | `host/data/kernels` 中已退役 IOP 的独占 kernel/entry | 删除 | 先区分共享 `extended.cl`/color helpers；只删无剩余消费者的 entry 与 programs 注册 |
| D0.4 | `common/iop_order.c`、`libs/modulegroups.c`、`usermanual_url.c` 的已退役名字 | 最终删除引用 | 这些文件仍服务旧 UI/registry；等其所有算法消费者清零后随 DELETE 批次处理 |
| D0.5 | 无消费者的 `iop/choleski.h`、`equalizer_eaw.h`、`svd.h`、未注册 `useless.c` | 删除 | 再次全仓搜索确认无 include/target/fixture owner，补 retired list 与 freeze check |

### 3.2 IOP 算法队列（65 个注册模块）

以下表的组顺序也是依赖顺序；同组默认按行串行。`fixture` 来自冻结 manifest，只表示有静态证据，不表示已覆盖。

#### A. 当前颜色调度与颜色基础

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| C3 | `temperature` — `iop/temperature.c` | yes | **当前 / ALG**；明确 RAW WB 与 `channelmixerrgb` ownership，camera/as-shot/reference 模式，详见第 2 节 |
| C4 | `colorin` — `iop/colorin.c` | yes | 排队 / ALG；依赖 S1，ICC/input profile、matrix/LUT、unsupported profile contract |
| C5 | `colorout` — `iop/colorout.c` | yes | 排队 / ALG；依赖 C4/O1，输出 profile、intent、soft-proof 与 export 一致性 |
| C6 | `primaries` — `iop/primaries.c` | yes | 排队 / ALG；依赖 C4，custom primaries/white point/gamut mapping |
| C7 | `profile_gamma` — `iop/profile_gamma.c` | no | 排队 / ALG；依赖 C4，无 fixture 时先提交 Ravo-owned synthetic + RAW reference |
| C8 | `gamma` — `iop/gamma.c` | yes | 排队 / ALG；现有简化 `ravo.core.gamma` 不算验收，复刻冻结 lookup/边界 |
| C9 | `exposure` — `iop/exposure.c` | yes | 排队 / ALG；补齐 automatic/black/deflicker与 mask 语义，现有手动 EV 只是子集 |
| C10 | `colorbalance` — `iop/colorbalance.c` | yes | 排队 / ALG；与 `ravo.color.colorbalancergb` 明确 overlap，不用三参数替代旧完整路径 |
| C11 | `colorchecker` — `iop/colorchecker.c` | yes | 排队 / ALG；依赖 C4/S1；算法/校准表迁入，GTK chart/picker 删除 |
| C12 | `colorcorrection` — `iop/colorcorrection.c` | yes | 排队 / ALG；依赖 S1/M1，Lab/色度与 blend contract |
| C13 | `colorcontrast` — `iop/colorcontrast.c` | yes | 排队 / ALG；现有简化 control 不算验收，复刻冻结色彩空间数学 |
| C14 | `colorharmonizer` — `iop/colorharmonizer.c` | yes | 排队 / ALG；依赖 S1/M1，色彩和谐与 mask/ROI |
| C15 | `colorize` — `iop/colorize.c` | yes | 排队 / ALG；依赖 S1/M1，Lab hue/saturation/source mix |
| C16 | `colormapping` — `iop/colormapping.c` | yes | 排队 / ALG；依赖 S1，source/target statistics、cluster 与确定性 |
| C17 | `colorzones` — `iop/colorzones.c` | yes | 排队 / ALG；`colorequal` 保持默认；迁入完整可选 HSL/Lab 分区与曲线 |
| C18 | `monochrome` — `iop/monochrome.c` | yes | 排队 / ALG；现有单 amount 不算验收，复刻 channel filter 与色彩空间 |
| C19 | `lowlight` — `iop/lowlight.c` | yes | 排队 / ALG；依赖 S1，低光色觉曲线与 LUT |
| C20 | `splittoning` — `iop/splittoning.c` | yes | 排队 / ALG；现有简化 split toning 不算验收，复刻 balance/compress 路径 |
| C21 | `velvia` — `iop/velvia.c` | yes | 排队 / ALG；现有简化 velvia 不算验收，复刻 luminance/saturation weighting |

#### B. 显示变换、曲线与 LUT

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| T1 | `basecurve` — `iop/basecurve.c` | yes | 排队 / ALG；依赖 C4，camera preset、exposure fusion、曲线插值 |
| T2 | `rgbcurve` — `iop/rgbcurve.c` | yes | 排队 / ALG；独立于已迁 `tonecurve`，复刻 linked/independent/preserve-color modes |
| T3 | `rgblevels` — `iop/rgblevels.c` | yes | 排队 / ALG；auto/manual、linked channels、picker UI 删除 |
| T4 | `filmicrgb` — `iop/filmicrgb.c` | yes | 排队 / ALG；Sigmoid 保持默认；完整 scene/display、chroma/gamut/reconstruct modes |
| T5 | `agx` — `iop/agx.c` | yes | 排队 / ALG；Sigmoid 保持默认；AgX curve、primaries与 gamut path |
| T6 | `lut3d` — `iop/lut3d.c` | yes | 排队 / ALG；依赖 C4/I/O，LUT format adapter、missing/invalid file、interpolation |
| T7 | `negadoctor` — `iop/negadoctor.c` | yes | 排队 / ALG；negative input、film base、scanner/profile 与 picker UI 分离 |

#### C. RAW preprocess 与 demosaic

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| R1 | `rawprepare` — `iop/rawprepare.c` | yes | 排队 / ALG；补齐 crop/black/white/CFA/orientation，取代当前 absorbed 子集 |
| R2 | `demosaic` — `iop/demosaic.c` + `iop/demosaicing/*` | yes | 排队 / ALG；Bayer/X-Trans 各 mode、dual/green matching、memory/ROI；基础 3×3 只是子集 |
| R3 | `rawdenoise` — `iop/rawdenoise.c` | yes | 排队 / ALG；pre-demosaic wavelet/threshold 与 sensor reject |
| R4 | `cacorrectrgb` — `iop/cacorrectrgb.c` | no | 排队 / ALG；与已迁 pre-demosaic `cacorrect` 分离；先建立 synthetic/RAW fixture |
| R5 | `colorreconstruct` — `iop/colorreconstruction.c` | yes | 排队 / ALG；依赖 R2/C4，完整高光颜色传播/ROI |
| R6 | `rasterfile` — `iop/rasterfile.c` | no | 排队 / ALG；依赖 I1/M1，raster-source/mask ownership 和无 fixture 基线 |

#### D. 几何、画布与尺度

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| G1 | `flip` — `iop/flip.c` | yes | 排队 / ALG；补齐 EXIF/orientation/ROI，现有 mirror/quarter-turn 只是子集 |
| G2 | `rotatepixels` — `iop/rotatepixels.c` | no | 排队 / ALG；依赖 G1，sensor/pixel rotation synthetic fixture |
| G3 | `scalepixels` — `iop/scalepixels.c` | no | 排队 / ALG；依赖 S3，插值/ROI/scale contract 与 synthetic fixture |
| G4 | `crop` — `iop/crop.c` | yes | 排队 / ALG；依赖 M1/G1，完整 aspect/keystone/ROI；现有 normalized crop 只是子集 |
| G5 | `enlargecanvas` — `iop/enlargecanvas.c` | yes | 排队 / ALG；canvas coordinate、fill/alpha、mask transform |
| G6 | `ashift` — `iop/ashift.c` + `ashift_lsd.c` + `ashift_nmsimplex.c` | yes | 排队 / ALG；line detection、lens geometry、auto/manual fit；现有 straighten 不是替代 |
| G7 | `finalscale` — `iop/finalscale.c` | no | 排队 / ALG；依赖 G3/O1，正式 resampling/output-size contract |
| G8 | `borders` — `iop/borders.c` | yes | 排队 / ALG；依赖 G5/O1，frame/aspect/color/metadata 与 export |
| G9 | `liquify` — `iop/liquify.c` | yes | 排队 / ALG；依赖 M1/G4，deformation graph、ROI 与 cancellation |

#### E. Detail、降噪、模糊与局部对比

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| F1 | `sharpen` — `iop/sharpen.c` | yes | 排队 / ALG；现有简化 USM 不算验收，复刻 blur/threshold/ROI |
| F2 | `highpass` — `iop/highpass.c` | yes | 排队 / ALG；依赖 S3/M1，Lab/RGB blend 与 contrast path |
| F3 | `lowpass` — `iop/lowpass.c` | yes | 排队 / ALG；依赖 S3/M1，Gaussian/bilateral modes 与 saturation |
| F4 | `shadhi` — `iop/shadhi.c` | yes | 排队 / ALG；依赖 S3，bilateral shadows/highlights 路径 |
| F5 | `atrous` — `iop/atrous.c` | yes | 排队 / ALG；完整 a-trous 多尺度 bands、boost/threshold、mask |
| F6 | `bilat` — `iop/bilat.c` | yes | 排队 / ALG；冻结快速 bilateral grid，不与 `bilateral` 合并猜测 |
| F7 | `bilateral` — `iop/bilateral.cc` + `Permutohedral.h` | yes | 排队 / ALG；permutohedral lattice、memory budget、CPU determinism |
| F8 | `nlmeans` — `iop/nlmeans.c` | yes | 排队 / ALG；依赖 S3，patch/search/scattering 与取消 |
| F9 | `diffuse` — `iop/diffuse.c` | yes | 排队 / ALG；多迭代 anisotropic diffusion、scale/ROI、blending |
| F10 | `blurs` — `iop/blurs.c` | yes | 排队 / ALG；Gaussian/lens/motion modes，不用单一 blur 替代 |
| F11 | `hazeremoval` — `iop/hazeremoval.c` | yes | 排队 / ALG；atmospheric model、distance estimate、guided filter |
| F12 | `soften` — `iop/soften.c` | yes | 排队 / ALG；现有简化 soften 不算验收，复刻 blur/mix/color path |

#### F. Mask、合成、修复、效果与诊断

| ID | IOP / owner | fixture | 状态 / 依赖 / 特有门槛 |
| --- | --- | --- | --- |
| M1 | `mask_manager` — `iop/mask_manager.c` | yes | 排队 / ALG+CORE；先完成 3.3 S3 canonical mask/blend graph |
| M2 | `retouch` — `iop/retouch.c` | yes | 排队 / ALG；依赖 M1/S2，clone/heal/blur/fill、source geometry |
| M3 | `overlay` — `iop/overlay.c` | yes | 排队 / ALG；依赖 M1/I1/G5，资源生命周期与 alpha composition |
| M4 | `censorize` — `iop/censorize.c` | yes | 排队 / ALG；依赖 M1/S2，pixelate/blur/noise modes |
| M5 | `watermark` — `iop/watermark.c` | yes | 排队 / ALG+DATA；依赖 M1/I1，SVG/text/font/metadata resource determinism |
| M6 | `bloom` — `iop/bloom.c` | yes | 排队 / ALG；现有简化 bloom 不算验收，复刻 threshold/blur/mix |
| M7 | `grain` — `iop/grain.c` | yes | 排队 / ALG；现有 deterministic noise 不算验收，复刻 Lab/ISO/channel mode |
| M8 | `vignette` — `iop/vignette.c` | yes | 排队 / ALG；现有简化 radial darkening 不算验收，复刻 shape/dither/colors |
| O1 | `dither` — `iop/dither.c` | yes | 排队 / ALG；quantization method、bit depth、deterministic random 与 export |
| O2 | `overexposed` — `iop/overexposed.c` | no | 排队 / ALG；只迁诊断计算/阈值，GTK overlay presentation 删除，建 synthetic fixture |
| O3 | `rawoverexposed` — `iop/rawoverexposed.c` | no | 排队 / ALG；RAW CFA threshold/channel diagnostic，建 synthetic fixture |

### 3.3 Shared algorithm、pixelpipe、mask 与 domain owner

| ID | owner 路径 | 动作 | 依赖与完成门槛 |
| --- | --- | --- | --- |
| S1 | `common/colorspaces*`、`chromatic_adaptation.h`、`illuminants.h`、`matrices*`、`custom_primaries*`、`gamut_mapping.h`、`darktable_ucs_22_helpers.h`、`color_*`、`colorchecker.h`、`curve_tools*`、`wb_presets*` | CORE 迁入 engine 私有 color science | C3–C21/T1–T7 的显式 workspace/LUT owner；无 GTK/LCMS 裸类型越界 |
| S2 | `common/bilateral*`、`box_filters*`、`distance_transform*`、`dwt*`、`eaw*`、`eigf.h`、`gaussian*`、`guided_filter*`、`fast_guided_filter.h`、`heal*`、`locallaplacian*`、`nlmeans_core*`、`splines*`、`interpolation*`、`noiseprofiles*`、`bspline.h`、`luminance_mask.h`、`rgb_norms.h`、`focus*`、`histogram*`、`develop/noise_generator.h`、`develop/openmp_maths.h` | CORE 迁入 engine primitives | F/G/M/diagnostic 批次逐消费者验收；OpenCL twin 不迁 |
| S3 | `develop/blend*`、`develop/blends/*`、`develop/masks/*`、`develop/masks.h` | CORE 建 canonical mask/blend graph | shape/group/coordinate/parametric blend/ROI/schema/取消 UT；M1 前置 |
| S4 | `develop/develop*`、`pixelpipe*`、`pixelpipe_cache*`、`pixelpipe_hb*`、`tiling*`、`imageop*`、`format*`、`borders_helper*` | CORE 收敛到 Engine facade + services cache/scheduler | 每个旧 consumer 清零；不复制动态 pixelpipe/global state |
| S5 | `iop/iop_api.h`、`common/module*`、`module_api.h`、`dynload*`、`introspection.h`、`action.h`、`darktable*`、`darktable_api.h`、`poison.h`、`iop_group*`、`iop_order*`、`iop_profile*` | DELETE 动态 IOP/module ABI 与全局 composition | 65 个 IOP 全部退役后删除；operation registry 保持内建 versioned schema |
| S6 | `common/database*`、`database_schema*`、`sqliteicu*` | CORE/DELETE | 对照 Ravo schema/FTS/ICU；需要的数据 contract 迁入 SQLite adapter，其余旧 catalog ABI 删除 |
| S7 | `common/image*`、`film*`、`import_session*`、`grouping*` | CORE/DELETE | Asset/import/folder/group use case contract 覆盖后删除全局 image/film owner |
| S8 | `common/collection*`、`selection*`、`ratings*`、`colorlabels*`、`act_on*` | CORE/DELETE | Ravo LibraryQuery/selection/review 覆盖；缺失 collection 查询逐项补 UT |
| S9 | `common/exif*`、`metadata*`、`metadata_export*`、`tags*` | CORE/DATA | capture/writable/ICC/export metadata schema，原片只读；Exiv2 类型留 adapter 私有 |
| S10 | `common/history*`、`history_snapshot*`、`styles*`、`presets*`、`undo*` | CORE/DATA | canonical recipe history/style/preset import/reject 与 rollback；GUI preset owner 删除 |
| S11 | `common/cache*`、`image_cache*`、`mipmap_cache*`、`imagebuf*` | CORE/DELETE | 明确 byte budget/LRU/atomic publication；由 CatalogService/PreviewCache 取代 |
| S12 | `common/atomic*`、`dtpthread*`、`resource_limits*`、`system_signal_handling*`、`utility*`、`datetime*`、`variables*`、`calculator*`、`file_location*`、`math.h`、`points.h`、`heap.h`、`tea.h`、`dttypes.h`、`debug.h`、`extra_optimizations.h`、`sse.h`、`grealpath.h`、`win_file_trash*` | CORE/DELETE | 仅迁仍有 Ravo consumer 的值/线程/路径/平台逻辑；其余随全局 core 删除 |
| S13 | `common/curl_tools*`、`dbus*`、`gimp*`、`pwstorage/*`、`overlay*` | DELETE 或 adapter | 只有 active IOP/service 明确需要才建独立 port；不得保留远程发布/UI 凭据壳 |
| S14 | `common/opencl*`、`dlopencl*`、`opencl_drivers_blacklist.h` | DELETE | CPU goldens 完成后删除；Ravo GPU 不复用 OpenCL API |

### 3.4 Codec、imageio 与输出插件

| ID | owner | 动作 | 完成门槛 |
| --- | --- | --- | --- |
| I1 | `imageio/imageio.c`、`imageio_module*`、`imageio_common.h` | CORE/DELETE dispatcher | 所有 format/storage 走 Ravo ports；动态 imageio ABI 与全局 registry 删除 |
| I2 | `imageio_libraw*` | adapter audit | Ravo pinned LibRaw 覆盖 RAW metadata/embedded/decode/sensor errors 后删除旧 wrapper |
| I3 | `imageio_rawspeed*` + external RawSpeed wiring | DELETE 或独立 decoder | 先做格式/性能/fixture 决定；不得与 LibRaw 静默 fallback |
| I4 | `imageio_dng*`、`common/dng_opcode*` | ALG/adapter | DNG opcode/crop/black/metadata fixture；LibRaw 已处理部分不能冒充全覆盖 |
| I5 | `imageio_jpeg*` | adapter audit | orientation/ICC/alpha/errors 与 Qt decoder contract；无消费者后删旧 libjpeg wrapper |
| I6 | `imageio_png*` | adapter audit | bit depth/ICC/alpha/errors 与 Qt decoder contract |
| I7 | `imageio_tiff*` | adapter audit | 8/16/float、multi-page、ICC/alpha/errors 与 Qt contract |
| I8 | `imageio_qoi*` + `qoi.h` | ALG/adapter | 若 Ravo 保留 QOI，补 decoder/encoder fixture；否则明确 unsupported 后删除 |
| I9 | `imageio_rgbe*` | ALG/adapter | HDR RGBE decode/color contract 与 fixture；不得当普通 raster |
| I10 | `imageio/format/copy.c` | DELETE/复用 original-copy service | 原子复制、conflict/cancel 已测后清理动态 format ABI |
| I11 | `imageio/format/jpeg.c` | adapter/export | quality/ICC/metadata/subsampling/disk-full contract |
| I12 | `imageio/format/png.c` | adapter/export | bit depth/ICC/metadata/alpha/disk-full contract |
| I13 | `imageio/format/tiff.c` | adapter/export | 8/16/float/compression/ICC/metadata/disk-full contract |
| I14 | `imageio/storage/disk.c` | DELETE/复用 CatalogService export | path template/conflict/cancel/atomic write 覆盖；动态 storage ABI 删除 |
| I15 | `external/CMakeLists.txt`、`external/LibRaw-cmake`、`cie_colorimetric_tables.c`、`ThreadSafetyAnalysis.h` | DATA/DELETE | 必要 table 迁入 owner；依赖只走 FreeCM source-root，删 vendored/build shim |

### 3.5 Control、jobs 与应用生命周期

| ID | owner | 动作 | 完成门槛 |
| --- | --- | --- | --- |
| J1 | `control/control*`、`jobs*`、`progress*`、`signal*` | CORE/DELETE | SerialExecutor/task handle/cancel/progress/close 资源合同覆盖；无 detached/global controller |
| J2 | `control/jobs/control_jobs*` | DELETE/映射 use case | command/service contract 覆盖后删除旧 job wrapper |
| J3 | `control/jobs/develop_jobs*` | CORE/DELETE | preview/render/export queue、supersede/cancel/close UT |
| J4 | `control/jobs/film_jobs*` | CORE/DELETE | folder/import batch/reopen 由 CatalogService 覆盖 |
| J5 | `control/jobs/image_jobs*` | CORE/DELETE | asset mutation、duplicate/missing/errors 的 service contract |
| J6 | `control/jobs/sidecar_jobs*` | DATA/DELETE | XMP read/write policy、原片安全、conflict/rollback 明确后删除 |
| J7 | `control/conf*`、`settings.h`、`crawler*` | DELETE 或 Ravo settings port | 只迁有产品 consumer 的 typed setting；不保留旧 key compatibility 壳 |

### 3.6 旧 UI、views 与 Lighttable/libs（删除，不移植）

| ID | 模块 / owner | 动作 | 删除门槛 |
| --- | --- | --- | --- |
| U1 | `bauhaus/*` | DELETE | 所有 IOP UI 已由 Studio 或无 UI recipe 消费；不建 Qt/Bauhaus adapter |
| U2 | `dtgtk/*` | DELETE | button/range/expander/thumbnail/thumbtable/culling 等无算法消费者 |
| U3 | `gui/gtk*`、`workspace*`、`accelerators*`、`context_menu*`、`system_commands*` | DELETE | Studio command registry/composition 已覆盖所需意图 |
| U4 | `gui/color_picker_proxy*`、`guides*`、`hist_dialog*`、`import_metadata*`、`metadata_tags*`、`log_history*` | DELETE | 所需计算/metadata service 先迁；GTK dialog/picker 不迁 |
| U5 | `gui/preferences*`、`presets*`、`styles_dialog*`、`about*`、`splash*` | DELETE/DATA | typed settings/style schema 先完成；旧窗口与资源删除 |
| U6 | `views/darkroom.c` | DELETE | 所有 Develop consumer 在 Studio，旧 view/module ABI 清零 |
| U7 | `views/lighttable.c` | DELETE | Gallery/import/review consumer 在 Studio，旧 view/module ABI 清零 |
| U8 | `views/view*` | DELETE | U6/U7 与动态 view loader 删除后清理 |
| L1 | `libs/import.c` | DELETE | import service + Studio/CLI contract 覆盖 |
| L2 | `libs/styles.c` | DATA/DELETE | style schema/import/reject 完成 |
| L3 | `libs/image.c` | DELETE | Asset actions/metadata/review service 覆盖 |
| L4 | `libs/select.c` | DELETE | Studio multi/range selection UT 覆盖 |
| L5 | `libs/recentcollect.c` | DELETE | catalog recent/filter product decision与 query contract |
| L6 | `libs/filtering.c` + `libs/filters/*` | CORE/DELETE | 每个过滤字段（aperture/color/date/duplicate/exposure/filename/focal/history/ISO/local-copy/module-order/rating/ratio/search）映射 LibraryQuery 或明确 unsupported |
| L7 | `libs/navigation.c` | DELETE | Studio zoom/pan/navigation state 覆盖 |
| L8 | `libs/histogram.c` + `libs/scopes/*` | CORE/DELETE | RGB histogram/waveform/vectorscope/split 各自 UT；现有 histogram/parade 只是子集 |
| L9 | `libs/modulegroups.c` + header | DELETE | Studio Inspector grouping 完成后删除旧模块名/quick-access 配置 |
| L10 | `libs/backgroundjobs.c` | DELETE | J1 progress/task presentation 覆盖 |
| L11 | `libs/masks.c` | DELETE | S3/M1 canonical mask service + Studio intent 覆盖 |
| L12 | `libs/ioporder.c` | DELETE | canonical recipe operation order/version contract 覆盖 |
| L13 | `libs/tools/viewswitcher.c` | DELETE | Studio Gallery/Edit mode command 覆盖 |
| L14 | `libs/tools/darktable.c` | DELETE | 旧品牌/label 工具无消费者 |
| L15 | `libs/tools/flags.c` | DELETE | reject/flag review state 覆盖 |
| L16 | `libs/tools/colorlabels.c` | DELETE | color label service/Studio 覆盖 |
| L17 | `libs/tools/ratings.c` | DELETE | rating service/Studio 覆盖 |
| L18 | `libs/tools/lighttable.c` | DELETE | Gallery mode command 覆盖 |
| L19 | `libs/tools/view_toolbox.c` | DELETE | Studio view commands 覆盖 |
| L20 | `libs/tools/module_toolbox.c` | DELETE | Studio Inspector/command registry 覆盖 |
| L21 | `libs/tools/filmstrip.c` | DELETE | Studio filmstrip/model contract 覆盖 |
| L22 | `libs/tools/hinter.c` | DELETE | Studio visible status/error presentation 覆盖 |
| L23 | `libs/tools/image_infos.c` | DELETE | Asset metadata presenter 覆盖 |
| U9 | `libs/lib*` / `lib_api.h` | DELETE | L1–L23 清零后删除动态 Lighttable module loader |
| U10 | `main.c`、`cli/main.c`、`src/CMakeLists.txt`、`config.cmake.h`、`strings.h` 旧 app/CLI target | DELETE | Ravo CLI/Studio/package 三平台闭环，无旧入口消费者 |
| U11 | `osx/*`、`win_msvc_compat.h`、`unistd.h`、launcher/plist templates | DELETE | Ravo 平台 composition/package 已覆盖 |

### 3.7 Host resources、测试证据与最终目录清理

| ID | 路径 | 动作 | 完成门槛 |
| --- | --- | --- | --- |
| H1 | `host/data/kernels/*` | DELETE | 对应 CPU ALG 全部验收；必要 GPU 数学只从 Ravo CPU 真相源重写，不复用 OpenCL |
| H2 | `host/data/noiseprofiles*`、`wb_presets*`、color tables | DATA | 迁入 versioned Ravo calibration resource + schema/checksum，或对应能力明确不需要后删除 |
| H3 | `host/data/styles/*` | DATA | canonical style import/reject fixture 后迁入 Ravo tests 或删除 |
| H4 | `host/data/watermarks/*` | DATA | M5 所需资源迁入 versioned test/product assets；未用资源删除 |
| H5 | `host/data/themes/*`、`pixmaps/*`、`shortcutsrc`、darktable config XML/DTD | DELETE | Studio theme/icon/command truth source 已覆盖；不保留 GTK 资源 |
| H6 | `host/packaging/*`、`host/cmake/*`、host tools/scripts/CMake | DELETE | RavoPackage 三平台安装闭环通过后删除旧构建/打包逻辑 |
| E1 | `legacy/tests` 原图、XMP、expected PNG | 保留证据 → DATA | 每个 IOP 行完成后将仍需 golden/metadata 摘要迁入 Ravo fixture truth source并校验 hash；队列清空前只读 |
| E2 | `legacy/src/tests/*`、`legacy/tests/run`、check/delta/performance binaries/scripts | DELETE | Ravo regression/performance/sanitizer 入口存在；从未运行旧 test target/runner |
| E3 | `legacy/benchmarks/*` | DELETE/DATA | 有效门槛吸收到 `DevDocs/GPU_Baseline.md`，旧脚本不执行 |
| E4 | `legacy/docs/*`、`RELEASE_NOTES.md`、legacy README | 保留证据 → DELETE | 所有算法/ownership 结论进入 ADR/ARCHITECTURE/TESTING 后删除旧源码地图 |
| E5 | `legacy/host`、`legacy/src` 空目录与根 `legacy/` | DELETE | 所有 ALG/CORE/DATA/DELETE 行清空、fixture 迁出、全仓链接与 package 检查通过 |

### 3.8 横向可靠性与发行阻塞

以下均未完成；它们不构成第二条功能队列，但可阻断受影响项或发行。

- [ ] Gallery thumbnail 虚拟化、长列表内存、worker/preview/cache 明确预算；
- [ ] 数据库不可写/损坏、cache 损坏、磁盘满、源移动、批量取消、崩溃重开；
- [ ] schema migration fixture、导入到首帧指标、窗口/catalog 关闭后的资源销毁；
- [ ] 键盘、焦点、HiDPI、可访问性；
- [ ] Windows/macOS/Linux configure/build/test、staged install 和安装目录闭环；
- [ ] metadata/ICC 与输出颜色合同达到承诺格式门槛；
- [ ] 指定 packaged-release product owner 与 code-review owner；
- [ ] 发行切换前证明备份/回滚、原片安全和无 legacy 生产依赖。

GPU 不进入当前队列。只有相关 CPU 路径验收、金样稳定且端到端测量证明收益后，
才能按 [`DevDocs/GPU_Baseline.md`](DevDocs/GPU_Baseline.md) 新建专项 TODO。

3.1–3.7 已列出当前全部剩余模块；[`DevDocs/ProductRoadmap.md`](DevDocs/ProductRoadmap.md) 只保留
尚未冻结的跨层设计约束，不能据此从本 TODO 隐去模块。C3 完成前不并行实施后续行。

## 4. 本 TODO 完成门槛

删除本文前必须同时成立：

- [ ] C3 及后续逐项提升的算法已验收并从本文移除，已接受的旧 owner 同变更退役；
- [ ] 共享旧 owner 只剩明确消费者，剩余树均能映射到 `Ravo/MIGRATION.md` leftover；
- [ ] 横向可靠性和承诺平台安装闭环达到发布门槛，或拆成有 owner 的独立根 TODO；
- [ ] 原片安全、schema migration、备份/回滚和结构化 unsupported 有测试证据；
- [ ] 全仓搜索、target/link 图和运行验收证明不存在 Ravo ↔ legacy 生产依赖；
- [ ] 长期结论已同步到 owning docs/ADR/代码/manifest/checker/test；
- [ ] 本文不再包含未完成项，然后直接删除，不移入 `DevDocs/` 归档。
