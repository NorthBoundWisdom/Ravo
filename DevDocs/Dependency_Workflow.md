# FreeCM source-root 依赖联调工作流

本文说明 Ravo 如何使用 FreeCM 管理第三方源码、如何在本地切换到依赖源码联调，
以及如何把联调结果安全地固化为可复现的 pinned 基线。规则适用于 Windows、macOS 和 Linux；示例中的
本地路径只代表当前机器的活动配置，不应进入受版本控制的模板。

## 真相源与目录所有权

当前工作区实际使用什么依赖，首先看本地活动锁，而不是只看提交中的模板。

| 路径 | 所有权与用途 |
| --- | --- |
| `source_roots.lock.jsonc.in` | 受版本控制、供评审和发布使用的直接依赖基线；永久 pin、公共 CMake 默认值和依赖清单在这里修改。 |
| `source_roots.lock.jsonc` | 被忽略的本机活动锁；决定当前 checkout 的 mode、manual path 和本机 CMake 配置，可为联调手工修改，但不得提交。 |
| `build/dependency_seed_repos/*` | `--init` 准备的本地 Git seed；为离线解析和物化提供对象与 ref，不是父仓库源码。 |
| `build/dependency_source_roots/*` | `--update`/`materialize` 生成的 concrete source root；可被替换，绝不直接修改。 |
| `CMakePresets.json` | `--update` 生成的当前主机 preset；不手工修改或提交。 |
| `.freecm.workspace.lock` | Python workflow 与 FreeCM 插件共用的短生命周期互斥目录；不手工创建或提交。 |

根锁只声明 Ravo 的直接依赖。若依赖自己的锁模板声明 sibling 依赖，FreeCM 会从本地 seed
递归解析 transitive closure；不要为了复制 closure 而把所有传递依赖重复写进根锁。

当前直接依赖名为：`LibRaw` 和 `GeoControls`。
Quill 通过 `find_package(quill CONFIG REQUIRED)` 从主机 CMAKE_PREFIX_PATH
（vcpkg / Homebrew / Qt 前缀）解析，不进入 source-root 锁。
联调时 `depsManualPath` 的键必须使用这些逻辑依赖名，而不是猜测目录名。

## FreeCM 子模块跟踪

本仓通过 `.gitmodules` 跟踪 `FreeCM/master`。只有任务明确要求刷新 FreeCM 时才从干净的主仓根目录
执行：

```text
git status --short --branch
git -C FreeCM status --short --branch
git submodule update --remote --checkout FreeCM
```

不要在通常 detached 的 `FreeCM/` 中运行 `git pull`。刷新前后 gitlink 相同就是 no-op；若改变，先
解释 `git submodule status` 前缀，并运行：

