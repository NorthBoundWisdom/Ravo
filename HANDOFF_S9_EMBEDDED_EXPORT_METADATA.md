# Development prompt: embed catalog-owned export metadata

This is the execution prompt for the next development thread. Implement the
whole bounded tranche, validate it, and leave the working tree ready for review.
Do not commit, amend, rebase, or push unless the user explicitly asks.

## Goal

Implement one coherent S9/J6 plus I11/I12/I13 export-metadata tranche across
domain values, CatalogService, the raster port, the private JPEG/PNG/TIFF
encoders, tests, an ADR, and the stable owning documents.

Rendered JPEG, PNG, and TIFF exports must build metadata only from one immutable
Catalog-owned snapshot taken after asset lookup, embed all requested packets
into the in-memory encoded result, and only then enter the existing ADR-0032
atomic no-replace publication path. There must be no metadata rewrite after
publication.

The accepted container result for this tranche is:

| Rendered format | Required metadata containers |
| --- | --- |
| JPEG | Exif APP1, standard XMP APP1, and IPTC-IIM in one Photoshop APP13 resource |
| PNG | one `eXIf` chunk and one uncompressed `iTXt` XMP chunk with keyword `XML:com.adobe.xmp`; no IPTC-IIM chunk and no `pHYs` |
| TIFF | main-IFD baseline fields retained, an `EXIFIFD` custom directory, XMP tag 700, and IPTC tag 33723 |
| Original copy | exact source bytes only; no generated packet and no render |

The metadata policy is deliberately fixed for this first product contract:

- Rendered exports always include the bounded Catalog-owned public metadata
  described below. Do not add a CLI flag, Studio preference, preset, or hidden
  compatibility switch in this tranche.
- Never copy arbitrary Exif/IPTC/XMP packets from the original. Never read,
  merge, create, replace, or delete an XMP sidecar during export.
- Do not embed recipe JSON, edit history, snapshots, rating/reject/color-label
  state, private tags, a source path, a catalog path, or a random identifier.
- `captured_unix_s` is not exported. Its current LibRaw origin does not preserve
  the source timezone, so converting it into `DateTimeOriginal` would fabricate
  semantics. The Catalog has no accepted GPS value either; emit no GPS IFD or
  location XMP. Record both omissions as explicit remaining S9/J6 work.
- Output metadata must be reproducible. It contains no current time, filesystem
  time, locale-dependent number, host path, random padding, or unordered
  iteration result. Two identical requests against the same snapshot must
  produce byte-identical output.
- ICC/cICP remain owned by the existing color-profile contract. Metadata
  packets describe the output color space but do not duplicate or replace the
  exact embedded ICC bytes.

Acceptance requires all of the following:

- A successful Catalog or CLI JPEG/PNG/TIFF export independently parses to the
  exact field matrix below, while existing pixels, precision, compression,
  subsampling, ICC/cICP, TIFF resolution/grayscale, and atomic publication
  behavior remain unchanged.
- Metadata is serialized and embedded before the encoded byte vector is handed
  to publication. Any validation, serialization, marker/chunk/tag, directory,
  cancellation, allocation, codec-finalization, or publication failure returns
  a structured error and publishes no destination.
- Source pixels, source files, source metadata, Catalog values, ICC bytes,
  recipes, and an existing adjacent sidecar retain their hashes, sizes, modes,
  and modification times. No missing sidecar is created.
- Packet and container limits are checked before narrowing or allocation.
  Unsupported oversized JPEG APP1/APP13 state fails closed; do not truncate
  text, drop tags, split into nonstandard packets, or silently omit metadata.
- Existing JPEG/PNG/TIFF input, original-copy export, preview/cache, Studio,
  and high-precision export contracts remain unchanged.
- No fallback, second export pipeline, new dependency, generated-source patch,
  or change under `legacy/` or `build/dependency_*` is added.

The durable authorities remain
[`TODO_LEGACY_MIGRATION.md`](TODO_LEGACY_MIGRATION.md),
[`Ravo/README.md`](Ravo/README.md),
[`Ravo/ARCHITECTURE.md`](Ravo/ARCHITECTURE.md),
[`Ravo/MIGRATION.md`](Ravo/MIGRATION.md),
[`Ravo/TESTING.md`](Ravo/TESTING.md), and
[`DevDocs/ProductRoadmap.md`](DevDocs/ProductRoadmap.md). This handoff
supplements them; it does not override them.

