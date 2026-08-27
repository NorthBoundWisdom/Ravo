# Settings

## Goal

Change the current Studio interface language and understand what Settings does
and does not configure in this baseline.

**Last verified:** 2026-08-27 against the current Studio language manager and
Settings page.

## Applies to

- Ravo Studio.

## Prerequisites

- Ravo Studio is running.

## Open Settings

Choose **File → Settings**, or use the command palette to search for
**Settings…**. Press **Back** or `Esc` to return to the workspace.

## Current setting: Language

Studio currently supports:

- **English** (`en_US`)
- **Simplified Chinese** (`zh_CN`)

Choose a language from the Language control. The selection is persisted in the
desktop settings and the QML interface is retransmitted immediately. Machine
errors from the catalog and engine remain in their structured source form; they
are not hidden by translation.

If the Chinese translation package was not included in a build, selecting
Simplified Chinese shows a clear package-missing error. Reinstall or rebuild the
translation target before retrying.

## What is not a Studio setting yet

The current Settings page does not provide account, cloud, AI, monitor-profile,
or general photo-library synchronization settings. Ravo's catalog and preview
paths are local. Color management is chosen per recipe in the Edit pane, not by
guessing the monitor profile from QML.

## Result

Studio uses the selected supported language on the next launch and in the
current window after a successful switch.

## Common questions

### Why is there no language list for every system locale?

Only English and Simplified Chinese are part of the current Studio translation
contract.

### Why did a language switch fail?

The requested language may be unsupported, or the build may be missing the
produced Chinese `.qm` package. The error text identifies the condition.

### Does changing the language change photo rendering?

No. It changes labels and command presentation only. Recipes and rendered
pixels are independent of UI language.
