# ADR-0170: Borrow immutable input for a retained native GPU tail

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0133](0133-engine-gpu-preview-adapter.md),
  [ADR-0134](0134-engine-qrhi-gpu-backend.md)

## Context

Interactive rendering already retains the exact source/prefix on the Engine GPU
adapter. The native path nevertheless clones the entire CPU float buffer before
dispatching an all-GPU tail that never modifies or downloads that CPU copy.

## Decision

The existing Engine render call may borrow its const working input synchronously
on the existing macOS native-display path when the source is retained, the working
profile is linear Rec.709 RGB, there is
no pending primaries conversion or mask overlay, CPU pixels are not requested,
and the existing classifier admits the complete remaining tail as one nonempty
GPU batch. The normal planner gains a complete-tail requirement; encountering any
CPU stage declines this optimization before execution. Hybrid and CPU requests
keep their existing path.

The same pass builder, shader order, adapter, retained-source identity and native
surface owner execute the batch. The borrowed input outlives the synchronous call;
it is not stored in the adapter, returned to desktop or queued asynchronously.
The adapter still owns all device resources. Errors and cancellation propagate
directly; a failed GPU batch never retries on CPU. No algorithm, shader, colour
tolerance, source-cache key, display resolution or thread owner changes.

## Consequences and validation

The eligible native tail avoids one full-image float allocation/copy. It retains
the existing CPU oracle and packed-pixel bounds. Planner tests must reject CPU
and masked tails and preserve cancellation. Existing retained-source replacement,
native/CPU pixel, odd portrait dimensions and immutable handoff tests apply.
Release measurements must show actual warm parameter changes and include complete
image-publication and burst latency separately from coarse render timing.
Unavailable toolchains remain untested; this does not admit another GPU backend.
