本次审阅固定在 **`main@e4986fc31e3dffd4118628866ad3441ec01fa05e`，版本 0.9.17**，提交时间为 **2026 年 10 月 10 日 20:41（台北时间）**。结束时再次读取，`main` 仍指向该提交。

**本轮最值得处理的是 Gallery 的分页、选择和选片建议筛选。** 我发现四项具体问题，其中一项跨越 CatalogService 的线程边界，另外三项会导致筛选或多选结果不完整。它们都存在于当前版本，但**不是这次着色器修改新引入的回归**。

对新增的 Sigmoid、RCD 显式通道写回，我没有发现确定的数学回归，建议保留；相关测试补强放在后面说明。

## 评审结论

| 优先级 | 问题 | 用户可见影响 |
|---|---|---|
| **P1** | 选片建议筛选在 GUI 线程直接调用工作线程拥有的 CatalogService | 违反数据库线程约束；重复文件扫描还会占用 GUI 线程 |
| **P2** | 选片建议在分页之后才过滤，并把结果标为没有后续页 | 第一页之外的命中照片无法显示 |
| **P2** | 范围选择、全选和批量操作依赖当前已加载的行 | 跨页选择遗漏照片，甚至把空字符串加入选择集合 |
| **P2** | 三个缓存页都含选中照片时，新加载页会立即被淘汰 | 继续滚动或选择第四页的照片可能一直停在占位状态 |

下面按可以直接实施的方式说明。

---

## 1. P1：选片建议必须回到 CatalogService 所属线程执行

### 位置

```text
Ravo/desktop/src/studio_presenter_selection.cpp
    StudioPresenter::setCullSuggestionFilter()

Ravo/services/src/catalog_cull_assistance.cpp
    CullService::find_exact_duplicate_groups()
```

### 当前问题

`setCullSuggestionFilter()` 直接调用：

```cpp
service_->cull().find_exact_duplicate_groups({});
service_->cull().find_near_duplicate_groups({});
service_->cull().propose_burst_groups({});
```

这里没有经过 `executor_.post()`，也没有将结果通过 queued callback 发布回 GUI。

而 `service_` 是在 `createCatalog()`、`openCatalog()` 的 executor 工作中构造和替换的。`CatalogService` 自身也明确约定：Service、Engine、Repository、Decoder 属于同一个 owner thread，Service 本身不是线程安全对象。

