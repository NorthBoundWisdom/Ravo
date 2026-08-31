# ADR-0032: Publish encoded bytes through one atomic no-replace boundary

- Status: Accepted
- Date: 2026-08-27

## Context

Ravo pixel encoders already returned complete encoded byte vectors to
`CatalogService`, but the final writer used one fixed temporary suffix, removed
that path before opening it, and published with a rename that could replace a
late race winner. It did not synchronize the temporary file, distinguish
write/sync/close failures, preserve stage context for disk exhaustion, or check
cancellation immediately before publication.

ADR-0028 accepted stronger destination primitives for original-copy export.
Encoded output needs the same destination ownership and no-clobber guarantees,
without treating an in-memory encoder result as a synthetic source file or
mixing codec, metadata, and storage policy into the publication owner.

## Decision

- Destination descriptor, temporary ownership, unique adjacent naming, native
  writes, synchronization/close, and atomic no-replace publication are private
  service primitives shared by encoded output and original copy. The
  original-copy source stream and source-change contract remain separate.
- Encoded publication consumes an opaque, immutable byte vector and one
  explicit destination. It writes in bounded 64 KiB chunks. An empty vector is
  a valid exact empty file; codec owners remain responsible for rejecting an
  empty result when their format requires bytes.
- The temporary is opened with native exclusive-create semantics. Only a path
  successfully created by this call enters owned cleanup. The old fixed
  `.ravo-export-tmp` sentinel and unrelated files are never removed.
- The temporary is fully written, synchronized, and closed before publication.
  Publication uses `renamex_np(RENAME_EXCL)` on macOS,
  `renameat2(RENAME_NOREPLACE)` on Linux, and `MoveFileExW` without a replace
  flag on Windows. There is no overwrite fallback; a pre-existing output or a
  late concurrent winner remains untouched.
- Cancellation wins at entry, chunk boundaries, before synchronization/close,
  and immediately before publication. Errors preserve the existing `path`
  context and add stable `reason` and `output` context. Temporary open, write,
  sync, close, target conflict, and publish failures remain distinct; disk
  exhaustion keeps its stage reason and adds `disk_full=true`. Cleanup never
  replaces the primary failure.
- A private four-argument overload accepts a synchronous, non-owning,
  `noexcept` checkpoint hook for deterministic cancellation, race, and failure
  tests. The existing three-argument service API remains the product call and
  delegates with no hook.

## Consequences

All current Catalog pixel exports keep their existing service flow while final
encoded bytes gain exclusive temporary ownership, synchronized file contents,
atomic no-replace publication, and complete deterministic failure evidence.
Original-copy retains its exact source and error contracts while reusing the
same destination primitives.

This tranche does not synchronize the parent directory after the atomic move.
It does not own EXIF/XMP construction, JPEG or other codec options, path
templates, batch scheduling, storage collision policy, or sidecars. Windows and
Linux branches are retained by construction but were not executed in this
macOS tranche. I14 and legacy storage retirement therefore remain incomplete.

## Rejected alternatives

- Keep the fixed temporary suffix and remove it before use: a stale or foreign
  file could be deleted, and parallel exporters would share mutable state.
- Check destination existence and then use a replacing rename: a late writer
  could still be overwritten between those operations.
- Reuse original-copy by writing a synthetic source file: that would add an
  unnecessary transfer and conflate source immutability with encoded-byte
  ownership.
- Fold metadata, filename templates, or batch policy into this helper: encoded
  publication deliberately treats bytes and destinations as already resolved.
