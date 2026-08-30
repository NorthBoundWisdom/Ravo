---
name: i18n-translation-workflow
description: Ravo Studio Qt/QML multilingual catalog extraction, translation-memory reuse, and package validation.
metadata:
  version: 0.3.0
---

# Ravo Studio i18n workflow

Use this skill when adding or updating Ravo Studio translations. The versioned owner is
`Ravo/desktop/i18n/locales.json`; do not hard-code a parallel locale list.

## Boundaries

- Translate Studio UI strings only. Keep CLI JSON, service/engine errors, and documentation source text in English.
- Update `.ts` catalogs through this workflow, not by manually adding messages.
- Preserve placeholders, URLs, resource paths, file extensions, shortcuts, product names, and explicit line breaks.
- Every manifest locale must be complete before packaging. Missing or invalid catalogs fail explicitly.
- Translation memories are locale-specific `RavoStudio_<locale>.memory.ini` files and are versioned inputs.

## Extraction

```text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py \
  --repo-root . --part 1 --lupdate /path/to/lupdate
```

This extracts every manifest catalog, makes the source catalog an English identity catalog,
and synchronizes active messages into each selected translation memory. Use repeatable
`--locale <code>` only when deliberately updating a subset. `--clean-ts` is destructive and
must be explicitly requested.

## Translation

Fill `<unfinished>` values in each locale memory. Reuse exact translations where the context
and source key are unchanged. Review terminology and naturalness per locale; machine output is
a draft, not acceptance evidence.

## Apply and validate

```text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py \
  --repo-root . --part 2
```

Part 2 refuses incomplete memories, applies them to `.ts`, and runs manifest-wide structural
validation. Then build `ravo_studio_translations`, run the localization contract tests, and run
`ravo_studio_localization_smoke` for package/runtime evidence.
