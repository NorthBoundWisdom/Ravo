# ADR-0167: Foreign conversion receipts and conservative resume

- Status: Accepted
- Date: 2026-10-10
- Extends: [ADR-0166](0166-lightroom-data-preservation-and-develop.md)

## Ownership and lifecycle

ConversionService owns a native Lightroom conversion identity derived from the
source SHA-256, selected foreign IDs, normalized volume mappings and preview
policy. Schema 20 stores its revision and per-photo/per-collection checkpoints
in the destination catalog. Ordinary SQLite backups include both source bytes
and the journal. SQL and short revision-checked transactions stay in adapters;
no executor, renderer or whole-catalog transaction is introduced.

Each mutation first records its phase. Receipt updates retain the target asset
ID, mapped fields, omissions and errors. Only a fully converted photo has the
logical `complete` marker. Compatible history/snapshots continue through the
existing Develop/recovery owner; a journal does not replace recovery mirrors.
Collections have separate checkpoints so resume cannot recreate a completed
set. Checkpoint failure stops new mutations and remains in the returned receipt.
Post-archive failures never become indistinguishable preflight errors.

## Resume and ambiguity

`convert-foreign --resume` requires the observed source SHA-256, the same request
configuration and an unchanged destination revision. It skips completed records
and continues untouched photos and collections. Virtual copies retain their
own foreign and target IDs, independently of the master. The read-only
`foreign-conversion-status` command exposes durable receipts after process exit.
`foreign-conversions` lists their identities without requiring a prior report.

An interrupted or failed photo is deliberately blocked with
`incomplete_conversion_requires_resolution`, its last phase and any known
target ID. The service cannot infer whether a mutation committed immediately
before a crash or whether recovery publication finished. It never replays
history, creates another version, rolls back someone else's edits, or declares
that ambiguous work complete. Later untouched records can still be imported.
Destination edits after the checkpoint reject resume rather than overwriting
them. Missing/unavailable preflight sources remain untouched and can be retried
when available. JSON interchange fixtures do not acquire a resume protocol.

Finer automatic retry requires atomic mutation-plus-checkpoint primitives in
the respective existing owners, including expected recipe/history identity
and post-commit recovery state. Each such stage must be qualified independently;
the journal is not evidence of per-photo atomicity across current service calls.

## Cancellation and source audit

Preflight hashes canonical original paths once, including shared virtual-copy
sources, before destination writes. Fingerprinting checks identity before and
after hashing. Conversion cancellation prevents new mutations while bounded
receipt publication preserves already committed facts. Final source hashing
uses the same cancellation token; each recorded source reports `verified`,
`changed`, `failed` or `cancelled`, with before/after identities where available.
`source_audit_complete` distinguishes completion from interruption;
`originals_unchanged` is true only when every recorded source was reverified.
Observed changes use `source_changed_during_conversion`; they do not attribute
the change to a particular process. Audit errors retain the conversion report.

## Validation

Contracts cover completed/untouched resume after reopen, independent copies,
no duplicate history or sets, blocked partial work, destination revision conflict,
checkpoint rollback, backup readback, CLI status/resume, audit cancellation,
source disappearance and observed changes with retained target IDs. Large-corpus
and crash-at-every-publication qualification remain separate acceptance gates.
