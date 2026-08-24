# ADR-0004: Freeze 0.9 and make Ravo the only growth path

- Status: Accepted
- Date: 2026-07-21
- Supersedes: ADR-0002

## Context

ADR-0002 established the one-way goal that Ravo replaces `src`, but still
allowed incremental `src` → Ravo adapters and capability-by-capability deletion
inside the 0.9 application. Further reduction, dependency cleanup, IOP surgery,
and an OpenCL-to-Metal refactor in the coupled 0.9 graph have poor return
relative to implementing the same product outcomes directly in Ravo.

Maintaining two evolving products would also make the legacy executable a
moving oracle. A change made only to unblock a differential test could alter
the very result that Ravo is meant to reproduce or explicitly reject.

## Decision

- DarkTableNext 0.9 is frozen. Its source, GTK application, dynamic IOP graph,
  OpenCL backend, build graph, resources, and data behaviour receive no further
  product, reduction, architecture, or GPU work.
- Ravo is the only production growth path. All unfinished work formerly
  planned for 0.9—product reduction, final IOP decisions, compatibility
  handling, corpus completion, backend-neutral GPU design, Metal acceleration,
  and eventual OpenCL removal—is owned by the corresponding Ravo milestones.
- The two production graphs remain independent. Neither `src` → Ravo nor Ravo
  → `src` adapters are allowed.
- Ravo tests may read frozen fixtures and source as static evidence. They do
  not configure, compile, package, or run the 0.9 executable, CTest graph, or
  image runner. A frozen-fixture limitation is recorded rather than fixed in
  `src` to advance Ravo.
- `src` is not deleted capability by capability during implementation. After
  Ravo satisfies the complete release-switch gate, M7 retires the old
  application, including its OpenCL code, build entries, resources, config,
  documentation, and duplicate tests.

## Consequences

- Ravo work cannot be blocked on making 0.9 easier to build, test, or adapt.
- Checked-in expected outputs and reproducible unchanged legacy binaries become
  more important because the oracle no longer evolves.
- Duplicate implementations exist during development, but only Ravo changes;
  the duplication ends at the product switch rather than through old-side
  adapters.
- Metal and other GPU work starts from the validated Ravo CPU engine. Legacy
  OpenCL is research material, not an adapter or migration layer.

## Rejected alternatives

- **Resume 0.9 cleanup after the Ravo CLI stabilises**: this would reopen the
  coupled legacy graph and split engineering effort after the new engine has
  already established the replacement boundary.
- **Incremental `src` → Ravo adapters**: they require modifying the frozen
  product and create another integration surface that must later be removed.
- **Port legacy OpenCL to Metal before Ravo GPU work**: it pays the backend
  refactor cost in an application scheduled for retirement and preserves the
  wrong public boundaries.
