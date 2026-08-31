# ADR-0093: Versioned Studio locale catalog

## Status

Accepted, 2026-08-30.

## Context

ADR-0066 introduced a typed desktop language preference for English and
Simplified Chinese. Expanding Studio to mainstream languages must not create
independent locale lists in QML, C++, CMake, tests, packaging, and translation
scripts. Localization remains presentation-only: CLI JSON and service/engine
errors are machine contracts and remain English.

## Decision

`Ravo/desktop/i18n/locales.json` is the versioned owner of canonical locale
codes, native display names, system-locale aliases, TS catalog names, provider
language codes, and translation-memory names. It currently declares `en_US`,
`de_DE`, `es_ES`, `fr_FR`, `pt_BR`, `zh_CN`, `zh_TW`, `ja_JP`, and `ko_KR`.

The manifest is embedded in Studio. `StudioLanguageManager` parses it on the Qt
main thread and owns `QTranslator` installation and removal. A candidate locale
is normalized and its QM is loaded and installed before the active translator
or persisted preference changes. Unsupported aliases, malformed manifests,
missing or invalid QM files, install failure, and settings-write failure are
explicit errors and leave the prior active language unchanged. Corrupt stored
state is removed and repairs to the English source locale.

CMake derives the complete TS/QM inventory from the same manifest. The build
rejects missing, extra, incomplete, or structurally invalid catalogs before
packaging. Only generated QM files deploy; TS catalogs and locale-specific
translation memories remain repository inputs. CJK UI font candidates are
ordered for the active script without changing application data or rendering
algorithms.

## Consequences

Adding a locale is one manifest change plus a complete TS catalog and memory.
There is no runtime fallback to a related language and no partial language
package. English remains the repair/default locale, not a silent substitute for
a selected locale whose package is missing. Documentation source, CLI output,
versioned JSON, and service/engine diagnostics remain English.

## Validation

- Run the project i18n workflow and manifest-wide structural checker.
- Build `ravo_studio_translations` and the desktop command tests.
- Load every manifest QM in the localization contract test.
- Verify alias normalization and explicit missing-catalog failure.
- Run `ravo_studio_localization_smoke`, which launches every locale offscreen.
