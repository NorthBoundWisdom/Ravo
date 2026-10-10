# ADR-0167: Foreign conversion receipts and conservative resume

- Status: Accepted
- Date: 2026-10-10
- Extends: [ADR-0166](0166-lightroom-data-preservation-and-develop.md)

## Ownership and lifecycle

ConversionService owns a native Lightroom conversion identity derived from the
source SHA-256, selected foreign IDs, normalized volume mappings and preview
policy. Schema 20 stores per-photo/per-collection checkpoints; schema 21 adds
revision-bound business commit proofs in the destination catalog.
Ordinary SQLite backups include both source bytes
and the journal. SQL and short revision-checked transactions stay in adapters;
no executor, renderer or whole-catalog transaction is introduced.

Each mutation first records its phase against the task's confirmed revision.
The SQLite adapter temporarily binds the conversion/record/phase to its own
connection. TEMP triggers guard the prior confirmed revision and stamp the
actual business revision and target ID in the same transaction that changes
the data. Proof failure rolls back that business transaction. Other connections
cannot inherit the binding; their revision changes cause a conflict rather than
being adopted by the next receipt. There is no whole-run writer lock.
The synchronous binding ends before receipt publication; exceptional callbacks
close the connection to roll back unfinished transactions and prevent reuse.
The stage binding adds no enclosing transaction around file preparation or
recovery publication. The source archive retains its separate bounded chunk
transaction. Receipt updates retain the target asset
ID, mapped fields, omissions and errors. Only a fully converted photo has the
logical `complete` marker. Compatible history/snapshots continue through the
existing Develop/recovery owner; a journal does not replace recovery mirrors.
Collections retain their created `set_id` separately from member completion and
persist pending foreign IDs. Resume adds newly resolved selected members to the
same set through revision-checked, idempotent union. Excluded selection members
are not pending missing originals. A completed set has no pending selected IDs.
Checkpoint failure stops new mutations and remains in the returned receipt.
Post-archive failures never become indistinguishable preflight errors.

## Resume and ambiguity

`convert-foreign --resume` requires the observed source SHA-256, the same request
configuration and a destination revision explained by this task's checkpoints
and commit proofs. It skips completed records
and continues untouched photos and collections. Virtual copies retain their
own foreign and target IDs, independently of the master. The read-only
`foreign-conversion-status` command exposes durable receipts after process exit.
`foreign-conversions` lists their identities without requiring a prior report.
Resume requires an initialized journal. An exit between source archival and
initial journal creation leaves a source-only destination; retry uses a new
destination rather than inferring a conversion task from archive presence.

After an exit between business commit and receipt publication, durable proofs
identify the committed target and keep the task revision current. Untouched
records can proceed. An interrupted or failed photo is deliberately blocked with
`incomplete_conversion_requires_resolution`, its last phase and any known
target ID. Database commit identity does not establish whether every photo stage
or recovery publication finished. It never replays
history, creates another version, rolls back someone else's edits, or declares
that ambiguous work complete. Later untouched records can still be imported.
Destination edits after the checkpoint reject resume rather than overwriting
them. Migration does not invent proofs for unrecorded pre-schema-21 commits;
unexplained older revision drift still rejects resume.
Missing/unavailable preflight sources remain untouched and can be retried
when available. JSON interchange fixtures do not acquire a resume protocol.

Finer automatic photo retry requires stage completion and idempotence contracts in
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
checkpoint/proof rollback, business-commit process exit, second-connection
conflicts, missing-member reconciliation, backup readback, CLI status/resume, audit cancellation,
source disappearance and observed changes with retained target IDs. Large-corpus
and crash-at-every-publication qualification remain separate acceptance gates.
