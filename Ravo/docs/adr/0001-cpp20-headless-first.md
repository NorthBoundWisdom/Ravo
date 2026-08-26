# ADR-0001: C++20 headless engine and CLI first

- Status: Partially superseded by ADR-0007
- Date: 2026-07-21

ADR-0007 replaces the rule that catalog/services/desktop must wait for the
complete headless exit. The C++20 engine, supported CLI, CPU reference path,
and prohibition on wrapping the frozen core remain in force.

## Context

Version 0.9 combines GTK, IOP lifecycle, pixelpipe, database, tasks, and
OpenCL types in a single build graph. Existing algorithms and critical
dependencies are primarily C/C++, while the most valuable regression assets
are CLI image goldens rather than fine-grained unit tests sufficient to cover a
rewrite.

Rewriting the architecture, all numerical algorithms, and the language at once
would compound numerical-consistency, FFI, ownership, build, and UI risks. The
desktop UI must not be a prerequisite for validating the new image kernel.

## Decision

- All first-party Ravo implementation uses C++20, CMake, and FreeCM; the first
  deliverable does not add Rust/Cargo.
- The first product is the headless Ravo Engine and supported `ravo` CLI.
- CPU is the reference implementation; old image fixtures are reused through
  the legacy XMP adapter and CLI comparisons.
- The original decision required catalog, services, and Ravo Studio to wait
  until the headless phase was accepted. ADR-0007 supersedes that schedule:
  M1 now permits a minimal vertical slice that continues to reuse the same
  engine/CLI contract.
- Future UI calls the engine/services API directly rather than starting a CLI
  subprocess; the CLI remains a supported batch tool.

## Consequences

- Ownership, threading, error, and data contracts can be rebuilt while
  retaining numerical semantics.
- The first phase has no GUI demonstration, but can deliver a real product
  that is automatically verifiable and scriptable earlier.
- C++ does not automatically eliminate memory or concurrency problems, so the
  implementation must use RAII, explicit owners, sanitizers, and strict
  dependency boundaries.
- Deferring UI framework selection does not block engine progress.

## Rejected alternatives

- **Pure-Rust first version:** attractive for long-term safety, but it would
  simultaneously introduce algorithm reimplementation, third-party FFI, and a
  second build-system risk.
- **Start Rust presentation and the C++ engine together:** freezes the FFI too
  early and adds two toolchains and ownership protocols.
- **Rewrite the UI first:** does not resolve engine coupling or make existing
  headless image regression useful as the primary acceptance entry point.
- **Wrap the old core as a new library:** retains GTK/IOP/global-state leakage
  and is not a clean-slate architecture.
