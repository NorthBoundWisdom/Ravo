# ADR-0101: Verified backup scheduling and stable folder relink

- Status: Accepted
- Date: 2026-08-31

## Context

Verified backup creation was manual, and folder presentation depended only on
mutable paths. Recurring retention must never delete an unknown directory, and
a moved source folder must not be guessed from its display name or repaired by
rewriting originals.

## Decision

Schema v8 stores one catalog-owned backup policy: enabled state, destination,
interval, retention count, last verified success, next run, verified bytes,
and last failure. CatalogService runs the policy on its owned normal-priority
executor. Every scheduled output uses the strict
`ravo-<catalog-id>-<canonical-timestamp>.ravobackup` name and the ordinary
verified backup path. Retention enumerates at most 10,000 entries, considers
only strict names that reverify as the current catalog, orders them by manifest
creation time, and keeps the configured newest count. An expired backup is
atomically moved to a unique quarantine, reverified there, and only then
removed. Unknown, malformed, changed, symlink, active, or user-created paths
are retained. Failures after publication report the published artifact and
persist visible failure state when the catalog remains writable.

Schema v9 adds `catalog_folder(id, uri, created_unix_ms)` and binds each asset
to the stable identity of its direct containing folder. Display hierarchy may
include synthetic ancestors, but only rows with a stable ID are relinkable.
Missing state is an explicit live filesystem observation, not a guessed path
or a second persisted authority.

Relink requires a stable folder ID and an existing replacement directory, and
is accepted only while the old directory is missing. CatalogService maps each
asset by its existing basename, requires an exact current file identity
(stored size, modification time, and content fingerprint), and rejects URI or
folder conflicts before mutation. The SQLite adapter then rechecks the old
folder URI and exact asset set and updates the folder path, all asset URIs,
recovery generations, and catalog revision in one cancellable transaction.
Rollback leaves the former catalog state intact. Originals are read-only.

CLI exposes `catalog backup-policy`, `backup-run`, `folders`, and
`folder-relink`. Studio exposes the same scheduling status/actions and marks a
missing stable folder as a command-owned locate/relink action; QML owns no
retention, identity, or transaction policy.

## Consequences

- Backup schedule state travels with the catalog; the most recent backup may
  contain the policy state observed before that backup completed.
- A verified backup can remain published when later retention or policy-state
  persistence fails; structured context makes that durable fact explicit.
- Relink deliberately rejects a copied tree whose current file identity no
  longer matches. Content-search guessing and fuzzy filename matching are not
  fallbacks.
- Synthetic hierarchy ancestors without direct assets remain presentation
  nodes. A moved tree with several direct containing folders is relinked by
  each stable containing-folder identity.
