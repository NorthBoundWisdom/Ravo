# LightCraft 可吸收能力执行 TODO

用户要求：优先研究并改善调参渲染延迟，随后吸收 LightCraft 中有价值的能力。
本队列只记录未完成工作、依赖、风险和验收门槛；不将参考实现、宣传数字或
静态代码审阅视为 Ravo 已实现的能力。

主执行顺序由 [TODO.md](TODO.md) 维护；性能预算归 PERF-01/02，像素正确性归
IQ-00，RAW/profile 和 Lightroom 任务仍归各自 TODO，架构重构验证归主 TODO 的
跨层验证门槛。本文件是跨专题
吸收工作清单，不另建预算、像素基线或第二套产品状态。

## 顺序与边界

1. 当前推进 LC-03 及其他功能吸收；性能复测留待 CPU 空闲窗口。LC-00 的场景矩阵
   和 LC-02 剩余显示交接/发布开销仍未完成；精确 RGB8 缓存
   的稳定合同归 [ADR-0171](adr/0171-exact-rgb8-display-transform-cache.md)。后续由
   同口径测量选择 LC-01 或 GPU 融合，不强行先改已足够快的 GPU 内核。
2. LC-03 至 LC-09 按合同和依赖顺序推进；明确的数据损坏/误改问题
   始终优先修复，不等待性能工作。
3. LC-10 的 Web/新产品入口须先完成范围决策，不能作为前述任务的隐含依赖。
4. 全部任务保持 C++20、现有 Engine/services/desktop、Qt Quick 和 FreeCM。
   参考设计与可验证行为；不引入 Rust/egui/wgpu 产品实现、第二个 renderer、
   MCP 专属业务状态，或历史 OpenCL/IOP 接口。
5. 每项实现前明确 owner、线程、资源寿命、取消与失败路径、最小验证集。
   新依赖、模型、公共合同、schema、算法或产品范围按现有 ADR 流程先决策。
6. 不吸收参考项目的无条件重发写命令、无锁可写、宽松目标解析、静默 GPU
   fallback 或无界消息队列；保留 Ravo 的冲突、回执与原图安全合同。

## LC-00 / P0 — 调参渲染性能：先建立可比基线