```text
python3 -m repomgrcpp.tools.repo_tool check-lock-compat --repo-root .
python3 FreeCM/tools/validate_repo_commands.py .
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

随后执行当前平台 Ravo configure/build。主仓或子模块存在无关改动时停止刷新；未经用户明确要求不提交
或推送 gitlink。

## 三种依赖模式

- `pinned`：使用 `dependencies.<name>.commit` 中的精确提交；受版本控制的模板必须以此模式作为可复现基线。
- `latest`：使用 seed 中本地可见的最新 ref/commit；它不会联网刷新 seed，适合临时解析，不是发布基线。
- `manual`：只对 `depsManualPath.<name>` 非空的依赖使用指定真实 checkout；空字符串继续回退到受管解析结果，
  因而可以只联调一个依赖而不重写全部路径。

`manual` 和 `latest` 都是本地活动状态。不要把机器路径或移动 ref 写入
`source_roots.lock.jsonc.in`。

## 本机 CMake 与 preset 配置

`cmakeEnvironment`、`cmakeCacheVariables` 和 `terminalPath` 也由活动锁驱动。FreeCM 生成 preset 时先
应用公共值，再叠加当前主机的 `win`、`mac` 或 `linux` map。个人 SDK 位置、临时工具路径和只适用于
一个 checkout 的开关应留在活动锁或外部工具链环境；只有团队评审过的跨机器默认值才进入模板。

如果活动锁显式定义 `cmakeCacheVariables.CMAKE_PREFIX_PATH`，它会完全取代 FreeCM 自动生成的 managed
dependency install prefixes；不要把它当作自动追加项。`--update` 解析出的 source-root 环境变量则会覆盖
同名 `cmakeEnvironment` 值，确保 pinned 与 manual root 按实际解析结果进入 preset。修改这些字段后同样
必须重新运行 `--update`，不得直接修补生成的 `CMakePresets.json`。

## 首次准备与显式维护

首次准备工作区：

```text
git submodule update --init FreeCM
python configs/source_root_workflow.py --init
python configs/source_root_workflow.py --update
```

macOS/Linux 可把 `python` 写作 `python3`。

`--init` 是唯一允许联网的依赖步骤。它会在需要时从模板创建活动锁，克隆或准备 seed closure，
并准备锁中声明的远端资产。

`--update` 是纯离线步骤，只会：

1. 获取 `.freecm.workspace.lock`，读取本机活动锁和已有 seed closure；
2. 按 `pinned`、`latest` 或 `manual` 解析当前 concrete roots；
3. 物化/刷新 `build/dependency_source_roots/*`，或选择显式 manual checkout；
4. 把解析后的 source-root 环境写入当前主机的根 `CMakePresets.json`；
5. 执行本仓库的 host update callback。

`--update` 不会 clone、fetch、pull、下载资产，也不会编译依赖、配置 Ravo 或运行测试。seed 缺失时应先
显式运行 `--init`；不要在 Build/Test/Run 动作中暗中补联网步骤。生成的 preset 会把解析后的 roots
提供给 Ravo，当前 Ravo 构建图实际引用到的依赖只在后续 configure/build 中编译。

普通构建和诊断只运行下一节的只读检查。当前 FreeCM 提供以下显式维护动作：

| 动作 | 网络 | 作用与边界 |
| --- | --- | --- |
| `--refreshpin` | 禁止 | 要求活动锁和模板已是 `pinned`，只把活动锁的 dependency commits 对齐到 committed template；不切 mode，也不物化 roots。 |
| `--pinlatest` | 禁止 | 从本地 seed 已可见的最新提交生成本地候选并离线 update；不 fetch，也不证明提交已发布。 |
| `--update` | 禁止 | 按当前活动锁物化 source roots、生成 host preset 和执行 update callback。 |
| `--cleanbuild --dry-run` | 禁止 | 列出保守清理目标，不删除任何内容。 |
| `--cleanbuild` | 禁止 | 清理普通 build 输出，保留 `dependency_seed_repos` 和 `dependency_source_roots`。 |

`latest` 只用于明确的依赖刷新，不是可提交状态。CLI `--pinlatest` 会把活动锁留在 `latest`；FreeCM
面板的同名动作会生成 pinned snapshot，两者的收尾语义不同。CLI 整体刷新顺序为：确认所有相关
worktree 干净并私下保存原活动锁 → `--init` 联网准备 seed → `--pinlatest` 离线选择候选 → Ravo 快验 →
验证远端 SHA → 更新 committed template → 仅把活动锁的 `depsMode` 显式切回 `pinned` →
`--refreshpin` → `--update` → 再次 verify/configure/build/test。候选失败时立即停止验收，不修改 template；
恢复原活动锁并运行 `--update` 恢复物化状态后，报告原始失败和恢复结果。

清理前必须先运行 dry-run 并核对每个目标；不要用自写递归删除命令替代 FreeCM 的 containment 检查。

## FreeCM 项目命令 manifest

`configs/freecm.commands.jsonc` 使用 schema version 2。Config 是活动上下文；每个 Build、Run、Test、
Package variant 都必须声明非空 `configurations`，并绑定兼容的 Config。下游动作不得隐式执行
Configure，Config 的 inputs/outputs 用于判断当前生成状态是否就绪或过期。

修改 manifest、配置入口或 readiness 后运行：

```text
python3 FreeCM/tools/validate_repo_commands.py .
```

Ravo 当前把 Debug/Release 与 Windows/macOS/Linux 分成独立 Config；Build/Run/Test/Install 继续通过
`Ravo/tools/freecm_project.ps1` 或 `Ravo/tools/freecm_project.py` 执行，不直接复制长 CMake 参数。

## 检查当前活动状态

在修改依赖或判断错误归属前，先运行：

```text
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
```

- `show` 显示最终环境变量和 concrete root；
- `resolve` 显示 direct/transitive closure、每个依赖的 mode、commit、manual override、父子关系和路径；
- `verify` 检查最终 root 是否存在并包含 `configs/source_roots.py` 声明的必要文件。

更深入的离线诊断：

```text
python configs/source_roots.py graph --format json
python configs/source_roots.py graph --format dot
python configs/source_roots.py audit --format json
python configs/source_roots.py policy-check --format json
python configs/source_roots.py explain-conflict <dependency-name> --format json
```

本仓库的 generic FreeCM binding 使用 `show`，不使用其他 host 可能提供的 `status` 子命令；以
`python configs/source_roots.py --help` 的实际输出为准。

## 本地依赖联调

### 1. 选择真实可编辑 checkout

优先使用开发者明确提供的 sibling checkout，例如 Windows 的 `D:/work/LibRaw` 或 macOS/Linux 的
`/work/LibRaw`。不要把 `build/dependency_source_roots/LibRaw` 当作源码仓库修改；下次
`--update` 就可能覆盖它。

如果没有独立 checkout，且明确决定把 FreeCM seed 当作临时真实依赖仓库，可使用
`build/dependency_seed_repos/<repoName>`，但必须先确认：

```text
git -C build/dependency_seed_repos/<repoName> status --short --branch
```

seed 必须处于你理解的分支/提交且没有未知改动。应先完成下述 manual 接线，再修改 seed；有本地改动时
不要把它当作可随意刷新或删除的缓存。即使 manual path 正好指向受管 seed，`--init` 仍会把它当作
受管 seed：干净时可能同步回远端默认分支，脏时会拒绝覆盖。seed 联调期间不要运行 `--init`；缺失的
其他 seed 应在开始联调前准备完成。

### 2. 只修改本机活动锁

把 `source_roots.lock.jsonc` 的 `depsMode` 改为 `manual`，只填写需要联调的依赖。下面只是相关字段摘录；
实际编辑时必须保留原文件的 `schemaVersion`、`dependencies`、资产和本机 CMake 配置：

```jsonc
{
  "depsMode": "manual",
  "depsManualPath": {
    "LibRaw": "D:/work/LibRaw",
    "GeoControls": ""
  }
}
```

Windows JSON 路径推荐使用 `/`，避免反斜杠转义。其他依赖保留空字符串即可继续使用 pinned/受管 root。
不要为了本地联调修改或提交 `source_roots.lock.jsonc.in`。

### 3. 重新接线并验证路径

```text
python configs/source_root_workflow.py --update
python configs/source_roots.py show --format json
python configs/source_roots.py resolve --format json
python configs/source_roots.py verify
```

必须在 `show`/`resolve` 中看到目标依赖指向 manual checkout 后，才修改和构建依赖代码。活动锁或 manual
path 变化后总要重新执行 `--update`，否则现有 `CMakePresets.json` 仍可能包含旧 root。

随后只操作 Ravo；按任务要求配置/编译，并仅在需要行为验证时运行 Ravo tests。本仓库冻结的旧 0.9
工程仍不得运行。Windows 入口示例（`Test` 为按需动作）：

```powershell
& .\Ravo\tools\freecm_project.ps1 -Action Configure -Configuration Debug
& .\Ravo\tools\freecm_project.ps1 -Action Build -Configuration Debug
& .\Ravo\tools\freecm_project.ps1 -Action Test -Configuration Debug
```

### 4. 发布依赖，再更新父仓库 pin

一旦联调依赖产生要被父仓库引用的提交，按依赖拓扑从底层向上执行：

1. 在真实依赖仓库完成验证、提交并 push；
2. 运行 `git ls-remote <remote> <published-ref>`，确认已推送 branch/tag 返回的 SHA 与目标提交一致；
3. 把 `source_roots.lock.jsonc.in` 中对应 direct dependency 的 `commit` 更新为已发布 SHA，保持
   `depsMode: pinned`；
4. 在本机活动锁中把同一依赖切回 pinned SHA，同时保留本机专属 CMake 配置；
5. 重新运行 `--update`、`verify`、受影响的 Ravo configure/build/test；
6. 只提交模板、配置/消费代码和相应文档，不提交活动锁、preset 或 build 目录。

禁止让父仓库 lock、gitlink 或后续提交依赖只存在本机而尚未 push 的依赖提交。若变更涉及多层依赖，
必须先发布最底层，再更新中间层，最后更新 Ravo。

`python configs/source_roots.py pin --dep <name> --ref <ref>` 可从本地 seed 解析 ref 并写入活动锁，
但它不能替代对 tracked 模板的评审，也不能证明提交已经发布到远端。

## 常见错误

| 现象 | 处理 |
| --- | --- |
| 修改后被 `--update` 覆盖 | 修改了 `build/dependency_source_roots/*`；切换 active lock 到 manual，并改真实 checkout。 |
| `--update` 报 seed 缺失 | 显式运行一次 `--init`；不要给离线路径增加隐式网络 fallback。 |
| 构建仍使用旧源码 | 检查 active lock、运行 `--update`，再用 `show`/`resolve` 核对生成 preset 中的 root。 |
| manual path 无效 | 检查依赖逻辑名、绝对路径与必要文件，然后运行 `verify`。 |
| nested pin 冲突 | 运行 `graph`、`audit` 和 `explain-conflict`，修正真正声明冲突的根或父依赖模板。 |
| seed 有未知改动 | 停止自动刷新，先确认所有权；不要覆盖或删除开发者改动。 |
| 远端找不到 pinned SHA | 先 push 依赖提交，用 `git ls-remote <remote> <published-ref>` 核对 ref 返回的 SHA，再更新父仓库模板。 |
| `.freecm.workspace.lock` 超时 | 查明报告中的 owner 进程；不要同时让插件和 Python workflow 修改活动锁。 |
| 想清理旧构建但担心依赖丢失 | 先运行 `--cleanbuild --dry-run`；确认后使用 `--cleanbuild`，不要删除 dependency roots。 |
| FreeCM checkout 与 gitlink 不同 | 先检查主仓/子模块 dirty state 和 submodule 前缀；只在明确刷新任务中从主仓根运行 `--remote --checkout`。 |

不要用 `FetchContent`、CMake 下载、替代 submodule、源码复制或直接修补生成目录绕过上述工作流。

## GitHub Actions

`.github/workflows/ci.yml` 在干净 runner 上走同一条 `--init` / `--update` /
`cmake --preset` 路径。它只写被忽略的活动锁：把当前平台的 `CMAKE_PREFIX_PATH`
换成 runner 上的 Qt 与 host prefix，并把 `git@github.com:` remote 改写成 HTTPS。
模板 `source_roots.lock.jsonc.in`、本机活动锁和生成的 `CMakePresets.json` 都不作为
CI 的提交产物。GitHub Actions 里安装 GTest/Quill（以及 Windows 上的 zlib/libpng）
属于 runner 主机准备，不是 Ravo CMake 图里的 FetchContent。
