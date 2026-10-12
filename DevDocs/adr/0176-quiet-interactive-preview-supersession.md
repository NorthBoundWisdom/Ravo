# ADR-0176: Quiet interactive preview supersession

- Status: Accepted
- Date: 2026-10-11
- Extends: [ADR-0175](0175-exact-raw-preparation-work.md)

## Context and decision

Studio coalesces pending interactive intents while allowing an active frame to
finish. Slow RAW preparation therefore makes the final intent wait for both the
old preparation and its own preparation after a finite slider burst.

The desktop C++ Develop presenter owns one single-shot GUI timer. When an
interactive pixel job has a newer pending interactive intent, eight milliseconds
without another changed intent may supersede that job through its existing
PreviewRequestOwner. The latest pending intent starts only after the existing
serial executor returns the cancelled job. Continuous input still allows active
frames to finish. There is at most one quiet supersession between accepted
publications, so input arriving just beyond the quiet interval cannot repeatedly
cancel every frame and starve publication.

The timer does not own tasks, pixels, services or persistence. It stops when a
job starts, an explicit preview/save replaces the request, selection/catalog
identity changes, or the presenter shuts down. Its callback checks active and
pending interactive work, selected asset identity and absence of a pending save.
Existing request/session generations reject late completion, including a worker
that finishes concurrently with supersession. Save, history, recovery,
comparison and settled previews retain their existing scheduling contracts.

No algorithm, quality, resolution, thread pool, schema, dependency or fallback
changes. Failure retains the accepted image and existing structured error path;
cancellation remains cooperative at the Engine/service checkpoints.

## Validation

Controlled presenter tests expire the timer without relying on machine speed.
They verify cancellation/revision binding, latest-intent coalescing, one-shot
anti-starvation, save/comparison protection and context/close invalidation.
Keep existing late-result, persistence and destruction contracts. Run the full
Ravo suite and Release source-size checks, then compare repeated numeric bursts
through final owned-image publication with identical source, intent stream,
dimensions, backend and monitor ICC. Source, stored recipe and history remain
unchanged. Windows/Linux require separate build evidence.