这不是单纯“建议放到后台以提升速度”。Qt 的数据库连接必须在其所属线程访问；给调用外围加一把锁并不能修复线程亲和性。[Qt 文档](https://doc.qt.io/qt-6/qsqldatabase.html?utm_source=chatgpt.com)

此外，`find_exact_duplicate_groups()` 内部会读取整个资产列表，并逐个原文件计算 SHA-256。当前调用链意味着这些工作也可能直接发生在 GUI 线程。

### 修改方式

**继续使用现有 `executor_`，不要新建 CullWorker 或把 CatalogService 搬回 GUI。**

将 `setCullSuggestionFilter()` 收敛为三段：

**GUI 段：捕获请求。** 校验 mode，取消旧请求，捕获当前 catalog 会话身份、筛选请求 generation、mode 和 cancellation token。不要在 GUI 中读取或解引用 `service_` 来判断服务是否可用；服务检查放到 worker 内。

**Worker 段：执行分析。** 在 `executor_` 中确认对应 catalog 会话仍有效，再调用 `service_->cull()`。返回不可变的候选 ID 集合或错误，不在 worker 中修改 Presenter 状态。

**GUI 发布段：检查身份后采用结果。** 使用 queued callback，同时检查 catalog 会话、请求 generation 和当前 mode。只有最新请求可以更新 `cull_suggestion_asset_ids_` 并触发列表查询。

尤其要覆盖：

```text
exact_duplicate 开始
    → 用户切换 burst
    → burst 结果先被采用
    → 旧 exact_duplicate 结果不能再覆盖它
```

选择 `none`、关闭 catalog、替换 catalog 时，都应让旧分析失效。

### 同时修复取消传递

当前精确重复分析在文件之间检查 cancellation，但实际哈希调用没有传入 token：

```cpp
auto digest = sha256_file_hex(location.value().path);
```

应改成：

```cpp
auto digest = sha256_file_hex(
    location.value().path,
    request.cancellation);

if (!digest &&
    digest.error().code == ErrorCode::kCancelled)
{
    return digest.error();
}
```

其他哈希错误可以继续按既有合同记录为跳过原因；**取消不能被降级为普通 `hash_failed` 然后继续扫描**。

### 验收

建议增加以下测试，名称为建议新增名称：

| 测试 | 必须证明 |
|---|---|
| `CullFilterRunsOnCatalogOwnerThread` | Repository 访问发生在 catalog owner 线程 |
| `CullFilterRejectsSupersededResult` | 旧 mode、旧 catalog 的结果不能覆盖新状态 |
| `CullHashCancellationStopsCurrentFile` | 取消可以在单个文件哈希过程中生效 |
| `ClearCullFilterCancelsPendingAnalysis` | 清除筛选后，迟到结果不能重新打开筛选 |

线程测试最好在测试 Repository 或调用边界记录线程 ID，避免仅凭“没有看到 Qt 警告”判断通过。

---

## 2. P2：选片建议过滤必须发生在分页之前

### 位置

```text
Ravo/desktop/src/studio_presenter.cpp
    StudioPresenter::reloadVisibleAssets()
    load_catalog_listing()
```

### 当前问题

当前流程是：

```text
根据普通 LibraryQuery 读取一页
    ↓
在 GUI 中用 cull_suggestion_asset_ids_ 过滤这一页
    ↓
listing.total = filtered.size()
listing.has_more = false
```

也就是说，`total` 和 `has_more` 被当前页内的过滤结果覆盖了。

与此同时，候选 ID 来自全 catalog 的重复或连拍分析，而不是只来自第一页面。

### 一个确定的错误场景

```text
当前目录有 1,000 张照片。
前 200 张没有重复。
第 801、802 张是一组精确重复。

开启 exact_duplicate：
    全库分析找到了第 801、802 张
    列表查询只取出前 200 张
    GUI 过滤结果为空
    total 被设置为 0
    has_more 被设置为 false
```

最后界面会显示没有匹配照片，而不是这两张重复照片。这是查询顺序错误，不是缩略图还没加载完。

### 修改方式

把候选 ID 约束加入**现有列表查询 owner**，让查询顺序变成：

```text
目录 / 集合 / 普通过滤条件
    ∩
Cull 候选 ID 集合
    ↓
统一排序和堆栈折叠规则
    ↓
计算 total
    ↓
分页
```

具体实施要求：

**删除 `reloadVisibleAssets()` 中对已分页资产做二次过滤并覆盖 `total/has_more` 的代码。**

在现有 LibraryService/Repository 查询路径中支持候选 ID 约束。首页、后续页、定位指定行、全选 ID 查询必须使用同一约束，不能只修首页。

候选 ID 较多时，不要生成无限增长的单条 `IN (?, ?, ...)`。可以在当前查询 owner 内采用受控分批或连接私有临时表，但不需要新建持久化数据库或新的 Service。

还要显式区分：

```text
没有启用 Cull 筛选
```

与：

```text
启用了 Cull 筛选，但候选集合为空
```

两者不能都用“空 vector 表示不加条件”，否则没有命中时反而会显示全部照片。

### 清除筛选也要一起修

当前 `clearFilters()` 调用了 `library_.resetFilters(...)`，但没有清除根 Presenter 中的：

```cpp
cull_suggestion_filter_
cull_suggestion_asset_ids_
```

因此清除普通过滤条件不等于清除了选片建议过滤。

应在一次 GUI 状态变更中取消旧分析、将 mode 改为 `none`、清除候选约束，最后只触发一次列表重新查询。

### 验收

重点测试三种目录：

| 数据 | 预期 |
|---|---|
| 唯一命中位于第 801、802 张 | 显示两张，而不是空结果 |
| 命中数量超过一页 | `total` 为完整命中数，可以继续分页 |
| Cull 结果为空 | 显示零张，不退回全目录 |

再测试叠加 rating、folder、collection、collapse stacks，以及清除筛选后的结果恢复。

---

## 3. P2：选择集合不能由当前驻留的显示页决定

### 位置

```text
Ravo/desktop/src/studio_presenter_selection.cpp
    selectAssetRange()
    selectAllVisible()
    selected_asset_ids()

Ravo/desktop/src/asset_list_model.cpp
    assetIdAt()
```

### 当前问题

范围选择直接逐行执行：

```cpp
selected_ids_.insert(
    utf8_from_qstring(assets_.assetIdAt(row)));
```

但对于未加载的行，`assetIdAt()` 返回空字符串。

全选则跳过空 ID，所以只选中了已驻留的部分行。后续 `selected_asset_ids()` 又遍历 `assets_.records()`，把选择集合进一步限制在当前驻留记录中。

这个结果会传入实际业务操作。例如 `addSelectionToLibrarySet()` 使用的就是 `selected_asset_ids()`，并不重新解析完整选择范围。

### 隔离验证结果

我按上述循环做了一个不依赖 Qt 的隔离状态验证：

```text
总行数：1,000
已加载：0–199、800–999
请求范围：50–850，包含两端
```

结果：

| 指标 | 当前逻辑 |
|---|---:|
| 用户请求的行数 | **801** |
| `selected_ids_` 大小 | **202** |
| 是否包含空字符串 | **是** |
| 最终可传入批量操作的实际 ID | **201** |

同一个模型执行全选，也只得到已加载的 **400 个 ID**。

这是选择循环的机制验证，不是完整 Ravo UI 回归测试；但遗漏路径不需要依赖特殊线程竞争。

### 修改方式

这里不能只加：

```cpp
if (!id.isEmpty())
```

那只能去掉空 ID，仍然会遗漏尚未加载的照片。

建议建立一个**与显示页缓存无关的、有稳定顺序的选择快照**。继续放在现有 Presenter/LibraryService 内，不需要新的选择管理平台。

**范围选择和全选通过后台查询解析 ID。** 查询必须使用当前目录、集合、过滤条件、排序和堆栈折叠规则。只取 ID，不为了解析选择而加载所有缩略图或把所有 `AssetRecord` 填进 GUI 模型。

可以增加一个内部查询入口，职责类似：

```text
resolve_selection_ids(
    当前查询范围,
    当前显示排序与堆栈规则,
    行范围或全部,
    expected_catalog_revision,
    cancellation
)
```

这是建议新增的内部能力，不是当前已有 API。

**异步选择结果要绑定身份。** 捕获 catalog 会话、列表 generation、selection generation 和数据库 revision。用户切换目录、排序、过滤或再次选择后，旧结果不得生效。解析过程中发生导致行序变化的目录修改，应重新解析或明确拒绝，不能把旧行号套到新列表上。

**批量操作直接使用已确认的选择快照。** 改写 `selected_asset_ids()`，不再通过 `assets_.records()` 过滤。顺序也不应依赖 `unordered_set` 遍历顺序；保留按当前显示顺序解析出来的 ID vector，集合索引只用于 membership 查询。

在解析未完成时，批量操作应暂不可执行，或明确等待选择完成。不能显示“已选择 801 张”，实际只向 Service 传入 201 张。

### 需要顺带核对的入口

完成这一修改后，应检查导出、标签、加入集合、堆栈、批量 Develop 等调用方。凡是通过驻留 `AssetRecord` 判断整组选区是否含视频、是否允许删除的逻辑，也要避免把“未加载”误当成“不存在”或“不含视频”。

### 验收

建议测试：

```text
RangeSelectionResolvesUnloadedRows
SelectAllResolvesTheFilteredResult
BatchCommandUsesCompleteSelectionSnapshot
SelectionResolutionRejectsChangedQuery
```

核心断言应包括：

```text
50–850 得到 801 个非空、无重复 ID
全选得到整个当前过滤结果
页缓存变化不改变已确认选择
批量操作接收到的 ID 与界面确认的 ID 一致
```

---

## 4. P2：选中照片不能无限“钉住”整个显示页

### 位置

```text
Ravo/desktop/src/asset_list_model.cpp
    AssetListModel::setPage()
    AssetListModel::trimPages()

Ravo/desktop/src/studio_presenter.cpp
    StudioPresenter::requestLibraryPage()
```

### 当前问题

`trimPages()` 为保护选中照片，会跳过任何包含选中资产的页。新页已经加入后，再从所有页中寻找不含选择的淘汰对象。

因此存在如下状态：

```text
第 1 页：有一张选中照片
第 2 页：有一张选中照片
第 3 页：有一张选中照片

加载第 4 页：
    前三页均不可淘汰
    第四页还没有选中照片
    第四页成为淘汰对象
```

我对这段淘汰循环做了隔离验证：加入 600–799 行后，被删除的正是刚加入的 600–799 行，`rowLoaded(600)` 再次变为 false。

这与后续选择完成流程冲突：`requestLibraryPage()` 调用 `setPage()` 后才执行 `completePendingLibrarySelection()`；到那时目标行可能已经被淘汰。

界面可能表现为新页保持占位、无法完成点击选择；是否形成持续重复请求还取决于 QML 的通知时序，本次没有做完整运行复现。

### 修改方式

**先完成第 3 项的选择快照，再修改淘汰策略。** 否则放开选中页淘汰后，当前 `selected_asset_ids()` 会立即开始遗漏被淘汰页上的选择。

建议：

**刚加载且当前需要展示的页不能在同一次 `setPage()` 中成为淘汰对象。** 给 `trimPages()` 传入明确的当前页/视口保护信息，保护边界不要依赖“这一页是否已经有选中 ID”。

**移除“任意选中项就保护整页”的规则。** 选择身份由独立的 ID 快照保留；缩略图和完整显示记录仍按容量预算淘汰。最多为当前主照片保留必要的小型状态，不要为每个选中 ID 固定整页。

**页重新加载后，依据选择集合恢复高亮。** 不需要让选择集合随着页面移出内存而变化。

不要把最大页数从 3 改成 30 当作最终修复；那只会把阻塞推迟到第 31 页。

### 验收

建议新增：

```text
NewViewportPageSurvivesPinnedSelectionPages
EvictionPreservesSelectionIdentity
PendingSelectionCompletesAfterPageLoad
```

使用至少四页的数据，在前三页各选择一张，然后请求第四页。应同时满足：

```text
第四页可以显示并选择
前三张照片仍在逻辑选择集合中
驻留记录量维持既定预算
同一个目标页不会被反复读取又立即淘汰
```

---

## 新增着色器改动的意见

### Sigmoid：保留当前改法

当前修改把动态通道赋值改为先计算 `output_minimum / output_middle / output_maximum`，再按通道顺序显式构造 `vec3`。从这次差异看，我没有发现公式或通道排列被改变。新增测试也覆盖了六种严格 RGB 大小顺序，并在 0、0.5、1 三种 hue preservation 下与 CPU 结果比较。

建议把新增测试再补上**两个最大通道相等**的排列，例如：

```text
R = G > B
R = B > G
G = B > R
```

当前九个输入包含全相等以及部分“两个最小通道相等”情况，但没有完整覆盖上述并列最大值排列。这个属于测试补强，不是已经发现的成像 bug。

### RCD：保留显式分量累加

边界累加改为 `.r/.g/.b` 显式写入，当前读取到的修改没有改变采样位置或各颜色的计数公式。

验收应继续比较边缘像素，而不是只比较内部 ROI。尤其关注四种 Bayer 排列、奇数宽高，以及现有支持范围内非 workgroup 整数倍的尺寸。本次没有执行 GPU 测试，因此这里的结论是源码审阅意见，不是 GPU 输出通过证明。

---

## 上轮问题的状态

**当前 `studio_preview.cpp` 的 blob 仍与上一轮相同。** 发布锁依旧先 `tryLock(1000)` 再检查取消；之前指出的缩略图状态与通知修改也尚未进入这个文件。因此不能因版本升到 0.9.17，就把上一轮 Gallery 待办视为关闭。

本轮不重复展开旧问题，建议在任务列表中分开记录：

| 批次 | 内容 | 依赖 |
|---|---|---|
| **A** | Cull 分析回到 owner 线程，补取消和请求身份 | 独立优先处理 |
| **B** | Cull 候选约束进入分页查询，修复清除筛选 | 接 A 的结果发布 |
| **C** | 范围/全选解析完整 ID，批量操作使用选择快照 | 在改变页淘汰前完成 |
| **D** | 去掉选中项对整页的固定，保护当前视口页 | 依赖 C |
| **E** | 完成上轮缩略图状态、通知、锁取消及缓存身份修复 | 与 A–D 分开提交 |

**本轮最重要的设计结论是：显示页缓存只能决定“现在有哪些数据在内存里”，不能决定“查询结果是什么、用户选中了什么、批量操作作用于谁”。**

目前这三个边界仍有混用。把它们分开，比继续增加 Presenter、Worker 或缓存层更有价值。

本次没有修改仓库，也没有运行 Ravo/Qt/GPU 产品测试；执行验证仅包括选择和页淘汰循环的隔离状态验证。上述四项应通过对应的定向回归测试闭合，而不是依靠手动浏览少量照片判断完成。
