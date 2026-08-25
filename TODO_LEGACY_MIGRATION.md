# Ravo Legacy Migration TODO

> **状态：in progress**
>
> **更新日期：2026-08-25**
>
> **当前执行焦点：L1 `channelmixerrgb` 色彩校准。**
>
> 本文只记录尚未完成的执行工作、风险、依赖、验证命令和完成门槛。当前能力、
> 架构、迁移政策、leftover 边界和测试合同分别以
> [`Ravo/README.md`](Ravo/README.md)、
> [`Ravo/ARCHITECTURE.md`](Ravo/ARCHITECTURE.md)、
> [`Ravo/MIGRATION.md`](Ravo/MIGRATION.md) 和
> [`Ravo/TESTING.md`](Ravo/TESTING.md) 为真相源。尚未具备执行门槛的能力只记录在
> [`DevDocs/ProductRoadmap.md`](DevDocs/ProductRoadmap.md)。

## 1. 执行规则

- 一次只推进队列中的第一项；未完成前不得并行开下一项。
- 每项先静态读取旧 owner/fixture，再定义 Ravo owner、生命周期、失败/取消路径和最小验证集。
- 「Ravo 已验收」和旧 owner 删除门槛统一按 `Ravo/MIGRATION.md`；门槛未满足只能改 Ravo。
- 完成一项时，把长期结论同步到 README/ARCHITECTURE/MIGRATION/TESTING/ADR/代码或测试，
  然后从本文删除该项；不要留下 `[x]` 历史或归档 TODO。
- 可靠性 finding 可以阻断当前项，但不得借机批量清理 GTK/OpenCL/共享 imageio/fixture。
- 未运行的平台和手工检查必须写成「未验证」，不得复用历史结果冒充本轮通过。

## 2. 迁移队列

### L1. 色彩校准：`channelmixerrgb`（当前）

目标：

- 在 RAW/scene-linear 路径提供稳定的 RGB 通道混合；
- 补足简单 temperature/tint 之外的最小校准能力；
- CLI、Studio 与导出复用同一 recipe/engine operation。

范围：

- 首版只做显式工作空间中的 3×3 RGB mix；
- 不并入完整 CAT16、色卡识别、GTK chart、legacy preset/XMP ABI；
- 若现有 color input/white-balance ownership 不足，先停下来写清边界，不复制一套隐式 profile 状态。

验证与完成门槛：

- [ ] versioned schema 往返，未知字段、非有限数和越界矩阵 fail-fast；
- [ ] identity、单通道、交叉通道和不可逆/异常矩阵合成测试；
- [ ] 至少一张真实 RAW reference，明确工作空间和像素容差；
- [ ] Studio Color Inspector 调整、保存、关闭 catalog、重开后参数与结果一致；
- [ ] 完整 Ravo unit/contract/catalog 测试和 freeze/inventory/boundary 检查通过；
- [ ] 删除 `legacy/src/iop/channelmixerrgb.c` 及注册；
- [ ] `legacy/src/chart/common.c` 只有在全仓确认无其他消费者后才删除。

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

### L2. RAW 高光重建：`highlights`

- [ ] 对支持的 CFA/模式重建传感器裁切高光；不支持时返回结构化 unsupported；
- [ ] owner 位于 RAW prepare/demosaic 邻接的 engine 阶段，不塞进 Sigmoid；
- [ ] 覆盖专用高光 fixture 或 `mire1.cr2`、边界/NaN、取消和内存预算；
- [ ] 删除 `legacy/src/iop/highlights.c` 及注册。

### L3. 默认降噪：`denoiseprofile`

- [ ] 只迁旧 `DEFAULT_VISIBLE` 的 `denoiseprofile`；
- [ ] `nlmeans`、`atrous`、`bilateral`、`rawdenoise` 不并入本项；
- [ ] 覆盖固定种子合成噪声、真实 RAW、质量统计、时间/峰值内存和取消；
- [ ] 删除 `legacy/src/iop/denoiseprofile.c` 及注册。

### L4. 镜头校正：`lens`

- [ ] 有标定时校正畸变/TCA/暗角；无数据库或匹配时结构化失败；
- [ ] lensfun（或后继）只通过显式 source-root adapter 接入，禁止 FetchContent；
- [ ] 覆盖有/无标定、几何边界、真实 fixture 和可用平台构建；
- [ ] 依赖提交先发布并确认 remote 可达，再更新父仓锁模板；
- [ ] 删除 `legacy/src/iop/lens.cc` 及注册。

