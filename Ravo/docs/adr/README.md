# Ravo Architecture Decision Records

ADR 记录已接受且会约束后续实现的重要决定。新 ADR 使用递增四位编号，包含状态、日期、背景、决定、
后果和被否决方案。已接受 ADR 不静默改写；方向变化时新增 ADR 并标记被替代关系。

| ADR | 状态 | 决定 |
| --- | --- | --- |
| [0001](0001-cpp20-headless-first.md) | Partially superseded by 0007 | C++20、无头 engine/CLI 优先、首版不使用 Rust |
| [0002](0002-ravo-consumes-src.md) | Superseded by 0004 | Ravo 单向替代并最终删除旧 `src` 所有权 |
| [0003](0003-versioned-machine-contract.md) | Accepted | CLI JSON、recipe 和 operation schema 的版本化机器契约 |
| [0004](0004-freeze-09-ravo-only-growth.md) | Partially superseded by 0010 | 冻结 0.9；Ravo 是唯一增长路径。删除时点见 0010 |
| [0005](0005-qtcore-filesystem-adapter.md) | Partially superseded by 0007 | Qt6::Core 可由 Ravo target 按需直接使用；UI 排期由 0007 更新 |
| [0006](0006-explicit-colour-contract.md) | Accepted | 颜色状态在 engine 边界显式、版本化；第三方色彩类型保持私有 |
| [0007](0007-first-usable-catalog-viewer.md) | Accepted | C++ + Qt Quick/QML 第一版优先交付 SQLite catalog、图片导入与桌面 viewer 纵切片 |
| [0008](0008-p0-review-catalog-v2.md) | Accepted | Catalog schema v2 持久化 P0 rating/color/reject，并提升 preview contract |
| [0009](0009-p1-develop-recipe.md) | Partially superseded by 0016 | Catalog schema v4 每张图一份 canonical recipe，外加 tags/metadata/history；旧 lift/gamma/gain 由 0016 取代 |
| [0010](0010-incremental-legacy-retirement.md) | Accepted | Ravo 已验收的旧 owner 按 active migration TODO 增量删除；剩余 leftover 仍对照 freeze blob |
| [0011](0011-atomic-develop-publication.md) | Accepted | recipe/history/revision 原子发布；Develop preview 按 revision 拥有取消与晚到拒绝 |
| [0012](0012-explicit-channelmixerrgb.md) | Partially superseded by 0017 | `channelmixerrgb` V3 CPU 数学使用显式 D50 工作空间、adaptation 与 canonical schema；WB owner 由 0017 收口 |
| [0013](0013-bayer-hotpixels-preprocess.md) | Accepted | `hotpixels` 在 owned Bayer CFA 副本上按冻结四邻居合同执行并进入 RAW cache key |
| [0014](0014-bayer-cacorrect.md) | Accepted | `cacorrect` 保留 Bayer 两遍 tile 统计、多项式 shift fit 与 avoid-color-shift 路径 |
| [0015](0015-migrate-all-non-ui-algorithms.md) | Accepted | 全部剩余非 UI 算法逐项迁为 C++；GTK/Lua/动态 ABI/OpenCL 最终删除而不移植 |
| [0016](0016-filmlight-colorbalancergb.md) | Accepted | `colorbalancergb` 保留 Filmlight Yrg 三段调色、DT UCS 默认与显式 JzAzBz gamut 合同 |
| [0017](0017-explicit-raw-temperature.md) | Accepted | `temperature` 统一拥有 pre-demosaic as-shot/daylight/manual 四通道 scaling；late reference 只走显式 CAT |