## Product/architecture boundary

### Owned snapshot and lifecycle

- Extend the domain-owned `ExportMetadataSnapshot` with owned capture values
  and tags, or introduce an equivalently explicit owned value if that produces
  a clearer public contract. No Qt, Exiv2, libjpeg, libpng, or LibTIFF type may
  cross the domain/raster port.
- CatalogService constructs the snapshot once, immediately after successful
  asset lookup and output-path normalization, for every rendered JPEG/PNG/TIFF
  export. It copies the current writable metadata, capture metadata, and tags.
  The normalized destination remains available only for the accepted TIFF
  `DocumentName` baseline. Original copy receives no generated snapshot.
- Sort tags by UTF-8 byte order in the owned snapshot and require unique,
  normalized values. Validate all strings, numbers, counts, and aggregate
  packet bounds before rendering. Do not re-query the repository, original, or
  filesystem from an encoder.
- The raster call remains synchronous. Snapshot values and pixel spans are
  borrowed only for that call; prepared packet bytes and the final encoded
  vector are separately owned. There are no new threads or detached tasks.
- Add cancellation checks at snapshot validation, between packet families and
  bounded fields, before each format embeds metadata, during existing row
  loops, before codec finalization, and before publication. Preserve the one
  request token through all layers.

### Exact field policy

Use one private, validated semantic representation so the Exif, XMP, and IPTC
serializers cannot apply different optional/numeric rules. Container adapters
may add the framing required by their format, but they must not remap values.

| Ravo/output value | Exif/TIFF mapping | XMP mapping | IPTC-IIM mapping |
| --- | --- | --- | --- |
| output width/height | `PixelXDimension` / `PixelYDimension` | `exif:PixelXDimension` / `exif:PixelYDimension` | none |
| physically oriented output | orientation 1 | `tiff:Orientation=1` | none |
| sRGB vs other exact ICC | Exif `ColorSpace=1` for the accepted built-in sRGB state, otherwise `0xffff` | matching `exif:ColorSpace`; ICC bytes remain authoritative | none |
| camera make/model | IFD0/TIFF `Make` / `Model` | `tiff:Make` / `tiff:Model` | none |
| ISO | `PhotographicSensitivity` / tag 34855 | `exif:ISOSpeedRatings` | none |
| aperture | `FNumber` | `exif:FNumber` | none |
| focal length in mm | `FocalLength` | `exif:FocalLength` | none |
| shutter seconds | `ExposureTime` | `exif:ExposureTime` | none |
| title | no Exif field; keep TIFF `DocumentName` as the destination | `dc:title` language alternative, `x-default` | record 2 dataset 5 `ObjectName` |
| description | `ImageDescription`; retain the accepted TIFF baseline behavior | `dc:description` language alternative, `x-default` | 2:120 `Caption/Abstract` |
| creator | `Artist` | ordered `dc:creator` sequence with one item | 2:80 `By-line` |
| copyright | `Copyright` | `dc:rights` language alternative, `x-default` | 2:116 `CopyrightNotice` |
| sorted tags | no Exif field | `dc:subject` bag in canonical sorted order | one 2:25 `Keywords` dataset per tag in the same order |

Also include one deterministic `xmp:CreatorTool` value identifying Ravo, but
do not include a build path, timestamp, Git SHA, host name, or locale. Do not
add fields not present in this table merely because the frozen owner or a
third-party library can expose them.

Optional values retain their current three-state contract where a target field
supports text: absent omits the field, present-empty emits an empty field, and
present-nonempty emits the exact UTF-8 value. XMP arrays/alternatives must be
structurally valid for an empty value. The IPTC packet is omitted entirely only
when all four writable optionals are absent and the tag vector is empty. A
present-empty writable value therefore emits a packet containing its empty
dataset; do not collapse it to absent or emit an accidental empty Photoshop
resource. Freeze that rule in the ADR and tests.

For capture numbers:

- reject non-finite, zero, or negative ISO/aperture/focal-length/shutter values;
- require ISO to be exactly representable by the chosen Exif integer field and
  reject out-of-range/fractional values rather than silently clamping;
- encode the three positive rational fields using one documented deterministic
  unsigned-rational approximation with checked 32-bit numerator/denominator,
  shared by Exif and XMP lexical output;