### L5. 标签与可写 metadata

- [ ] Unicode 关键字/标签持久化并可筛选；
- [ ] 基础 EXIF/IPTC 可查看，允许字段只写 catalog；
- [ ] 覆盖 schema migration、事务回滚、重开、Unicode、Studio/CLI；
- [ ] 不做 faces、map、GPS/原片写回和未验收的全量 sidecar；
- [ ] 删除 `legacy/src/libs/tagging.c`、`metadata.c`、`metadata_view.c`，
  前提是无其他旧 owner 消费。

### L6. 持久 history / 快照

- [ ] 跨重启恢复先前 canonical recipe，不只依赖 presenter 会话栈；
- [ ] 未知旧 XMP history 继续 unsupported，不猜测重放；
- [ ] 覆盖保存/重开/恢复、事务失败、删除/版本关系、Studio 列表与 CLI；
- [ ] 删除 `legacy/src/libs/history.c`、`snapshots.c`、`copy_history.c`。

### L7. 颜色均衡（二选一）

- [ ] 在 `colorequal` 与 `colorzones` 中做 dated 产品决定，只选一个 HSL 分区能力；
- [ ] 覆盖 recipe、合成色块、工作空间边界和 Studio 分区控件；
- [ ] 删除被选 IOP；另一个写入 `Ravo/MIGRATION.md` leftover 或另开 TODO。

### L8. 渐变滤镜：`graduatednd`

- [ ] 实现线性渐变曝光/密度，作为第一个局部调整；
- [ ] 若需要通用 mask，先定义 mask owner/坐标/组合/取消契约，不暗建第二套 mask 图；
- [ ] 覆盖合成渐变、几何/方向、取消和 preview 晚到结果；
- [ ] 删除 `legacy/src/iop/graduatednd.c` 及注册。

### L9. 影调均化：`toneequal`

- [ ] 在已固定 Sigmoid 显示变换下实现，不再引入冲突的全局 tone mapper；
- [ ] 覆盖合成动态范围、至少一张 RAW、性能和内存；
- [ ] 删除 `legacy/src/iop/toneequal.c` 及注册。

## 3. 横向可靠性与发行阻塞

以下均未完成；它们不构成第二条功能队列，但可阻断受影响项或发行：

- [ ] Gallery thumbnail 虚拟化、长列表内存、worker/preview/cache 明确预算；
- [ ] RAW interactive linear working-image cache，避免为正确性退回 embedded-JPEG 编辑；
- [ ] 数据库不可写/损坏、cache 损坏、磁盘满、源移动、批量取消、崩溃重开；
- [ ] schema migration fixture、导入到首帧指标、窗口/catalog 关闭后的资源销毁；
- [ ] Studio 导出、tone curve、Sigmoid 和后续 operation 的真实保存/重开验收；
- [ ] 键盘、焦点、HiDPI、可访问性；
- [ ] Windows/macOS/Linux configure/build/test、staged install 和安装目录闭环；
- [ ] metadata/ICC 与输出颜色合同达到承诺格式门槛；
- [ ] 指定 packaged-release product owner 与 code-review owner；
- [ ] 发行切换前证明备份/回滚、原片安全和无 legacy 生产依赖。

GPU 不进入当前队列。只有相关 CPU 路径验收、金样稳定且端到端测量证明收益后，
才能按 [`DevDocs/GPU_Baseline.md`](DevDocs/GPU_Baseline.md) 新建专项 TODO。

## 4. 本 TODO 完成门槛

删除本文前必须同时成立：

- [ ] L1–L9 已逐项验收并从本文移除，已接受的旧 owner 同变更退役；
- [ ] 共享旧 owner 只剩明确消费者，剩余树均能映射到 `Ravo/MIGRATION.md` leftover；
- [ ] 横向可靠性和承诺平台安装闭环达到发布门槛，或拆成有 owner 的独立根 TODO；
- [ ] 原片安全、schema migration、备份/回滚和结构化 unsupported 有测试证据；
- [ ] 全仓搜索、target/link 图和运行验收证明不存在 Ravo ↔ legacy 生产依赖；
- [ ] 长期结论已同步到 owning docs/ADR/代码/manifest/checker/test；
- [ ] 本文不再包含未完成项，然后直接删除，不移入 `DevDocs/` 归档。
