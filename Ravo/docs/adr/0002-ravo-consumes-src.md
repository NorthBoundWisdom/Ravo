# ADR-0002: Ravo consumes and retires legacy `src`

- Status: Superseded by ADR-0004
- Date: 2026-07-21

ADR-0004 keeps the one-way replacement goal but supersedes this ADR's
incremental old-side adapters and capability-by-capability deletion strategy.

## Context

Parallel rewrites can become two permanent implementations: Ravo keeps growing
while `src` is never removed for compatibility reasons. That expands the
maintenance surface and tempts new code to achieve short-term progress through
old headers, old libraries, or shims.

The desired end state is that Ravo's covered capability grows while the
reachable capability of `src` shrinks, until Ravo replaces the entire former
application.

## Decision

- Migrate by capability/operation, establishing an old CPU baseline and a new
  contract for each item.
- Ravo production code never depends on `src`; validation only reads frozen
  fixtures and never configures, compiles, or runs an independent old process.
- Any necessary transition dependency may only be a stable `src` → Ravo
  facade, and it must enable measurable old-code deletion.
- A capability is migrated only after Ravo acceptance, consumer migration, and
  removal of old source, build wiring, resources, configuration, and
  documentation.
- Delete the former application entry points and remaining `src` after the
  final release transition; Ravo becomes the only supported implementation.

## Consequences

- Parallel executables may exist during migration, but Ravo has neither a
  build-time nor runtime dependency on the old core.
- Short-term implementation can be slower than direct copying; long-term, the
  project will not maintain two reachable algorithms or a permanent
  compatibility layer.
- Deletion is part of every migration unit and requires regression and
  compatibility decisions proportional to the risk.
- Source-line counts can show a trend, but acceptance is measured by reachable
  consumers, tests, and ownership.

## Rejected alternatives

- **Link `libdarktable` permanently from Ravo:** the dependency direction is
  wrong and prevents independent release or old-core removal.
- **Copy all source first and tidy it later:** creates a second legacy system
  and cannot prove individual behavior or ownership boundaries.
- **Delete `src` only once at the very end:** accumulates duplicate
  implementations and makes transition risk uncontrollable.
