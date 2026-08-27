# Format Coverage Matrix

## Purpose

Use this matrix to choose a realistic smoke-test input and to distinguish a
format boundary from a regression. A file suffix is only an import candidate;
the actual container, pixel layout, profile, and decoder support still decide
the result.

**Last verified:** 2026-08-27 against the current decoder, encoder, and import
candidate contracts.

## Input coverage

| Input family | Candidate extensions | Current expectation |
| --- | --- | --- |
| JPEG | `.jpg`, `.jpeg` | Baseline raster import, preview, Develop, and rendered export. RGB layouts and supported ICC state are required. |
| PNG | `.png` | Baseline raster import, preview, Develop, and rendered export. Unsupported color types, interlace/encoding variants, or invalid color metadata fail explicitly. |
| TIFF | `.tif`, `.tiff` | Baseline single-page raster layouts can be imported when the Qt TIFF plugin is present. BigTIFF, multi-page, tiled, floating-point, planar, unsupported compression, and unsupported sample layouts are outside the current input contract. |
| BMP | `.bmp` | Candidate raster input through the Qt image path; validate with a real file on the target kit. |
| GIF | `.gif` | Candidate raster input through the required Qt GIF plugin; validate with a real file on the target kit. |
| WebP | `.webp` | Candidate raster input through the required Qt WebP plugin; validate with a real file on the target kit. |
| LibRaw RAW | Common `.arw`, `.cr2`, `.cr3`, `.nef`, `.dng`, `.raf`, `.orf`, `.rw2`, plus other recognized RAW suffixes | Supported only when the actual camera/container is decoded by the pinned LibRaw path. Bayer RAW is the current tested path; embedded previews may be used for Gallery thumbnails. |

Directory import considers raster and RAW candidates recursively, ignores hidden
filenames, and returns item-level results. It does not make unsupported formats
supported by copying or renaming them.

## Output coverage

| Output | Current behavior | User-facing options |
| --- | --- | --- |
| PNG | Opaque RGB8 output with resolved supported ICC state. | Studio format selection; CLI uses the typed default compression 5. The current CLI does not expose a PNG bit-depth flag. A 16-bit request is structurally unsupported from the RGB8 source. |
| JPEG | Opaque rendered output with resolved supported ICC state. | Studio format selection; CLI `--quality 5..100`, default 95. |
| TIFF | Classic little-endian, top-left, contiguous output with baseline directory metadata and supported ICC state. | Studio format selection; CLI sample type, compression, level, and optional grayscale. Default is uint8 / Deflate predictor / level 6 / RGB / 300 DPI. |
| Original copy | Exact source bytes copied to a new destination. | Studio filter or CLI `--format original`, `copy`, or `original-copy`. No Develop rendering. |

TIFF accepts typed `uint16`, `float16`, and `float32` requests for validation but
the current RGB8 rendered source returns unsupported for those requests. TIFF
qualified flags are valid only with TIFF export.

## Profile and metadata boundary

- Input profiles can come from supported embedded/source metadata or explicit
  recipe choices.
- Output and proof profiles are recipe state; Studio does not infer a monitor
  profile.
- Supported encoded outputs retain the resolved RGB profile where the format
  contract permits it.
- Complete EXIF/IPTC/XMP packet writing, GPS/timezone policy, and generated
  sidecars are not part of the current general export contract.

## Test evidence guidance

For a release or platform report, record the exact source file, platform,
preset, Qt plugin set, output format/options, and whether the result was
imported, previewed, edited, reopened, and independently decoded. Do not turn a
candidate extension into a blanket format guarantee.
