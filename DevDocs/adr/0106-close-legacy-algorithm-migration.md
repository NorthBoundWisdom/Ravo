# ADR-0106: Close leftover algorithm migration

- Status: Accepted
- Date: 2026-09-01
- Supersedes: [ADR-0015](0015-migrate-all-non-ui-algorithms.md) for every
  leftover image algorithm that is not already Ravo accepted
- Relates to: ADR-0004, ADR-0010, `DevDocs/MIGRATION.md`

## Context

ADR-0015 put every remaining non-UI leftover algorithm in scope as a C++ port
with leftover-faithful CPU math. Ravo already ships a professional catalog and
Develop core. The leftover IOP list is leftover completeness: extra denoisers,
optional display transforms, specialist film/deform/diagnostic modules, and
leftover-exact census of crop, flip, demosaic, and curves that Ravo already
owns. Keeping that queue would delay independent Ravo work and keep a leftover
source tree beside the product.

## Decision

Unaccepted leftover image algorithms are leftovers, not ports. Do not rewrite
them from leftover C, OpenCL, GTK, or dynamic ABI. Defaults stay Sigmoid and
Color Equalizer. Photographer-useful remaining work is independent Ravo product
under `TODO_PHOTO_MANAGEMENT.md`, `TODO_PRO_WORKFLOW.md`, and
`ProductRoadmap.md`.

The leftover source tree was deleted. Freeze/inventory checkers were removed.
Frozen fixtures live in `Ravo/tests/fixtures/frozen`.

## Consequences

- Leftover-faithful Filmic/AgX, NLMeans, à-trous, liquify, film-negative,
  colorize, extra blurs, leftover diagnostic overlays, and leftover-exact grain
  or vignette are not Ravo work items.
- Accepted Ravo operations keep their current contracts. Their leftover C
  files left with the leftover tree; that is leftover retirement, not a missing
  algorithm.
- Historic leftover XMP that names an unaccepted leftover IOP continues to
  fail closed.

## Rejected alternatives

- Keep a leftover-faithful IOP queue until every leftover module has a C++ twin.
- Port GTK, Lua, leftover OpenCL, or the leftover catalog in order to finish
  leftover.
- Archive leftover C as a second live algorithm owner beside Ravo.