- use locale-independent canonical decimal or rational strings in XMP and test
  the chosen representation exactly.

### Private packet ownership

- Add one adapter-private export-metadata builder/serializer shared by JPEG,
  PNG, and TIFF. It should consume only domain values plus output dimensions and
  resolved profile state, and return owned prepared values/packet bytes. Do not
  duplicate XML escaping, rational conversion, optional-state handling, or
  bounds in three encoders.
- Prefer small owned serializers over enabling another metadata stack. The
  current Exiv2 build has XMP disabled and is linked only by the engine for RAW
  metadata. Do not enable Exiv2 XMP, move Exiv2 into a public contract, or link
  it into `ravo_adapters` merely to reopen a completed encoded file.
- The Exif payload for JPEG/PNG is a bounded TIFF profile with checked offsets,
  counts, types, and little-endian integer/rational writes. JPEG adds the
  `Exif\0\0` APP1 identifier; PNG `eXIf` contains only the TIFF profile, without
  the JPEG marker, length, or identifier.
- XMP is canonical UTF-8 RDF/XML with fixed namespace and property order,
  correct XML escaping, no BOM/current time/random padding, and a standard
  packet wrapper if one is used. A single bounded serializer supplies all
  formats.
- IPTC uses one valid UTF-8 IIM dataset stream, including the coded-character-
  set marker needed for UTF-8. JPEG wraps it in one well-formed Photoshop APP13
  image-resource block; TIFF tag 33723 receives the same IIM payload according
  to LibTIFF's public field contract. PNG does not receive an invented IPTC
  chunk.
- Establish explicit per-packet and aggregate bounds in the domain/private
  contract. They must fit JPEG marker lengths including identifiers and framing.
  Extended XMP, multiple Photoshop metadata resources, and silent truncation
  are out of scope; oversized values return a stable structured reason.

