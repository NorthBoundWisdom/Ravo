# Settings

## Goal

Change the current Studio interface language and understand what Settings does
and does not configure in this baseline.

**Last verified:** 2026-08-29 against the current Studio language manager and
Settings page.

## Applies to

- Ravo Studio.

## Prerequisites

- Ravo Studio is running.

## Open Settings

Choose **File → Settings**, or use the command palette to search for
**Settings…**. Press **Back** or `Esc` to return to the workspace.

## Current settings

### Language

Studio currently supports:

- **English** (`en_US`)
- **Simplified Chinese** (`zh_CN`)

Choose a language from the Language control. The selection is persisted in the
desktop settings and the QML interface is retransmitted immediately. Machine
errors from the catalog and engine remain in their structured source form; they
are not hidden by translation.

If a stored value is malformed, Studio removes it and starts in English. A
failed settings write leaves the previous language active and reports the
failure; unsupported requested languages likewise do not change the stored
value.

If the Chinese translation package was not included in a build, selecting
Simplified Chinese shows a clear package-missing error. Reinstall or rebuild the
translation target before retrying.

### Assistant

The Assistant panel uses an OpenAI-compatible HTTP endpoint. Settings stores:

- **URL** — API base, default `https://api.x.ai/v1`
- **Model** — model identifier, default `grok-4.5`
- **API key** — optional stored secret. If empty, Studio uses the process
  environment `XAI_API_KEY` at send time only.

Invalid URL or model values are rejected and do not overwrite a good stored
value. A malformed stored URL or model is removed and replaced with the
default. The key is never written to logs.

Open or hide the floating panel from the top toolbar **Assistant** button
(next to Gallery/Edit), **View → Assistant**, or `Cmd/Ctrl+Shift+A`. It does
not block Gallery or Edit. Ask about the selected photo or a Develop edit; it
does not change the catalog by itself.

## What is not a Studio setting yet

The Settings page does not provide monitor-profile guessing or general
photo-library synchronization. Color management is chosen per recipe in the
Edit pane.

## Result

Studio uses the selected supported language on the next launch and in the
current window after a successful switch.

## Common questions

### Which languages are supported?

The Studio translation contract includes English, German, Spanish, French,
Brazilian Portuguese, Simplified and Traditional Chinese, Japanese, and Korean.

### Why did a language switch fail?

The requested language may be unsupported, or the build may be missing its
produced `.qm` package. The error text identifies the condition.

### Does changing the language change photo rendering?

No. It changes labels and command presentation only. Recipes and rendered
pixels are independent of UI language.
