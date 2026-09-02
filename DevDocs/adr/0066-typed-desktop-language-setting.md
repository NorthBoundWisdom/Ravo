# ADR-0066: Product settings are typed and consumer-owned

- Status: Accepted; assistant endpoint settings added by
  [ADR-0081](0081-studio-assistant-endpoint-panel.md); window geometry added by
  [ADR-0115](0115-typed-studio-window-geometry.md)
- Date: 2026-08-29
- Extends: [ADR-0007](0007-first-usable-catalog-viewer.md)

## Context

The old application exposed a global string-key configuration store to core,
jobs, image I/O, modules, and GTK. Its crawler also coupled preferences to
background thumbnail generation and automatic sidecar discovery. Ravo needs a
setting only when a current product consumer owns its type and lifecycle; it
must not import the old key space or revive global configuration access.

## Decision

- The only persistent Studio product preference currently required is the UI
  language. `StudioLanguageManager` owns the typed `en_US | zh_CN` value and
  the private `desktop/language` QSettings key.
- Language aliases are normalized before use. An unsupported explicit request
  fails without changing the active language or persisted value. A malformed
  stored value is removed synchronously and startup falls back to English.
- A user-initiated switch installs and verifies any required translation
  package, synchronously persists the normalized value, and only then changes
  the active translator. Persistence failure is visible and leaves the prior
  language active.
- Zoom, pan, browse mode, scope mode, thumbnail layout, and current library
  filters are session presentation state. Color, export, recipe, catalog, and
  task values remain in their typed owning contracts rather than QSettings.
- There is no legacy-key reader, bulk preference migration, stringly typed
  service configuration facade, automatic sidecar crawler, or background
  thumbnail preference. Preview work is demand-owned and cancellable.

## Consequences

J7's Ravo product-setting contract is accepted. The shared old `control/conf*`
and `settings.h` files remain until their old engine/UI consumers are retired;
`crawler*` remains until its direct control/history/GUI callers reach zero.
They are not Ravo dependencies. The U5 GTK preference/style/preset windows may
now be retired after a whole-repository consumer check.

## Rejected alternatives

- Importing the old configuration database or preserving its keys. Most keys
  describe removed GTK, dynamic-module, OpenCL, cache, or sidecar behavior.
- Persisting every QML control. View-session state does not warrant migration,
  compatibility, or recovery policy.
- Falling back silently when a translation package or settings write fails.
  The user-visible state must match durable state.