The relevant standards/API evidence includes the
[PNG Third Edition](https://www.w3.org/TR/png-3/) `eXIf` and XMP `iTXt`
requirements, the
[IPTC Photo Metadata Standard](https://iptc.org/standards/photo-metadata/iptc-standard/),
the [Adobe XMP specifications](https://developer.adobe.com/xmp/docs/xmp-specifications/),
and the
[LibTIFF custom-directory contract](https://libtiff.gitlab.io/libtiff/functions/TIFFCustomDirectory.html).
Use the pinned materialized LibTIFF headers and tests only as read-only API
evidence; never edit or stage them.

### Container integration

- JPEG writes the Exif APP1, standard XMP APP1, existing ICC APP2 sequence, and
  optional IPTC Photoshop APP13 in one deterministic marker order after
  `jpeg_start_compress` and before the first scanline. Check every marker's
  complete framed length before calling libjpeg. Existing quality,
  subsampling, density, error-manager, cancellation, and destination-manager
  behavior remains one owner.
- PNG uses the private libpng owner to register exactly one `eXIf` and one
  uncompressed `iTXt` XMP entry before pixel data, preserving existing
  iCCP/cICP, RGB8/RGB16, filters, compression, endian, cancellation, and error
  handling. Do not write legacy raw-profile text, duplicate plain Title/Author
  chunks, or invent `pHYs`.
- TIFF retains its classic little-endian main IFD, pixels, strips,
  compression/predictor, sample type, ICC, resolution, grayscale decision, and
  baseline destination/writable tags. Use public LibTIFF
  `TIFFCreateEXIFDirectory`/`TIFFWriteCustomDirectory` plus the main-IFD
  `EXIFIFD` link; set XMP 700 and IPTC 33723 through public fields. Follow the
  pinned `test/custom_dir.c`, `test/custom_dir_EXIF_231.c`, and
  `test_write_read_tags.c` lifecycle as API evidence. Preserve the existing
  client-I/O error capture and make directory save/reload/rewrite/finalize
  failures structured. This all happens inside the one in-memory encoder;
  there is no filesystem reopen and no second publication phase.
- Do not modify JPEG/PNG/TIFF decoders to compensate for malformed output.
  Existing input parsing may be reused by tests only where it is genuinely
  independent of the writer under test.

### Scope and durable truth

- Add an accepted ADR (next number after 0037) that freezes the allowed field
  matrix, privacy/sidecar/history decision, ownership, bounds, deterministic
  encoding, error/cancellation path, and rejected alternatives. Update the ADR
  index.
- Move the now-decided embedded-metadata portion out of
  `DevDocs/ProductRoadmap.md`; leave batch persistence/presets and any still-
  undecided export workflow there. Update `Ravo/README.md`,
  `Ravo/ARCHITECTURE.md`, `Ravo/MIGRATION.md`, `Ravo/TESTING.md`, the root TODO,
  and the phase-0 capability inventory without copying an implementation diary.
- Do not claim S9 or J6 complete. Capture timezone/GPS schema, general sidecar
  read/write/conflict/rollback, shared old consumers, and deletion gates remain.
  Do not delete legacy JPEG/PNG/TIFF/common metadata/sidecar owners in this
  tranche.
- TIFF multipage masks, explicit Studio PNG/TIFF options, JPEG subsampling CLI,
  path templates, batch jobs/presets, and old storage/imageio retirement remain
  separate work.

## Current state

- Repository: the current checkout root (the directory containing this prompt)
- Branch: `main`
- HEAD: `00d5696 [feat]: complete high-precision product export`
- Earlier local commits: `ad83741 [feat]: extend typed PNG export support` and
  `07ac880 [chore]: remove retired legacy library registrations`.
- `origin/main` is `e6fd11b`; the parent branch is three commits ahead and has
  not been pushed.
- At handoff creation, the prior implementation/docs were clean and this prompt
  was the only intended untracked path. Preserve unrelated user changes if the
  state has moved.
- Active source-root mode is `pinned`; `show`, `resolve`, and `verify` pass.
  GeoControls and LibRaw seeds are clean on their tracked default branches;
  FreeCM is a clean detached checkout. No source-root, seed, gitlink, template,
  or dependency update belongs to this task.
- `source_roots.lock.jsonc`, `CMakePresets.json`, `.freecm/`,
  `build/dependency_*`, build trees, and `userdoc/site/` are ignored local or
  generated state. Never stage them or patch a materialized dependency.

## Completed

- Domain already owns bounded `WritableMetadata`, read-only `CaptureMetadata`,
  sorted Catalog tags, typed JPEG/PNG/TIFF options, and
  `ExportMetadataSnapshot` with TIFF destination plus writable values.
- CatalogService performs one asset lookup and currently creates the metadata
  snapshot only for TIFF. It renders one explicitly tagged RGB8/RGB16/float
  product value, calls one synchronous raster encoder, then passes the complete
  byte vector to ADR-0032 atomic no-replace publication.
- JPEG output already owns libjpeg-turbo quality/subsampling, fixed density,
  ICC APP2, bounds, cancellation, and memory destination failures. It currently
  writes no Catalog Exif/IPTC/XMP.
- PNG output already owns libpng RGB8/RGB16, typed compression, exact ICC,
  recognized built-in cICP, bounds, cancellation, and memory failures. The
  input adapter can parse `eXIf` orientation, but the output currently writes no
  Exif/XMP and deliberately writes no `pHYs`.
- TIFF output already owns private LibTIFF RGB8/RGB16/float16/float32,
  none/Deflate/predictor, conditional grayscale, exact ICC, 72-9600 inch
  resolution, and bounded main-IFD `DocumentName`, `ImageDescription`, `Artist`,
  and `Copyright`. Title is deliberately unmapped there. Tests currently assert
  EXIFIFD 34665, IPTC 33723, and XMP 700 are absent; those assertions must become
  positive packet contracts while retaining all prior baseline tags.
- `legacy/src/imageio/imageio.c` statically shows the frozen two-stage flow:
  build an Exif blob, let each format write it, then reopen output to attach
  source/sidecar/DB XMP. Ravo must translate only the evidenced field/container
  behavior that matches this prompt's policy; it must not copy the reopen,
  swallowed-error, global DB, arbitrary source metadata, or sidecar merge.

## Validation

Immediately before this handoff, the following passed on macOS/Apple Silicon
for commit `00d5696`:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure --parallel 4
python3 -m mkdocs build -f userdoc/mkdocs.yml --strict
git diff --check
```

The full Ravo suite passed 414/414. The strict documentation build passed with
only the external Material for MkDocs warning about the future MkDocs 2.0
project. All changed C++ files passed clang-format dry-run. Windows and Linux
were not run and must not be reported as passing. No fallback was added.

For this cross-target/public-value tranche, minimum final validation is:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
ctest --test-dir build/mac_clang_debug --output-on-failure --parallel 4
python3 -m mkdocs build -f userdoc/mkdocs.yml --strict
git diff --check
git diff --stat
```

Run clang-format dry-run on every changed C/C++ source/header. If a narrower
test loop is useful during development, build and run at least the unit,
contract, catalog, PNG export/CLI, TIFF export/metadata/CLI/adapter, and desktop
smoke targets affected by the public raster-port change; the final gate remains
the full Ravo build and full CTest suite. Build every additional host toolchain
actually available, otherwise report Windows/Linux as untested.

Tests must include at least:

- focused domain/private-serializer tests for every field, exact absent versus
  present-empty behavior, sorted Unicode tags, XML escaping, UTF-8 IPTC marker,
  rational conversion, sRGB/other-profile color-space values, exact dimensions
  and orientation, deterministic repeated output, checked offsets/lengths, and
  every aggregate bound;
- an independent JPEG marker/parser test proving one correct Exif APP1, XMP
  APP1, ICC APP2, and optional Photoshop APP13 resource plus unchanged
  quantization/subsampling and successful decode;
- independent PNG chunk/CRC tests proving one `eXIf` profile with no `Exif\0\0`
  prefix, one correctly formed uncompressed `iTXt` XMP entry, fixed pre-IDAT
  ordering, exact ICC/cICP and RGB8/RGB16 pixels, and continued absence of
  `pHYs` and nonstandard raw-profile chunks;
- an independent TIFF parser that follows EXIFIFD, reads exact EXIF tags,
  reconstructs IPTC 33723 and XMP 700, and rechecks main-IFD baseline,
  ICC/resolution, RGB/grayscale, all four sample types, compression, and pixels;
- real Catalog and CLI exports after setting writable metadata/tags, covering
  JPEG/PNG/TIFF propagation, unrelated-format isolation, one asset snapshot,
  source and sidecar immutability, no sidecar creation, existing/racing output
  conflicts, and byte-identical repeated exports to distinct destinations
  after accounting for the TIFF `DocumentName` destination field;
- invalid UTF-8/control/NUL, duplicate or oversized tags, non-finite/invalid
  capture values, rational overflow, each packet/marker limit, cancellation at
  metadata stages, allocation and injected codec tag/chunk/marker/directory
  failures, resource destruction, zero publication, and complete structured
  error context;
- original copy still produces exact source bytes and no generated metadata;
  preview/cache/input decoders and source metadata remain unchanged.

## Open issues/blockers

There is no external dependency blocker for this bounded tranche. Resolve the
following implementation choices in the required pre-edit plan and freeze the
result in the ADR/tests rather than leaving comments as the only contract:

1. Choose the smallest private prepared-metadata representation that shares
   validation, numeric normalization, optional state, packet bounds, and XMP/
   IPTC bytes while still letting TIFF apply semantic Exif values through its
   custom-directory API.
2. Define exact packet limits and stable structured reasons, including complete
   JPEG framing overhead. The common policy must not accidentally allow a PNG/
   TIFF packet that the same Catalog values cannot encode as JPEG without an
   explicit format-specific error.
3. Define the deterministic positive-rational approximation and canonical XMP
   lexical form. It must be simple, checked, locale independent, and covered by
   boundary/tie tests.
4. Integrate LibTIFF EXIF custom-directory creation with the existing memory
   client and failure injection without losing the main directory, writing a
   second image IFD, bypassing finalization errors, or weakening classic-TIFF
   bounds.
5. Preserve the frozen present-empty IPTC rule across JPEG/TIFF framing and
   verify it independently. Do not let a codec helper silently reinterpret
   present-empty as absent in only one container.

The following are known product blockers outside this tranche, not choices for
the implementation thread:

- `captured_unix_s` lacks a source timezone contract; do not emit
  `DateTimeOriginal`, `OffsetTimeOriginal`, XMP creation time, or PNG `tIME`.
- GPS is absent from the accepted Catalog schema; do not inspect the original
  again or add GPS fields/schema migration here.
- General XMP sidecar read/write, explicit user selection, conflict/rollback,
  and history/recipe interchange remain J6/S9 work. This tranche's export rule
  is strictly no sidecar read or mutation and no embedded history.

If implementation reveals that the bounded packet contract itself cannot be
completed without changing one of those product decisions, stop and update
this prompt with evidence rather than inventing a fallback or silently reducing
the field matrix.

## Next action

First run `git branch --show-current`, `git status --short --branch`, and
`git log -5 --oneline`. Read the root and `Ravo/` `AGENTS.md` files plus all
linked authorities and ADR-0032/0036/0037. Statically inspect only the relevant
frozen owners:

```text
legacy/src/imageio/imageio.c
legacy/src/imageio/format/jpeg.c
legacy/src/imageio/format/png.c
legacy/src/imageio/format/tiff.c
legacy/src/common/exif.cc
legacy/src/common/metadata_export.c
legacy/src/common/metadata_export.h
```

Before editing, post a short cross-layer plan naming value ownership and
lifetime, the fixed field/privacy policy, serializer/container owners,
LibTIFF directory lifecycle, cancellation/error/resource paths, packet bounds,
and the smallest validation set.

Then begin with domain and private serializer tests: extend/factor
`ExportMetadataSnapshot`, add generic validation, and implement one private
prepared metadata builder with exact Exif/XMP/IPTC byte tests. Make those tests
pass before wiring any codec. This establishes the shared contract and prevents
three format adapters from drifting.

## Remaining sequence

1. Add the ADR and domain contract. Extend the immutable snapshot for current
   capture values and sorted tags, split generic metadata validation from the
   TIFF-only destination baseline, define bounds/reasons, and update every
   `RasterDecoder` implementation/test double explicitly. Do not add a default
   virtual fallback that hides missing metadata support.
2. Add the adapter-private prepared metadata value and serializers. Test exact
   endian/offset/count handling, XMP namespaces/order/escaping, IPTC datasets,
   Unicode, empty/absent states, rational normalization, packet limits,
   cancellation, allocation failure, input immutability, and owned output.
3. Wire CatalogService to snapshot/validate public metadata once for every
   rendered format before render, while leaving original copy untouched. Add
   a capturing raster double that proves exact values, tag canonicalization,
   one lookup/snapshot, and no filesystem/source metadata reread.
4. Integrate JPEG markers without changing quality, sampling, density, ICC,
   rows, or destination errors. Independently parse complete marker framing and
   packet contents; add real Catalog/CLI positive and failure tests.
5. Integrate PNG `eXIf` plus canonical XMP `iTXt` through libpng for both RGB8
   and RGB16. Independently parse chunks and rows; retain exact ICC/cICP,
   compression/filter/endian and no-`pHYs` contracts.
6. Integrate TIFF XMP/IPTC main fields and the EXIF custom directory for RGB8,
   RGB16, float16, and float32 under every supported compression/grayscale
   branch. Exercise directory save/reload/link/finalize and client-I/O failures;
   independently follow the EXIFIFD offset and parse all packets/tags.
7. Add cross-format Catalog/CLI determinism, validation, cancellation,
   conflict, immutability, sidecar-absence, and source-hash tests. Keep packet
   failure distinct from publication failure and prove neither leaves a temp or
   destination.
8. Update the stable authorities and root TODO precisely. Record this accepted
   packet/field policy, remove only the corresponding undecided roadmap text,
   and leave timezone/GPS, general sidecars/history, multipage/UI/batch,
   consumers, and retirement visibly unfinished. Run the full validation gate,
   inspect the complete diff, and report untested platforms and any fallback
   (expected: none).

## Do not do

- Do not commit, amend, rebase, or push without an explicit user request.
- Do not modify, configure, build, or execute `legacy/`; it is static evidence
  only. Do not delete any frozen metadata, sidecar, JPEG, PNG, or TIFF owner.
- Do not modify `FreeCM`, dependency seeds, `source_roots.lock.jsonc.in`, the
  active ignored lock, generated presets, or `build/dependency_*` for this task.
- Do not reopen or mutate a published output with Exiv2 or another library.
- Do not copy arbitrary original or sidecar metadata, preserve maker notes,
  emit GPS/timezone/history, or add an “ignore metadata errors” path.
- Do not add an Exiv2/XMP dependency, Qt metadata serializer, public third-party
  handle, compatibility switch, alternate output implementation, or silent
  format fallback.
- Do not invent PNG `pHYs`, PNG IPTC/raw-profile chunks, JPEG extended XMP,
  TIFF multipage masks, new UI/CLI options, batch presets, or sidecar writeback.
- Do not weaken current pixel/profile/precision/compression/cancellation/
  resource/publication tests to land metadata support.
