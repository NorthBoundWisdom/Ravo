# ADR-0028: Harden original-copy publication without retiring the legacy owner

- Status: Accepted
- Date: 2026-08-27

## Context

The frozen `legacy/src/imageio/format/copy.c` owner is a dynamic format plugin.
It resolves the original through global image state, synthesizes an output
extension, calls the shared legacy copy helper, and then writes an XMP sidecar.
Its own source notes that the surrounding export path cannot reliably prevent
overwrites.

Ravo already exposed an `original` export format, but its helper read the whole
source into memory and delegated publication to a generic byte writer with one
fixed temporary name. That did not establish bounded copy memory, exclusive
temporary ownership, a no-clobber race contract, complete source/output error
context, or deterministic disk-full tests.

This decision is the I10 hardening tranche. It defines Ravo's explicit
source-to-destination service contract, but it does not retire the old format
plugin. The old image-I/O dispatcher, disk storage path, command/job wrappers,
and dynamic format ABI remain live under I1, I14, U10, and J2.

## Decision

- Original copy consumes one explicit local source path and one explicit local
  destination path. It copies exact bytes and reports the byte count. It does
  not infer an extension, create XMP, preserve source mode or timestamps, copy
  xattrs, or apply export metadata; the destination receives new-file metadata.
- The service validates a regular source and streams through one 64 KiB buffer.
  It leaves the source read-only and rejects a size or modification-time change
  observed during the call rather than publishing a possibly mixed result.
- Publication uses a uniquely named adjacent temporary file opened with native
  exclusive-create semantics. Only that successfully created temporary is
  owned and eligible for cleanup; a pre-existing fixed sentinel or unrelated
  file is never removed.
- The temporary is fully written and synchronized before publication. Final
  publication uses the platform's atomic no-replace move: `renamex_np` with
  `RENAME_EXCL` on macOS, `renameat2` with `RENAME_NOREPLACE` on Linux, and
  `MoveFileExW` without replace on Windows. There is no overwrite fallback. An
  existing target or a late race winner remains untouched.
- Cancellation wins at entry, read/write loop boundaries, and immediately
  before publication. Every cancellation and copy failure carries stable
  `reason`, `source`, and `output` context. Missing, non-regular, open, read,
  concurrent-source-change, parent, temporary-open, write, finish, conflict,
  and publish failures remain distinct. Write/finish disk exhaustion retains
  the stage reason and adds `disk_full=true`.
- The private service test boundary is a synchronous, non-owning `noexcept`
  checkpoint hook. A nonzero returned `error_code` enters the same stage mapper
  as a real operation failure. It is not a product callback or asynchronous
  lifetime contract.
- CLI aliases `original`, `copy`, and `original-copy` select the same canonical
  format. Success reports canonical `original`; conflict and I/O failures
  serialize the complete service context. CLI has no injectable end-to-end
  cancellation seam in this tranche, so cancellation evidence remains at the
  direct service boundary.

## Consequences

Ravo original-copy export now has bounded memory, exact-byte and source-safety
evidence, deterministic cleanup/failure injection, and atomic no-clobber
publication independent of pixel encoders. I14 continues to own path templates,
batch/storage policy, and related collision decisions. Metadata and sidecar
export remain separate owners.

I10 is not complete. `legacy/src/imageio/format/copy.c`, its registration, and
the shared dispatcher/storage/job consumers remain until I1, I14, U10, and J2
reach their own accepted replacement and zero-consumer gates. This tranche
therefore authorizes neither legacy deletion nor a claim that the dynamic
format ABI has retired.

## Rejected alternatives

- Keep the read-all helper and fixed temporary suffix: copy memory would scale
  with the source, and stale or foreign temporary files could be mistaken for
  owned state.
- Check destination existence and then use a replacing rename: the check and
  rename race would allow a late writer to be overwritten.
- Copy source metadata or generate XMP for compatibility: this tranche freezes
  exact media bytes only; metadata/sidecar and storage policy have different
  owners and acceptance gates.
- Delete the legacy plugin after the Ravo service tests: dynamic dispatcher,
  storage, CLI/job, and format-ABI consumers are still reachable.
