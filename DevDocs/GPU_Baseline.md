# Ravo GPU：基线与准入规范

本文定义 Ravo CPU engine 增加 Metal 等 GPU adapter 前必须固定的测量方法。它不是
shader 实现说明；在工作负载、CPU 金样和首批连续热点链完成评审前，不开始 GPU
operation 实现。

> 当前状态（2026-07-21）：现有 runner、浮点像素比较和 GPU 路由统计属于冻结 0.9 的
> 只读测量资产。0.9 不再进行 OpenCL→Metal、pixelpipe 重构或第二轮清理。待 Ravo
> Ravo 基础软件和后续 CPU 编辑路径达到根 TODO 的各自验收门槛后，GPU 阶段复用测量方法并将
> runner 改为比较 Ravo CPU 与 Ravo GPU。当前状态不授权修改旧实现或开始 GPU 代码。

## 冻结 0.9 的观测能力

`-d perf` 已提供逐 IOP 的主机侧耗时和最终执行后端，`-d opencl` 已提供
OpenCL command queue 事件耗时。本仓库另外输出一条
`[dev_pixelpipe_summary]`，汇总：

- 本次 pixelpipe 的执行尝试数；
- 实际执行（包含失败后重跑）在 CPU/GPU 上的模块数和处理像素量；
- 连续 GPU 段数、CPU/GPU 后端切换数；
- GPU 段的理论端点数（用于观察分段，不等同于实际复制次数）；
- CPU/GPU 模块的主机侧累计墙钟时间。

GPU 模块的主机侧时间可能只包含异步提交，不是 kernel 的真实执行时间。kernel
与队列时间以 OpenCL event profiling 为准；端到端取舍以完整 pixelpipe 和进程耗时
为准。这些字段为 Ravo 的报告 schema 提供研究资料，不要求 Ravo 复刻旧日志、模块
ABI 或 OpenCL 调度。

## 固定工作负载

正式基线至少包含下表四个版本固定、可再分发的输入及其 XMP。原始文件不必进入
Git，但必须在基线清单中记录 SHA-256、相机、尺寸、来源/许可和 XMP SHA-256。

| ID | 内容 | 必须覆盖的处理 |
| --- | --- | --- |
| `raw-detail` | 高像素、细密纹理、低 ISO Bayer RAW | 去马赛克、镜头校正、输入/输出色彩、局部对比或锐化 |
| `raw-noise` | 高 ISO、暗部占比较高的 Bayer/X-Trans RAW | 去马赛克、色度/亮度去噪、曝光、tone mapping |
| `raw-geometry` | 建筑或网格状 RAW | 旋转/裁剪、透视或镜头几何、缩放/插值 |
| `raw-mask` | 明暗与高饱和颜色并存的 RAW | 绘制/参数蒙版、混合、颜色选区及至少一个高成本 IOP |

每个工作负载都测量三种产品路径：

1. 快速预览：长边 1600 px，允许产品定义的 preview 质量策略；
2. 100% 暗房：固定 ROI，禁止窗口尺寸和缩放状态漂移；
3. 全尺寸导出：高质量处理，32-bit float TIFF，无缩小。

冻结的 legacy CLI runner 只覆盖第 3 种路径。Ravo GPU 必须把预览与导出接到同一
版本化报告 schema；不能用全尺寸导出数据代替交互首帧和更新延迟。

## 可重复执行

以下命令仅用于复现冻结 0.9 的历史 CPU/OpenCL 报告；不得为使它通过而修改 `src`。
不要直接从长期使用的增量构建目录加载插件。残留 `.so` 可能被动态发现并污染结果，
应从冻结提交构建并安装到新的 staging prefix：

```sh
cmake --build --preset mac_clang_release -j 8
cmake --install build/mac_clang_release --prefix /tmp/dtn-gpu-stage
python3 benchmarks/gpu_baseline.py \
  --input /corpus/raw-detail.dng \
  --xmp /corpus/raw-detail.dng.xmp \
  --output-dir /tmp/dtn-gpu-raw-detail \
  --cli /tmp/dtn-gpu-stage/bin/darktable-cli \
  --data-dir /tmp/dtn-gpu-stage/share/darktable \
  --module-dir /tmp/dtn-gpu-stage/lib/darktable \
  --warmups 2 --runs 7 --require-opencl
```

