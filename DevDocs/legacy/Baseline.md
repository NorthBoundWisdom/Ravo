# Frozen legacy baseline

## Authority

The frozen darktable 0.9 reference was fixed on 2026-07-21 before Ravo product
targets existed.

| Item | Identity |
| --- | --- |
| Freeze commit | `320970bf7c9cbbc6611cfc3eb60f8f2b0424b782` |
| Frozen `src` tree | `a3ac761ecbb0cf668ecad49aff8bd0e29235f5f7` |
| Frozen `legacy/tests` tree | `1dc38893f39e113620aebbbdc927218ca4a2b8af` |
| Fixture set | 158 XMP files, 158 expected PNGs, 5 source images |
| Legacy XMP schema | 6 |

The generated
[`legacy_manifest.json`](../../Ravo/tests/fixtures/legacy_manifest.json) is the
complete hash and operation inventory. The freeze checker also protects the
legacy CMake, data, and packaging trees while allowing only explicitly accepted
owner retirement recorded by the two lists in this directory.

## Oracle limitation

No reproducible legacy image run is accepted as a live oracle. A historical
Windows investigation reached only the first fixture and was blocked by module,
LensFun-data, and blend-state problems; it produced no accepted output. Those
experiments are provenance, not commands or prerequisites. The old application,
CLI, tests, image runner, and packaging graph must not be configured, built, or
executed again.

Ravo may read the frozen source and fixtures statically. Product acceptance
comes only from Ravo-owned recipes, tests, image results, metadata, resource
limits, and structured failures.

## Current validation

Run only the read-only Ravo checks:

```text
python3 Ravo/tools/freeze_legacy_manifest.py --check
python3 Ravo/tools/check_capability_inventory.py
python3 Ravo/tools/check_freeze_reference.py
```

These checks establish source and inventory identity; they do not claim pixel
equivalence or support for every operation named by a fixture.
