# Runtime Persistence and Instance Policy

DarkTableNext derives runtime persistence behavior from `AppConfigs.DevMode` in the active
`source_roots.lock.jsonc`. The committed `source_roots.lock.jsonc.in` is the release baseline and
must remain `false`; developers can enable the ignored active lock locally.

## Build input

`configs/generate_build_runtime_config.py` reads the active lock (or the template before an active
lock exists) during CMake configure. It generates `build_runtime_config.h` with the selected mode
and the first 12 hexadecimal characters of SHA-256 over the resolved repository root. After changing
the local lock, run the normal `python3 configs/source_root_workflow.py --update` followed by
`cmake --preset <preset>` before building.

## Mode semantics

| Mode | Default persistence root | Process policy |
| --- | --- | --- |
| `DevMode: false` | GLib user config/cache roots under `darktable` | One process per user. An advisory `flock` is held in `darktable/Locks/application-instance.lock` for the full process lifetime. |
| `DevMode: true` | GLib user config/cache roots under `darktable-dev/<checkout-hash>` | No global singleton lock. Different checkout paths use different databases and D-Bus names; the existing database locks still prevent concurrent writers from the same checkout. |

The instance lock is acquired before opening either SQLite database and is never deleted. Its file is
only a stable rendezvous point: the kernel releases the advisory lock when the owner exits, including
after a crash, so it cannot become a stale database lock. The lock applies to both the GTK app and
`darktable-cli`; `--version` and other argument-only exits do not initialize persistence.

`--configdir` and `--cachedir` deliberately override the selected defaults. They are useful for
tests and controlled experiments, but in development mode passing the same override to two checkout
builds intentionally defeats persistence isolation. `--library` continues to select an alternate
library database; in production it does not bypass the application singleton.
