# Ravo Legacy Migration TODO

> **状态：in progress**
>
> **更新日期：2026-08-25**
>
> **当前执行焦点：C1 收口已迁入的 L2–L9。** 下一功能项仍是 L1
> `channelmixerrgb`。未完成 C1 前不得新开 IOP，也不得并行 L1。

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

### C1. 收口已迁入的 L2–L9（当前）

目标：让上一轮迁入的能力在 Studio/CLI 走同一条可信路径，而不是只存在 recipe/engine。

范围：

- RAW Develop 使用 scene-linear 工作图缓存；交互预览不得为正确性退回 embedded-JPEG；
- Inspector 能完成 highlights / denoise / lens（手动系数）/ colorequal / graduatednd /
  toneequal / 标签 / history-snapshot 的调整、保存、关闭 catalog、重开；
- 参数与像素结果在 reopen 后一致；取消和晚到 preview 丢弃；
- 不修键盘/焦点/HiDPI/长列表虚拟化等发行项；
- 不钉 lensfun source-root（仍用显式系数和版本化 lookup 表；无匹配继续 fail-fast）；
- 不把 L2–L9 的 CPU 数学改回简化替代路径。当前默认路径应对齐冻结 C：
  highlights `opposed`/`clip`/`inpaint`/`lch`、denoiseprofile wavelets+Y0U0V0、
  lensfun poly3 + 手动 vignette spline、colorequal dt UCS 8-node RBF、
  graduatednd `_compute_density`、toneequal 9-band RBF LUT。
- 不新开 IOP。

owner / 生命周期 / 失败：

- 工作图缓存属于 services + engine，不进 QML；
- 取消令牌绑定 preview/develop 任务；catalog 关闭后销毁缓存与 in-flight 请求；
- 缓存未命中或预算不足时结构化失败或降级到完整 decode，不得静默改用 embedded JPEG 当可编辑数据。

验证与完成门槛：

- [ ] 无 UI：RAW 交互预览与完整 render 使用同一 linear 工作缓冲合同；embedded JPEG 不能冒充可编辑结果；
- [ ] 无 UI：schema v4 标签/metadata/history 保存、重开、恢复；事务失败不丢当前 recipe；
- [ ] Studio 手工：至少一张 RAW（`mire1.cr2`）和一张 raster，覆盖上述控件的保存/关闭/重开；
- [ ] 取消与快速切换选择时，晚到 preview 被丢弃；
- [ ] 完整 Ravo unit/contract/catalog 测试和 freeze/inventory/boundary 检查通过；
- [ ] 未跑的平台和未做的手工步骤写成「未验证」。

最小命令：

```text
cmake --build --preset mac_clang_debug --target \
  ravo_unit_tests ravo_contract_tests ravo_catalog_tests ravo_studio

./build/mac_clang_debug/Ravo/tests/ravo_unit_tests
./build/mac_clang_debug/Ravo/tests/ravo_contract_tests
./build/mac_clang_debug/Ravo/tests/ravo_catalog_tests

python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
python3 Ravo/tools/check_ravo_dependency_boundary.py
```

### L1. 色彩校准：`channelmixerrgb`

依赖：C1 完成。

目标：

- 静态阅读冻结 `legacy/src/iop/channelmixerrgb.c` 的默认 CPU 路径，在 Ravo 复刻同一套通道混合数学；
- 补足简单 temperature/tint 之外的最小校准能力；
- CLI、Studio 与导出复用同一 recipe/engine operation。

范围：

- 复刻旧 C 默认 CPU 路径，而不是另写一套简化 3×3 再下线旧代码；
- 不并入 GTK chart、色卡识别 GUI、legacy preset/XMP ABI 或 OpenCL；
- 若现有 color input/white-balance ownership 不足，先停下来写清边界，不复制一套隐式 profile 状态。

验证与完成门槛：

- [ ] versioned schema 往返，未知字段、非有限数和越界矩阵 fail-fast；
- [ ] identity、单通道、交叉通道和不可逆/异常矩阵合成测试；
- [ ] 至少一张真实 RAW reference，明确工作空间和像素容差；
- [ ] Studio Color Inspector 调整、保存、关闭 catalog、重开后参数与结果一致；
- [ ] 完整 Ravo unit/contract/catalog 测试和 freeze/inventory/boundary 检查通过；
- [ ] 删除 `legacy/src/iop/channelmixerrgb.c` 及注册；
- [ ] `legacy/src/chart/common.c` 只有在全仓确认无其他消费者后才删除。

最小命令与 C1 相同。

## 3. 横向可靠性与发行阻塞

以下均未完成；它们不构成第二条功能队列，但可阻断受影响项或发行。
C1 已经认领的缓存与保存/重开门槛不要在这里重复开工。

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

后续算法（`hotpixels`、二选一的 CA、`colorbalancergb`）和 lensfun source-root
仍留在 [`DevDocs/ProductRoadmap.md`](DevDocs/ProductRoadmap.md)，C1 与 L1
完成前不写入本节队列。

## 4. 本 TODO 完成门槛

删除本文前必须同时成立：

- [ ] C1 与 L1 已逐项验收并从本文移除，已接受的旧 owner 同变更退役；
- [ ] 共享旧 owner 只剩明确消费者，剩余树均能映射到 `Ravo/MIGRATION.md` leftover；
- [ ] 横向可靠性和承诺平台安装闭环达到发布门槛，或拆成有 owner 的独立根 TODO；
- [ ] 原片安全、schema migration、备份/回滚和结构化 unsupported 有测试证据；
- [ ] 全仓搜索、target/link 图和运行验收证明不存在 Ravo ↔ legacy 生产依赖；
- [ ] 长期结论已同步到 owning docs/ADR/代码/manifest/checker/test；
- [ ] 本文不再包含未完成项，然后直接删除，不移入 `DevDocs/` 归档。
