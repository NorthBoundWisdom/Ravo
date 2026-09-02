# ADR-0115: Typed Studio window size and position

- Status: Accepted
- Date: 2026-09-02
- Extends: [ADR-0066](0066-typed-desktop-language-setting.md)

## Context

Studio opened at a fixed 1440×900 window every launch. Photographers expect the
last interactive size, position, and maximized state to return. ADR-0066 keeps
view-session controls such as zoom and filters transient, but the shell window
is a durable desktop preference with the same fail-closed settings policy as
language and assistant fields.

## Decision

- `StudioWindowGeometry` owns typed windowed `x`, `y`, `width`, `height`, and a
  `maximized` flag. Keys are `desktop/window/x`, `desktop/window/y`,
  `desktop/window/width`, `desktop/window/height`, and
  `desktop/window/maximized`. The first-launch default is 1440×900, unmaximized,
  with platform window placement.
- Valid windowed size is 640–16384 on each edge. Invalid remember requests fail
  without changing stored values. Incomplete or malformed stored records are
  removed together and startup uses the default. Persistence failure is visible
  on the owner and leaves the live window where the user put it.
- Restore fits the stored rectangle onto current available screens so a
  disconnected display cannot hide the window. Maximized restore applies the
  last windowed rectangle first. Minimized and fullscreen sessions do not
  overwrite stored windowed geometry. `--smoke` does not attach or persist.
- QML displays the startup size and asks the owner to restore/attach. QML does
  not write QSettings or own validation.

## Consequences

The next interactive Studio launch returns to the last usable window. Language,
assistant endpoint/model/key, and window geometry are the typed desktop
settings. Zoom, pan, browse mode, and library filters remain session state.

## Rejected alternatives

- Qt.labs Settings aliases in QML. Persistence, repair, and screen fitting
  belong in a typed C++ owner.
- Saving maximized pixel size as the windowed rectangle. Un-maximize would then
  fill the screen.
- Silently ignoring corrupt stored geometry while leaving the broken keys.
