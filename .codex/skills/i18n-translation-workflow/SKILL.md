---
name: "i18n-translation-workflow"
description: "Ravo Studio Qt/QML 中英本地化更新、翻译记忆复用和译包验证工作流。"
version: 0.2.0
---

# Ravo Studio i18n 翻译工作流

本 skill 是 Ravo Studio 的翻译资产唯一更新入口。它维护：

- Ravo/desktop/i18n/RavoStudio_en_US.ts
- Ravo/desktop/i18n/RavoStudio_zh_CN.ts
- Ravo/desktop/i18n/zh_translate.ini

zh_translate.ini 是持久翻译记忆：更新 source 后，已翻译条目会按
context::source 优先、source 次之复用；它可保留已经从当前 UI 移除的历史条目。
.ts 则始终只反映当前 source。不要手工编辑 .ts，也不要在 CMake 构建目录保存翻译源文件。

## 正常入口

    python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 1
    # 只修改 zh_translate.ini 中值为 <unfinished> 的项，保持 key 与顺序不变。
    python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --part 2

新项目首次建库或需要全自动草稿时：

    python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py \
      --part all --auto-translate --translation-provider mymemory

可选 OpenAI provider 只在显式传入 --auto-translate --translation-provider openai 时
调用，读取 OPENAI_API_KEY 或 --openai-api-key；不得把密钥写入仓库。自动翻译后的
术语、快捷键、占位符、产品名和文件格式必须人工复核。

## 阶段和脚本

- 1_add_qstr_to_qml.py：保守地把直接 QML 显示文本标记为 qsTr()。
- 2_update_translations.py：用 lupdate 扫描 Ravo/desktop，刷新中英 TS。
- 3_add_json_strings_to_ts.py：只在 Ravo/desktop/config/ 存在 JSON UI 字符串时运行。
- 4_update_zh_trans_ini.py：把新 source 与持久中文翻译记忆同步。
- 5_apply_chinese_translations.py：将记忆写回中文 TS。
- 6_fix_english_translations.py：将英文 TS 的 source 文本补全为英文翻译。

0_update_ts.py -p1 编排 1–4，-p2 编排 5–6；run_i18n_workflow.py 是首选入口。

## 强约束

1. 先运行第一阶段，再补齐 zh_translate.ini，最后运行第二阶段。
2. 只能修改 <unfinished> 的 value；不可增删、重排或改写 key。
3. 保留 %1、%L1、%n、\\n、格式串、URL、资源路径、文件扩展名、JSON/QML/C++ 标识符。
4. C++ 可见文本必须使用 Qt 可提取的 QCoreApplication::translate 或
   QT_TRANSLATE_NOOP，不能依赖未注册的自定义包装函数。
5. 成功标准是中英 TS 均无 active type="unfinished"，再由
   ravo_studio_translations 编译 .qm。缺少 lupdate、无法解析的 ini、顺序破坏、
   未完成翻译或无效 XML 都必须失败。

## 翻译原则

- 中文采用简体中文、摄影软件语境和稳定术语；Ravo Studio、RAW、ICC、D50、sRGB 等产品和
  技术专有名词按语境保留。
- 相同 source 在不同 context 语义不同，优先使用 context::source 记忆项。
- 不确定的术语保持英文并记录待确认，不得猜测或制造语义替换。
- 译包缺失时运行时会明确告警并显示英文；这不是翻译更新成功的替代结果。
