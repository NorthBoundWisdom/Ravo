# ADR-0090: Versioned local control for the live Studio session

- Status: Accepted
- Date: 2026-08-30
- Extends: [ADR-0003](0003-versioned-machine-contract.md),
  [ADR-0011](0011-atomic-develop-publication.md),
  [ADR-0079](0079-develop-set-inventory-and-probe-png.md),
  [ADR-0080](0080-studio-observes-catalog-revision.md), and
  [ADR-0087](0087-progressive-develop-preview.md)
- Partially supersedes: [ADR-0081](0081-studio-assistant-endpoint-panel.md)

## Context

The catalog CLI could inspect and mutate a named asset, but it could not
authoritatively identify the photo selected in a running Studio window, observe
pending Develop state, or bind a mutation to the selection that an agent had
actually observed. Process arguments, logs, open files, and preview-cache
activity cannot provide that contract. They also cannot reject a request if the
user changes photos while an agent is deciding what to edit.

## Decision

- `ravo_control` owns `ravo-studio-control/v1`, strict request/response framing,
  a 4 MiB message bound, owner-only discovery descriptors, and Qt local-socket
  transport. It contains no catalog, recipe, renderer, command policy, or
  assistant state. Each Studio process publishes a unique session and removes
  its descriptor and endpoint during normal destruction; unreachable stale
  descriptors are ignored. After a client connects, every success and failure
  path performs a bounded graceful disconnect and aborts any handle that does
  not settle, so a Windows named-pipe endpoint is reusable immediately after
  discovery's `ping`.
- `StudioLiveSessionController` is a desktop C++ owner on the UI thread. Its
  immutable snapshot identifies the executable/workspace, process/session and
  state revisions, catalog path/revision, primary and selected assets, browse
  mode, current/saved canonical recipes, baseline-relative modified
  operations, pending changes, and the current preview's bounded identity.
  Assistant URL/model/key and all other settings are absent.
- A Develop mutation carries the observed session revision, selection revision,
  and asset ID. The controller rejects stale, wrong-asset, busy, unavailable,
  duplicate, non-finite, unknown, and out-of-range requests before mutation.
  One ordered batch enters Develop when needed and commits through the existing
  `StudioCommandController`, presenter, `CatalogService`, history transaction,
  cancellation owner, and progressive preview path.
- The mandatory client is `ravo studio sessions|state|develop|preview --json`.
  Session resolution prefers the checkout containing the CLI's current working
  directory; multiple matching sessions require `--session-id`. Explicit
  expected revisions remain available to bind a later command to an earlier
  snapshot.
- Image bytes do not cross the control socket. `studio preview`, and
  `studio develop --output`, render the snapshot's exact canonical recipe
  through the existing non-persistent CatalogService/Engine preview path. The
  CLI rechecks selection and recipe revisions before atomic no-replace PNG
  publication and reports path, MIME type, dimensions, profile identity,
  SHA-256, byte size, and caller-owned lifecycle.
- Qt Network is allowed in `ravo_control` for `QLocalServer`/`QLocalSocket` and
  in CLI solely as its local client. Assistant HTTP and credentials remain
  desktop-only under ADR-0081; no network listener is introduced.

## Consequences

An agent can inspect the photo the user actually selected, understand both
saved and pending edits, commit strict parameter batches, wait for the exact
saved preview, and inspect a reproducible PNG without UI automation. Session,
selection, recipe, and preview revisions make races explicit instead of
redirecting work to another photo.

The local transport adds a small discovery registry and Qt Network dependency
to the control/CLI targets. Normal shutdown cleans discovery state; crash
residue is harmless because discovery proves the endpoint is live. Windows and
Linux retain the same Qt local-socket source contract but require target-host
execution before their runtime result can be claimed.

## Rejected alternatives

- Deriving the current photo from process arguments, SQLite handles, logs,
  cache timestamps, or the foreground window. None carries selection revision
  or pending recipe state.
- Sending screenshots or raw image buffers over the socket. That would create
  an unbounded transport and another pixel oracle.
- Letting an MCP adapter own live state or mutations. MCP may later project
  this exact protocol, but cannot add another command, renderer, or permission
  path.
- A network listener or persisted writable singleton. Live state is local,
  same-user, multi-session, and destroyed with its desktop owner.
