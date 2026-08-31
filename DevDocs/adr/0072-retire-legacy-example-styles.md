# ADR-0072: Legacy example styles are rejected evidence, not product resources

- Status: Accepted
- Date: 2026-08-29
- Extends: [ADR-0065](0065-versioned-recipe-style-artifact.md)

## Context

The frozen installation bundles 24 `.dtstyle` examples containing dynamic IOP
blobs, custom blend state, masks, multi-instances, and many operations that do
not have accepted Ravo schemas. ADR-0065 deliberately rejects the format as a
whole and tests that boundary with a bounded synthetic artifact. Keeping the
examples installed or copying selected ones into Ravo would imply partial
compatibility that the product does not provide.

## Decision

- The legacy examples remain conceptually covered by the explicit
  `unsupported_legacy_dtstyle` contract; they are not migrated, converted, or
  used as runtime resources.
- Remove all 24 bundled `.dtstyle` files, their data CMake owner, the host-data
  subdirectory registration, and the exclusive translation-string generator
  script/target.
- Retain shared old `common/styles*`, preset/history/undo, job, view, and UI
  consumers until their S10/J2/U* zero-consumer gates. Ravo `.rstyle.json`
  remains the only supported style artifact.

## Consequences

H3 is complete without adding a compatibility archive or second parser. The
legacy resource tree no longer installs examples whose operations Ravo cannot
reproduce. Existing ADR-0065 tests remain the canonical rejection evidence.

## Rejected alternatives

- Convert only examples whose first operation is known. Dropping the rest of a
  style would make it non-reproducible.
- Keep the resources for future work. Git and the frozen commit already retain
  the evidence; installation resources must describe current product support.
