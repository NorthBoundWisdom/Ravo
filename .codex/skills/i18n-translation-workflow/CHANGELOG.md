# i18n-translation-workflow Changelog

## [Unreleased]

## [0.2.0] - 2026-08-26

- 从 RobimPCR 迁移到 Ravo，改用 Ravo/desktop、RavoStudio_zh_CN.ts 和
  RavoStudio_en_US.ts。
- lupdate 输出不再记录机器相关的源文件路径，并移除过时 TS 项；历史翻译继续由
  zh_translate.ini 保存和复用。
- 新增 Linux lupdate 搜索路径，并使 QML 自动标记的失败返回非零。

## [0.1.0] - 2026-04-07

- 建立独立版本号管理：在 `SKILL.md` 中声明 `version`，便于跨 repo 同步时判断升级范围。
- 为仍在持续演进的翻译工作流设定 `0.x` 初始版本，后续在行为稳定后再进入 `1.x`。
