# ADR-0164: Closed Lightroom Classic catalog import

- Status: Accepted
- Date: 2026-10-08
- Extends: [ADR-0131](0131-foreign-catalog-conversion.md)

## Decision

The explicitly requested Lightroom reader supersedes CONVERT-01's blanket
vendor-reader deferral for this bounded import. Capture One remains deferred.
The existing Qt SQLite adapter reads a private, temporary copy of a closed
Lightroom catalog. It rejects active journals/locks, corrupt databases, missing
required columns, broken photo references and bounded-size violations. The
source, copied snapshot and source reread must have identical SHA-256 hashes.
No Adobe runtime, Rust target or new dependency is introduced.

ConversionService remains the owner. Its caller supplies an empty Ravo catalog,
originals use Add mode, and ordinary import/review/keyword services perform
writes. The reader returns owned domain values and destroys its SQL connection
before deleting the snapshot. Studio runs conversion on its catalog executor,
cancels on window destruction/user request, rejects executor shutdown and checks
destination identity before publishing completion.

The structural reader contract requires Adobe_images, AgLibraryFile,
AgLibraryFolder and AgLibraryRootFolder and the columns selected by the adapter.
It admits at most 2 GB, one million photos/keywords, five million keyword
assignments and 128 keyword ancestors. This is a structural contract, not a
claim of coverage for every Lightroom release. The JSON report uses
`ravo.lightroom-catalog-conversion/v1`; fixture reports retain their old schema.

Original paths, ratings, reject flags, standard English color labels and
hierarchical keywords map to Ravo fields. Foreign volume paths unavailable on
the current host and virtual copies are explicit unsupported items. Missing
originals are skipped. Develop, history, snapshots, collections, custom label
text and IPTC payloads are reported as unconverted when present; they do not
silently become Ravo recipes. No pixel-equivalence claim is made.

Conversion preserves existing partial-completion semantics: completed photos
remain on cancellation or later item failure. A populated destination is a
conflict; resuming into it is not implemented. Retrying requires a new empty
destination. Snapshot acquisition checks cancellation per copied chunk; SQL
quick-check and statement execution are synchronous, with cancellation checked
between rows. No fallback is added.

## Reference and validation

The locally supplied RAWmakase reader informed the table relationships and
closed-snapshot policy; the implementation is native C++ within Ravo's owners.
Validation uses synthetic Lightroom SQLite databases through the actual service,
source hashes, reopen/readback, missing files, broken references, active journals,
cancellation and non-empty destination conflicts. CLI JSON and Studio command/
QML smoke remain acceptance surfaces. Real private catalogs and Windows/Linux
toolchains require separate evidence; synthetic catalogs do not establish vendor
version certification.