- [ ] 按 [TESTING.md 的比较口径](TESTING.md#interactive-performance-comparison)
  补齐当前 SHA 的同口径 Release 场景矩阵；历史 Debug、缓存整帧命中和单次最小值
  不能代替正式基线。
- [ ] 在暖缓存、约 2.46 MP 输出、GPU 参数变动重渲染之外，补冷解码、冷 GPU、
  首次预览、空间参数变化和完整导出的独立分布；不把历史 `~4 ms` 泛化为所有操作。
- [ ] 在同一台机器、同一源图及相同输出尺寸/颜色合同下运行；分别记录原片尺寸、
  prepared 尺寸、输出尺寸、像素数、有效配方、CPU/GPU、质量、mask/ICC 与缓存状态。
  无法构建参考项目或缺少同图时明确标记未验证，不给跨项目速度倍数。
- [ ] 在现有常用数值/空间字段矩阵之外，补局部蒙版、1:1 ROI、典型复杂配方、
  多相机/高 ISO 与多 profile 的场景；不能只测 identity 或删掉默认锐化。
- [ ] 在现有参数/重复突发测量中补峰值 owned bytes、缓存/上传命中及任务取消/
  合并计数；未观察到发布的 intent 不直接等同物理掉帧、取消任务或 GPU dispatch。
- [ ] 补齐首次拖动、GPU/shader 冷启动的独立场景；按既有真实参数/完整 burst
  暖机口径采集，不能用 cached settled 图片或单次最小值代替。
- [ ] 补 prepared 尺寸、阶段缓存/上传命中、线程和峰值资源 metadata；现有微秒
  service probe 与参数矩阵按 [TESTING.md](TESTING.md#interactive-performance-comparison)
  重跑。未填字段、旧整数毫秒日志不能用于口径匹配。
- [ ] 分解计时：输入分派/排队、catalog/source 验证、Recipe 构造/校验、解码/准备、
  prefix 与空间阶段、GPU upload/copy/dispatch/submit/wait/readback、RGB8 owned handoff、
  ICC、GUI publication、hash/scopes、真实 native frame swap。
- [ ] 继续调查去雾正向区间的场景无响应与剩余高耗时，分解源准备、环境光和
  传输率下限；补有雾/HDR/正负强度 corpus。按现有 CPU oracle 验证优化，语义
  改动单独决策；不得用无响应样本通过速度门槛。
- [ ] 将显示 owner 绑定/解除绑定的诊断对照纳入可复跑测量，交错顺序并记录真实
  monitor profile；解除绑定只用于归因，不作为正确显示结果或性能达标路径。
- [ ] 记录实际执行过的阶段、dispatch、字节复制、readback、分配和线程创建次数。
  拖动路径已有内存预览和合并，不预设 DB fsync、PNG 编码或 RAW 重解码为瓶颈。
- [ ] 基于 warm 参数重渲染 P50 ≤4 ms、P90 ≤8 ms 和当前默认尺寸
  intent→owned-image P90 ≤16 ms 的候选目标，由 PERF-01 扩展同口径测量后冻结
  适用配方/硬件与正式预算；单图/单机达标不是全功能保证。

**验收：** 可重跑的阶段归因报告指出主导 owner；CPU/GPU、渲染完成、owned-image
发布与屏幕呈现分开报告；至少一组非 identity 的实际参数变动且像素发生预期变化。
不以降低默认分辨率、隐藏效果、旧帧命中、放宽像素误差或只报最小值达标。

## LC-01 / P0 — 依赖驱动的阶段复用与 GPU 常驻

- [ ] 对照当前 pre-light prefix 与 LightCraft 的 `geo / lin_key / per-plane radius`
  分层，列出 Ravo 每个阶段真正读取的参数、源 generation、几何、颜色与 mask 输入。
- [ ] 依据 LC-00 结果扩展现有 `InteractivePreviewRenderCache`，评估可安全复用的
  几何结果、profile/preprocess、降噪与空间中间量；只失效真实依赖的阶段及其后继。
- [ ] 优先分解白平衡/去雾的 RAW 准备重算和几何密度变化导致的 prepared miss；
  缓存边界必须保留白平衡逐样本舍入、demosaic、去雾与 Input Color 的原顺序。
  不把大尺寸 working 二次缩放的结果假定为直接准备的逐位等价结果。
- [ ] 在末尾未蒙版降噪 basis 复用之外，分解冷 wavelet/MAD、半径/噪声输入变化
  和复杂多阶段配方成本；评估 mask/非末尾阶段复用前先冻结 owned-byte 上限与
  所有真实输入的失效合同，保留 uncached CPU 等价和取消。
- [ ] 保留 Ravo 现有算法顺序。不得为仿照参考缓存，把依赖曝光结果的 Lab 锐化等
  空间运算移动到曝光之前，或在输入变化后复用其旧中间量。
- [ ] 评估 per-view、分辨率/ROI generation 有界缓存与 GPU 常驻；分析全图/ROI、
  Before/After 与不同 output size 争用单一 retained-source 的成本。
- [ ] 对预览/CFA window/前景 prefix 使用同一明确的内存核算和驱逐规则；区分资源
  容量复用与像素内容命中，不能只凭宽高复用。需要新预算时先由 PERF-01 冻结。
- [ ] 核实混合/CPU 路径的 `WorkingImage` 拷贝、RGB 交接、Recipe 序列化与整图遍历
  的必要性；已准入 native GPU tail 的借用合同归 [ADR-0172](adr/0172-borrowed-retained-native-preview.md)。在
  lifetime 可证明的边界使用不可变共享输入/租借，保留错误与 owned publication。
- [ ] 覆盖源文件替换、同尺寸 pan、operation order/enabled/mask/profile 改变、Undo、
  catalog/asset 切换、缓存驱逐、取消、设备丢失和窗口销毁后的失效。

**owner / 生命周期：** 像素和阶段计划归 Engine，prepared/source 生命周期归服务
已有 owner，设备资源归 QRhi adapter。desktop 只持有不可变结果；关闭/替换先取消，
再等待 owner 完成后释放。缓存失败不发布半成品，不增加持久化 edit authority。

**验收：** cached/uncached CPU 结果保持现有严格合同；新 GPU 复用路径通过 IQ-00
既有误差门槛。暖曝光不重跑不相关解码/降噪/几何，命中/失效计数可验证；同负载
P90 和资源成本改善，不牺牲冷路径、ROI 或复杂配方正确性。

## LC-02 / P0 — 末端融合、提交与显示路径

- [ ] 对照 LightCraft `finish`，评估将相邻、已准入的逐像素 Ravo 操作融合为更少的
  GPU dispatch/整图读写；保持 operation 顺序、实例、mask、颜色和舍入合同。
- [ ] 把空间运算与逐像素融合区间明确分开；优先由 LC-00 的 pass/等待归因选定候选，
  不一次迁移整条 GPU pipeline，也不绕过 CPU oracle。
- [ ] 检查每次预览的资源绑定、uniform、buffer 分配、seed copy、QRhi frame end、
  Metal blit 与同步等待；在既有 device owner 内评估复用和提交合并。
- [ ] 评估首次可交互前的 device/shader 与真实选图 prefix 预热；只在既有 Engine
  设备线程/服务 executor 中安排有界可取消工作，不能借用参考项目的任意线程初始化。
  先等准确 Recipe 载入，切图/关闭拒绝迟到结果；CPU-only 或未准入设备不强制加载 GPU。
- [ ] 分别核算 IOSurface→RGB8 snapshot、后续复制、ICC 与 surface 再发布。
  不能只因存在 IOSurface 就把整条链路称作零拷贝。
- [ ] 继续分解 `show_preview_result → apply_display_presentation_image` 的 RGB 打包、
  复制与 surface publication；在既有 worker 中评估完整转换后接受不可变结果。
  GUI 不新增像素算法或任务 owner。
- [ ] 扩展精确 RGB8 缓存的冷启动、不同 output/proof ICC、频繁换屏和多窗口资源
  压力测量；64 MiB 常驻及替换期间旧读者占用纳入 PERF-01 峰值预算。
  保留 `NOCACHE/NOOPTIMIZE` byte-exact 参考；GPU ICC、近似 LUT 或修改标志须
  单独准入，不得作为无语义变化的微优化。
- [ ] 保留跨线程不可变像素、live identity、scopes 与 monitor ICC；评估安全的有界
  surface lease/槽位生命周期，不能恢复借用双缓冲 token 跨异步队列的旧风险。
- [ ] 持续交互仍采用 bounded latest intent；同时测最新参数追上时间，不能只优化
  中间帧完成时间。后台 import、export、模型推理不可饿死前景预览。
- [ ] 扩展慢 RAW quiet supersession 的持续输入、不同输入间隔、多相机和复杂配方
  压力测量；同时约束首帧延迟和最终追赶，不能只以其中一项通过预算。
- [ ] 只有用户可见且有合同的质量模式才评估 draft/full 两档与缓存；默认 1600px
  显示路径保持现有行为，不能通过静默改成低分辨率制造 4 ms。

**验收：** 每次只准入一个有测量支撑的改动；现有 CPU gold、GPU packed-delta、
原图/配方/保存安全、取消、晚结果、device loss、close/join 与 source-size 合同不退步。
GPU 失败保持结构化失败；native frame timing 须用受支持机器接口采集，不能用
应用截图或 Computer Use 验收。

## LC-03 / P1 — 从导入到交付的 AI/MCP 工作流

- [ ] 在共享 C++ command/service 合同和 CLI 中补齐机器可见的导入计划/执行、
  关键词树管理、集合和导出计划/执行，再投射到 MCP；保持与已有
  有界图库查询、显式选图和试调/提交合同一致的修订与回执语义。
- [ ] 对 ADR-0184 的显式目标批量注释和有界状态读回补齐真实照片集与安装包验证；
  保持逐项提交、失败即停和未知结果回执，不扩大为当前多选。
- [ ] 复用现有菜单/命令与 Recipe 描述，补 machine-readable 参数、单位、范围、
  availability、输入/输出 schema；减少手写工具、CLI 和 desktop 的语义漂移。
- [ ] 自动生成工具须使用受支持且明确的 capability 集；嵌套参数保持严格验证，
  不能把 free-form UI/菜单调用当作稳定公共 API。
- [ ] 显式 ID、catalog/session/selection/recipe 修订在同一 owner 检查后执行；
  非法/缺失目标不得退到当前照片，写入丢失响应不得自动重放或换新 ID。
- [ ] 扩展 operation 回执覆盖批量/导出已完成项、取消、unknown outcome 与资源释放；
  history/receipt/session 失效时提供明确的 reconcile 路径。
- [ ] 取消测试用屏障或受控慢任务保证请求仍在运行；同时覆盖完成与取消竞态。
  不能假设节流的第一个非零 progress 一定早于完成，或依赖机器速度通过测试。
- [ ] 评估 offline catalog MCP 与 live Studio 两种明确绑定方式；二者仍复用 CLI 和
  现有服务，不引入 MCP 内独立 Session/catalog/renderer。

**验收：** 真实 subprocess 完成导入→查询→选择→只读试调→提交→导出→读回；覆盖
两客户端竞争、切换照片、断线、重复请求、部分完成、output conflict、EOF 和关闭。
既有 [ADR-0168](adr/0168-ai-control-and-mcp.md) 的严格边界保持成立。

## LC-04 / P1 — 模型管理与真实 AI 推理

- [ ] 为语义蒙版、Bayer/X-Trans 降噪及人脸检测/识别分别定义可发布模型、权重来源、
  许可/分发条件、hash/size、运行硬件、版本和可复现质量数据；缺失模型明确 unavailable。
- [ ] 实现 opt-in 模型安装/校验/取消/替换与离线使用；下载失败保留已验证旧模型，
  不在默认安装或编辑时隐式下载，不把启发式/stub 标成已运行 AI。
- [ ] 比较独立 worker、tile/overlap、源/模型/算法版本缓存、缓存校验与配额驱逐，
  将并发、内存、取消与 interactive foreground 优先级写进服务 owner 合同。
- [ ] 优先 subject/sky/background/object，其后 people/face naming；保留用户确认、
  禁止自动删除/命名，明确模型和 confidence 的含义。
- [ ] 评估可验证的 Auto Tone/白平衡、灰尘候选、焦点/连拍辅助、参考风格和 shoot-level
  一致性；结果仍是普通 Recipe/metadata proposal，启发式能力不标成模型推理。
- [ ] AI 降噪的导出请求须生成或验证所需结果；不能无提示地导出未降噪原结果。
  RAW 安全、色彩、细节和不同 CFA 分开验收。

**依赖 / 验收：** 先完成模型/运行时 ADR 和固定依赖流程；实现沿用 Engine/services，
不直接接入参考 Rust runtime。测试畸形模型、shape/预算、tile seams、设备失败、
源/模型替换、关闭、重开与没有网络/模型的明确状态；真实授权 corpus 和人工质量
审阅仍是发布门槛。与 [ProductRoadmap.md](ProductRoadmap.md) R3/R4、IQ-01 对齐。

## LC-05 / P1 — 可持久化、与模型解耦的语义蒙版

- [ ] 对照 `SegMask` 的量化 logits、压缩、normalized bounds 与局部 refinement，
  决定 Ravo canonical mask 的有界 raster/semantic 结果合同和版本迁移。
- [ ] 结果绑定 asset/source/recipe revision、model/weight identity、坐标/方向/裁切、
  MIME/尺寸/hash 和来源；拒绝无界解压、损坏尺寸、错误目标和迟到的 worker 结果。
- [ ] 保存可重放的 mask 结果，渲染、重开、撤销、导出和备份恢复不依赖重新运行模型。
  refinement 与 brush/path/range/group 组合沿用同一 mask evaluator。
- [ ] 多分辨率边缘/透明度、局部 patch、ROI apron、预览/导出一致性通过 CPU fixture
  后，再考虑 GPU；模型缓存不是 edit authority。

**验收：** 用户可预览、接受、拒绝、撤销和恢复；模型卸载后已接受结果仍可渲染；
拒绝/取消不改变保存状态。schema 和资源生命周期先有 ADR，不添加空表或空 UI。

## LC-06 / P1 — 相机、默认外观与镜头证据

- [ ] 建立可重算的相机/格式/variant/CFA 证据表，区分 sensor decode、preview-only、
  unsupported 和 unverified，保留来源、hash、设置与固定输入版本。
- [ ] 补默认 RAW 外观、白平衡/曝光/颜色、边缘/高光/高 ISO、ROI/导出的一致性实拍
  数据；比较相机 JPEG/校准参考时分别标明目标，不能将拟合 JPEG 等同实测色彩标定。
- [ ] 评估自有相机 profile 校准/guarded fitting；数据采集与留出集质量门槛归
  [TODO_RAW_RENDERING_PROFILES.md](TODO_RAW_RENDERING_PROFILES.md)，不替换未准入默认。
- [ ] 为真实镜头数据库定义校准来源/许可、版本更新、camera/lens/focal/aperture 匹配、
  distortion/TCA/vignetting 与缺失匹配合同；当前 fixture lookup 不算真实覆盖。
- [ ] 对已有 LibRaw 与文件内 lens metadata 优先补证据和 adapter；不以重写 RAW
  解码器或仅 pin 一个依赖代替产品能力。

**验收：** 每个新增覆盖声明对应真实可重跑样本；矩阵变化有留出集与异常样本；
镜头匹配明确且可检查，未命中不猜参数。共享 IQ-00/IQ-01 与现有 profile 队列。

## LC-07 / P2 — HDR、合成与输出格式扩展

- [ ] 评估 HDR 编辑/预览与完整交付链、线性/浮点 DNG、HDR gain-map JPEG、PQ AVIF、
  JPEG XL 导入及 WebP 交付的实际需求，列出编码器/解码器/颜色/metadata 依赖。
- [ ] 对照 spherical/cylindrical panorama、HDR panorama、deghost/refinement、seams、
  boundary warp 与多分辨率融合，补真实运动/视差/曝光 bracket corpus 后分项准入。
- [ ] 每个范围扩展先有 ADR，复用现有 merge/export owner、原子 no-replace、provenance、
  原图保护和 per-item 结果；不另建 exporter 或 renderer。
- [ ] HDR 码值、transfer/primaries、ICC/NCLX、bit depth、gain map 容器、SDR rendition、
  重开与不支持 HDR 显示器的行为分别验收。

**依赖 / 验收：** 当前 [ADR-0163](adr/0163-hdr-panorama-derived-assets.md) 仍限定 TIFF
HDR/平面全景；浮点 DNG、球面与新格式在决定和完整测试前保持未实现状态。
颜色/原图/取消/磁盘失败/输出冲突合同必须先于覆盖声明。

## LC-08 / P2 — 图库、迁移与可观测性

- [ ] 评估无需正式导入的 Local browse 体验、metadata-only 发现、按需解码和有界
  cache；明确 temporary record 与 canonical catalog 的关系。
- [ ] 若引入清理策略，提供 dry-run、显式确认和引用/编辑 fingerprint 保护；任何
  edited、history、version、collection、stack、proxy、undo 或正在使用的记录不得丢失。
- [ ] 借鉴 field-by-field migration diagnostics、inspectable activity 与持久化错误
  可见状态；保留 Ravo 的完整源档案与精确 native history/receipt，不退为 best-effort。
- [ ] 扩展 torn-write、ENOSPC、丢失响应、锁失败、进程结束、升级与恢复 fault matrix。
  持久化仍归 SQLite/services，不能改用第二套 JSON journal 或无锁可写 fallback。
- [ ] Lightroom 残余任务归 [TODO_LIGHTROOM_IMPORT.md](TODO_LIGHTROOM_IMPORT.md)；
  大图库索引/分页/内存预算和工作饥饿归 PERF-01 与 REL-01。
- [ ] 对导入网格键盘路径补受控负载与冷字体比较，结合现有分段 wall/process-CPU
  样本冻结 50 ms 焦点移动预算的适用条件；独立复测成功不能代替
  并发压力证据，保留越界样本，不能调大门槛掩盖问题。

**验收：** 100k 规模、低内存、外部盘断连、重复导入/恢复后关联和编辑完整；没有
源文件/数据证明时明确冲突或 unknown，不以“功能存在”代替质量证据。

## LC-09 / P2 — Workspace 与开发质量工具

- [ ] 评估可停靠/分组/浮动/恢复布局、快捷键发现与可访问性，沿用 Qt Quick 和
  GeoControls；workspace preference 的版本、损坏恢复和用户 reset 保持单一 owner。
- [ ] 将正式 command/control/schema、可用状态、测试和文档引用形成可检查清单；
  分开标注 implemented、fixture-qualified、corpus-qualified 与 packaged-host-qualified。
- [ ] 补性能/内存 inspector 与受支持的机器诊断接口；不得增加 QML introspection、
  UI click/screenshot 控制面来代替命令或像素验收。
- [ ] 为可选模型/codec/硬件 capability 建立构建矩阵与真实已安装包检查；本机通过
  不代替 Windows/Linux、native display 或真实设备验证。

**验收：** 文档、schema 和 command/control ID 漂移能被 CI 检测；所有新增能力都有
服务/CLI合同测试和适用的 UI smoke，未验证平台明确标注。

## LC-10 / Decision gate — Web 与额外产品入口

- [ ] 评估浏览器端只读审片/远程控制或 C++ Engine WASM 的目标用户、格式、颜色、
  文件访问、模型、内存/线程与数据安全边界；确认收益后再定范围。
- [ ] 优先验证现有 machine contract 能覆盖的体验；完整 Web 编辑器、额外 UI 架构
  或新 renderer 均需单独产品决策，不能因参考项目有 WASM 就自动开工。

**验收：** 有明确可交付场景、单一算法/业务 owner、技术验证与资源预算；没有
决定前保持 pending，不添加占位 app、兼容 shell 或第二套目录/配方。

## 固定参考输入

以下源码仅作为任务依赖；开始实现时重新核实行为与可用测试，保留参考 SHA。

- LightCraft `0ba118de92a81f7463ca0e6d79e1323137b1adae`：
  [render benchmark](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/engine/examples/render_bench.rs)、
  [GPU stages](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/gpu/src/render.rs)、
  [CPU stage plan/cache](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/pipeline/src/lib.rs)、
  [fused finish](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/gpu/src/wgsl/finish.wgsl)。
- 后续能力：
  [command/MCP projection](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/mcp/src/tools.rs)、
  [semantic result](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/develop/src/segmask.rs)、
  [model/denoise lifecycle](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/crates/engine/src/denoise.rs)、
  [RAW evidence](https://github.com/storytold/lightcraft/blob/0ba118de92a81f7463ca0e6d79e1323137b1adae/docs/raw-parity.md)。

## 每项交付门槛

- [ ] 选择当前最高优先级的一个 bounded tranche，先写 owner/寿命/错误/取消与
  最小测试计划，再实现；新公共合同与范围对应 ADR/architecture/capability/testing 更新。
- [ ] 运行受影响 Ravo target、真实 service/CLI/subprocess 合同与静态边界检查；
  公共 header、调度或广泛变更运行完整 suite。性能只采当前 Release，不并行扰动样本。
- [ ] GPU 依 [MIGRATION.md](MIGRATION.md) 和 IQ-00 准入，保存相同输入/配方的 CPU
  参考；不复用 OpenCL、不改冻结 gold、不新增静默 fallback。
- [ ] 报告实际 pass/fail/skip、source/package SHA、像素/性能/资源取舍与未验证平台。
  没有模型、工具链、参考依赖或 corpus 时只完成可用验证，不标记通过。
- [ ] 完成一项即将稳定结论移入所属 authority，删除本文件对应已完成任务；不保留
  实现日记或两个互相冲突的执行计划。
