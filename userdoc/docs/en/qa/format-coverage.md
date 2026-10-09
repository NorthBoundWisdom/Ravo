# Format Coverage Matrix

## Purpose

Use this matrix to choose a realistic smoke-test input and to distinguish a
format boundary from a regression. A file suffix is only an import candidate;
the actual container, pixel layout, profile, and decoder support still decide
the result.

## Input coverage

| Input family | Candidate extensions | Current expectation |
| --- | --- | --- |
| JPEG | `.jpg`, `.jpeg` | Baseline raster import, preview, Develop, and rendered export. RGB layouts and supported ICC state are required. |
| PNG | `.png` | Baseline raster import, preview, Develop, and rendered export. Unsupported color types, interlace/encoding variants, or invalid color metadata fail explicitly. |
| TIFF | `.tif`, `.tiff` | Classic TIFF and structurally valid BigTIFF can import supported single-page contiguous 8/16-bit integer RGB or gray layouts when the Qt TIFF plugin is present. Multi-page, SubIFD, tiled, floating-point, planar, unsupported compression, and unsupported sample layouts fail explicitly. |
| BMP | `.bmp` | Candidate raster input through the Qt image path; validate with a real file on the target kit. |
| GIF | `.gif` | Candidate raster input through the required Qt GIF plugin; validate with a real file on the target kit. |
| WebP | `.webp` | Candidate raster input through the required Qt WebP plugin; validate with a real file on the target kit. |
| HEIC/HEIF | `.heic`, `.heif` | Primary photographs through ImageIO on macOS 14+. The current Windows/Linux providers report unavailable. |
| LibRaw RAW | Common `.arw`, `.cr2`, `.cr3`, `.nef`, `.dng`, `.raf`, `.orf`, `.rw2`, plus other recognized RAW suffixes | Supported only when the actual camera/container is decoded by the pinned LibRaw path. First-frame full decode accepts validated 16-bit RGB Bayer 2×2 and X-Trans 6×6 CFA data. Other sensor layouts remain explicitly unsupported. Embedded JPEG may provide the initial Gallery thumbnail. TIFF-wrapped camera files without a RAW suffix can import as RAW. |
| Video | `.mov`, `.mp4`, `.m4v` | Supported H.264, HEVC and standard ProRes with supported AAC/PCM audio or no audio. Import, posters, metadata, review, Loupe playback and original export; no photo Develop or transcoding. |

Directory import considers raster, RAW and video candidates recursively, ignores hidden
filenames, and returns item-level results. It does not make unsupported formats
supported by copying or renaming them.

## Output coverage

| Output | Current behavior | User-facing options |
| --- | --- | --- |
| PNG | Opaque 8-bit or 16-bit output with resolved supported ICC state. | Studio format selection; CLI `--png-bit-depth` accepts 8 or 16, and `--png-compression` accepts 0–9, defaults 8/5. A 16-bit request from an 8-bit source is structurally unsupported. |
| JPEG | Opaque rendered output with resolved supported ICC state. | Studio and CLI quality 5–100 (default 95), plus `auto`, `444`, `440`, `422` or `420` chroma subsampling. |
| TIFF | Classic little-endian, top-left, contiguous output with baseline directory metadata and supported ICC state. | Studio format selection; CLI sample type, compression, level, and optional grayscale. Default is uint8 / Deflate predictor / level 6 / RGB / 300 DPI. |
| Original copy | Exact source bytes copied to a new destination. | Studio filter or CLI `--format original`, `copy`, or `original-copy`. No Develop rendering. |

Videos support original copy only. HLG/PQ poster and playback frames map to SDR
sRGB; native HDR output, video edit/render export and automatic Live Photo
pairing are not supported. Standard ProRes is admitted; ProRes RAW and
unsupported pixel/audio layouts remain explicit failures. Unsupported auxiliary
audio can coexist with a supported track and is reported as a warning.

TIFF product export accepts typed `uint16`, `float16`, and `float32` from
engine-owned samples. An 8-bit source still returns unsupported for those
requests. TIFF-qualified flags are valid only with TIFF export.

## Profile and metadata boundary

- Input profiles can come from supported embedded/source metadata or explicit
  recipe choices.
- Output and proof profiles are recipe state, separate from monitor
  presentation.
- Supported encoded outputs retain the resolved RGB profile where the format
  contract permits it.
- Rendered JPEG/PNG/TIFF write the bounded Catalog-owned Exif/XMP/IPTC subset,
  including validated capture time/offset/GPS. Capture refresh and
  full/no-location/none privacy are explicit catalog/export operations.
  Arbitrary source-packet copying and adjacent history-sidecar interchange are
  not part of the current export contract. Catalog-owned recovery JSON is a
  separate durability artifact.

## Test evidence guidance

For a release or platform report, record the exact source file, platform,
preset, Qt plugin set, output format/options, and whether the result was
imported, previewed, edited, reopened, and independently decoded. Do not turn a
candidate extension into a blanket format guarantee.