legacy runner 为 CPU/OpenCL 分别创建隔离的 config/cache，使用内存数据库、关闭自定义
预设，并强制 32-bit float TIFF。输出目录必须为空。它生成：

- `report.json`：Git/二进制/输入哈希、完整命令、每次耗时、逐模块路由、GPU 段、
  OpenCL queue 时间、浮点像素误差和输出哈希；
- `summary.md`：中位数和最慢模块；
- 每次运行的原始 `.log` 和正式测量 `.tif`。

runner 通过 libtiff 读取 CPU/OpenCL 第一组正式输出的解码后浮点 scanline，忽略
会变化的 TIFF 时间和路径元数据，再计算全图及分通道误差。RMSE 和最大误差扫描
全部样本；P99 对超过一百万样本的图像使用固定步长确定性采样，并在 JSON 中记录
样本数和步长。默认门槛对应下述复杂算子准入线，可用 `--rmse-limit` /
`--max-abs-limit` 为更严格的工作流收紧。

对比必须固定机器、macOS 版本、电源供电/低电量模式、温度状态、release 构建、
线程数、输入/XMP、输出尺寸和后台负载。正式报告使用 2 次预热、至少 7 次测量，
同时报告中位数、P90 和离散程度；单次结果只能作为冒烟检查。

## CPU 金样与正确性门槛

金样由指定 release commit 在 CPU-only 模式生成，连同输入/XMP/二进制哈希、命令
和色彩配置一起版本化记录。不要只保存最终 8-bit JPEG；至少保存 32-bit float
TIFF，并逐步增加关键 IOP 边界的线性浮点 dump。

所有 Ravo GPU 候选先满足以下硬门槛：

- 宽高、通道、ROI 和几何映射完全一致；所有输出均为有限值，不出现新 NaN/Inf；
- alpha、蒙版内外语义和未处理通道一致，离散标签/索引必须 bit-exact；
- 普通浮点算子：归一化通道 `RMSE <= 2e-6`、`max_abs <= 2e-5`；
- 含迭代、原子归约、去噪或去马赛克的算子：`RMSE <= 2e-5`、
  `max_abs <= 2e-4`；
- 最终显示参考图：CIEDE2000 的 P99 `<= 0.25`、最大值 `<= 1.0`，并人工检查
  边缘、暗部、饱和高光、蒙版边界和几何边界。

这些是首轮准入线，不是对所有算子放宽精度的许可。若 CPU 实现本身存在平台浮点
漂移，应先通过 CPU 重复运行和跨机器数据量化噪声，再以“operation + 参数范围 + 原因”
登记更窄的例外；不得只因 Ravo GPU 未通过而扩大全局容差。

## 性能与首批迁移链选择

候选链按实际执行顺序分组，而不是按 `.cl` 文件排序。选择同时满足：

- 在至少一个代表工作负载中属于端到端主要耗时；
- 相邻操作可连续留在 GPU，边界和格式转换成本可摊薄；
- 公共依赖（色彩/格式、插值、混合/蒙版、Gaussian/NLM 等）可被多个 IOP 复用；
- CPU 回退和错误后从可信输入重跑的边界清楚。

Ravo GPU 准入目标在采完基线后写入具体数值。最低要求是：核心工作流的端到端中位数
有可重复收益，P90 不退化，峰值内存和能耗不出现未解释回归。单模块 kernel 更快，
但完整 Ravo render 因边界、等待或缓存失效变慢，判定为不通过。

## Ravo GPU 准入出口清单

- [ ] 四个工作负载及三条产品路径的版本化清单；
- [ ] 正式 CPU 金样及版本化清单；
- [x] 32-bit float TIFF 逐像素比较器和可配置容差门槛；
- [x] CPU/OpenCL CLI runner 与结构化报告；
- [x] 逐模块后端、连续 GPU 段和路由端点统计；
- [ ] 预览首帧/交互更新、峰值内存和能耗采样；
- [ ] 基于报告选出的首批连续模块链和端到端收益门槛。

只有全部完成，才在 Ravo engine 内进入后端中立 port 与 GPU adapter 实现；冻结的
0.9 pixelpipe 和 OpenCL 路径始终不修改。
