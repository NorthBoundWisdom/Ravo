# Legacy IOP census

This file is a machine-checked inventory of the IOPs still registered in the
frozen legacy tree. It records only source presence and whether committed XMP
fixtures mention an operation. Current product status and retirement order live
in [MIGRATION.md](../MIGRATION.md) and
[TODO_LEGACY_MIGRATION.md](../TODO_LEGACY_MIGRATION.md); this census must not
duplicate them.

`Ravo/tools/check_capability_inventory.py` compares this table with
`legacy/src/iop/CMakeLists.txt` and
`Ravo/tests/fixtures/legacy_manifest.json`. A fixture value of `yes` is static
evidence only, not a compatibility or acceptance claim.

## Legacy registry census

| Legacy IOP | Fixture |
| --- | --- |
| `agx` | yes |
| `atrous` | yes |
| `basecurve` | yes |
| `bilat` | yes |
| `bilateral` | yes |
| `bloom` | yes |
| `blurs` | yes |
| `cacorrectrgb` | no |
| `censorize` | yes |
| `colorize` | yes |
| `colormapping` | yes |
| `crop` | yes |
| `demosaic` | yes |
| `diffuse` | yes |
| `filmicrgb` | yes |
| `finalscale` | no |
| `flip` | yes |
| `grain` | yes |
| `highpass` | yes |
| `liquify` | yes |
| `lowlight` | yes |
| `lowpass` | yes |
| `mask_manager` | yes |
| `negadoctor` | yes |
| `nlmeans` | yes |
| `overexposed` | no |
| `overlay` | yes |
| `rasterfile` | no |
| `rawoverexposed` | no |
| `rawprepare` | yes |
| `rgbcurve` | yes |
| `rgblevels` | yes |
| `rotatepixels` | no |
| `scalepixels` | no |
| `shadhi` | yes |
| `soften` | yes |
| `vignette` | yes |
