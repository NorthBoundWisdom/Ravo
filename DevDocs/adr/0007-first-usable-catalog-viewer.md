# ADR-0007: Deliver the catalog/import/viewer vertical slice first

- Status: Accepted; reference-only import superseded by ADR-0102
- Date: 2026-08-24
- Supersedes in part: ADR-0001, ADR-0005

## Context

The headless-first sequence established a clean C++20 engine, CLI contracts,
RAW decoding, and testable error boundaries. It also postponed catalog and
desktop work until a much larger image-processing migration was complete. That
sequence no longer matches the product priority: the next milestone must be a
usable application that creates a local database, imports images including RAW,
and lets a user view them.

Waiting for the full pixelpipe, all retained operations, and a complete export
stack before exercising database, import, preview, and desktop lifecycles would
delay the first product feedback and leave important cross-layer contracts
untested. Reusing the frozen GTK application or putting SQL and codecs directly
in QML would reach a screenshot sooner but recreate the ownership problems that
Ravo is intended to remove. Leaving the Qt presentation choice open would also
let Widgets and QML grow in parallel and produce two desktop architectures.

## Decision

- The first accepted product milestone is a cross-layer vertical slice:
  create/open a versioned SQLite catalog, reference local JPEG/PNG/TIFF/RAW
  files, generate previews, list assets, and view the selected image.
- Ravo Studio may now add `ravo_domain`, `ravo_services`, and `ravo_desktop`
  targets before the complete legacy pixelpipe is migrated.
- The first desktop uses Qt 6 Quick/QML with a C++20 composition and
  presentation layer. `Qt6::Qml`/`Qt6::Quick` and the `QtQuick.Controls`/
  `QtQuick.Dialogs`/`QtQuick.Layouts` imports are private to `ravo_desktop`;
  Qt Sql is private to the catalog adapter. Desktop-owned QObject presenters
  expose immutable view state and commands. QML owns
  layout, bindings, presentation, and input only; it never issues SQL, decodes
  images, owns background tasks, or reaches engine private state.
- No Ravo production target links Qt Widgets, and the first desktop does not
  provide a Widgets/QML hybrid or fallback. A future change requires a new ADR
  rather than introducing a second presentation architecture incrementally.
- SQLite is accessed through the private QSQLITE adapter. Domain and service
  contracts expose stable value types, repository ports, structured errors,
  cancellation, and immutable result snapshots rather than SQLite or Qt model
  objects.
- Import is reference-only for the first version. Originals are never copied,
  moved, renamed, modified, or deleted. A normalized local URI is unique within
  a catalog.
- Preview files are rebuildable, versioned cache artifacts stored outside the
  database and committed atomically. The database records cache identity and
  state, not original or preview image blobs.
- RAW previews use the existing Ravo CPU engine. JPEG/PNG initially use a
  private `QImageReader` adapter; TIFF follows the same port after its codec
  deployment is proven. Raster and RAW paths join the same orientation, colour,
  sizing, error, and cache contracts.
- The `ravo` CLI remains supported and becomes a second client of the same
  services where headless catalog/import/preview acceptance is useful. The
  desktop must not execute the CLI as a subprocess.
- UI work does not reopen the frozen 0.9 graph. Ravo production targets remain
  independent from `src`, GTK, legacy IOP loading, and global `darktable` state.

## Ownership, lifetime, and threading

The C++ composition root owns the catalog adapter, codecs, engine, services,
task executor, presenters, and QML engine. Destruction happens only after owned
tasks have stopped. The UI thread sends intents and displays immutable
snapshots; scanning, metadata, decode, preview, cache, and database I/O run in
owned cancellable C++ tasks. QML objects never become task or service owners.

Each asynchronous result carries catalog, asset, and request revisions. Results
from a cancelled request or an earlier selection are discarded. Database
transactions publish only trusted state, and preview files become visible only
after atomic commit. Partial decode or cache output is never presented as a
successful asset.

## Consequences

- Product feedback arrives after a narrow database/import/viewer slice instead
  of after the whole editor is rewritten.
- The first slice crosses more layers, so catalog schema, task ownership, error
  recovery, and desktop lifecycle tests become immediate requirements.
- Qt Gui/Qml/Quick, QML modules, and SQLite runtime deployment enter the
  cross-platform build and packaging matrix. They are explicit product
  dependencies, not legacy UI compatibility layers.
- The CPU engine and CLI work remain valuable and testable; the scheduling
  change does not weaken recipe, colour, fixture, or migration contracts.
- Managed-original import, old catalog migration, editing, export, and GPU work
  remain later product decisions and cannot be smuggled into the first slice as
  silent fallback behaviour.

## Rejected alternatives

- **Finish every headless operation first**: delays the first usable product and
  leaves catalog/UI ownership untested for too long.
- **Wrap the frozen GTK application**: creates a production dependency in both
  directions and preserves the old global lifecycle.
- **Let QML use SQLite, codecs, or services as an object graph directly**: is
  fast only for the first screen and prevents service testing, cancellation,
  stable ownership, and safe replacement.
- **Copy originals into a managed library immediately**: adds conflict,
  rollback, disk-space, and data-loss policy before the basic viewer is proven.
- **Use Qt Widgets or keep both Widgets and QML**: duplicates presentation,
  packaging, lifecycle, and test paths. Qt Quick/QML is the single accepted
  desktop architecture.
