# ADR-0169: Exact RGB8 display transform cache

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0144](0144-per-display-icc-presentation.md)
- Work: LC-02 / PERF-02 in [LightCraft adoption](../TODO_LIGHTCRAFT_ADOPTION.md)

## Context

The monitor transform evaluates encoded RGB8 with LittleCMS relative colorimetric,
black point compensation, NOCACHE and NOOPTIMIZE. Pixel evaluation dominates the
warm publication path. Changing evaluator flags or using an interpolated LUT
would change accepted pixels. RGB8 has a finite domain of 16,777,216 inputs.

## Decision

Services owns an optional, caller-owned exact RGB8 transform cache. It enumerates
every input through the unchanged LittleCMS evaluator and stores packed RGB8
results in a 64 MiB table. Integer lookup performs no interpolation or additional
quantization. The uncached service remains the correctness reference.

One Studio Inspect owner retains one table, shared across its photos. Exact source
and destination profile values bind the entry; rendering intent and evaluator
flags are fixed by this implementation. Screen tokens and recipe revisions are
not pixel dependencies. Replacing a table may temporarily retain the previous
64 MiB immutable entry while readers finish; preparation is serialized, and
there is no process-global cache, disk cache or persistent colour authority.

Develop snapshots the display state on the GUI thread and prepares the table on
its existing catalog/preview executor after obtaining owned preview bytes. Table
generation uses at most eight scope-owned participants, checks cancellation in
4096-colour batches, and joins before releasing its transform. It adds no pool,
detached thread, executor or GPU implementation. GUI publication performs only
exact lookup when the current profile matches; other presentation callers keep
the existing explicit CPU evaluator. A cache miss is not a GPU failure fallback.
Large lookups use the existing conversion boundary's at-most-eight scope-owned
row participants; small images stay serial. No lookup worker outlives its call.

Clear/profile change/close invalidates outstanding preparations by generation.
Cancelled, stale or failed preparation never publishes a partial entry. Allocation,
thread-start and ICC errors remain structured. Construction is allocation-free;
lazy metadata and table allocation stay inside the preparation Result boundary.
Existing asset/session/preview
revision checks still govern image publication. Closing the existing executor
cancels and joins work; the cache and reader snapshots use RAII.

## Consequences and validation

Warm conversion trades a bounded 64 MiB table for avoiding per-frame ICC
evaluation. Large lookups still pay bounded thread startup/join, whose cost must
be measured with real pixel distributions. ICC key payloads are additionally
bounded by the existing 8 MiB per-profile limit. Cold preparation is work on the existing worker and
must be measured separately. It does not reduce preview size, omit operations,
change recipes, exports or CPU gold, or relax colour precision.

Validation requires byte-exact agreement across the complete RGB8 input domain,
matrix and LUT ICC profiles, profile replacement, malformed input, cancellation,
clear during preparation and destruction. Studio tests retain monitor changes,
late-result/close safety and immutable identity. Release measurements report cold
preparation separately from 30 real warm parameter changes and frame publication;
they do not claim physical display latency. Platform/toolchain evidence remains
host-specific.

## Rejected alternatives

- Disabling monitor ICC or lowering the default preview resolution.
- Changing LittleCMS flags, approximating curves or interpolating a coarse cube.
- Building a full table synchronously on the GUI thread.
- A global transform cache or a new rendering/thread owner.
