# Ravo

Ravo is the only buildable photo software in this repository. Its current
product goal is to deliver a cross-platform first version quickly: create or
open a local SQLite catalog, import JPEG/PNG/TIFF/RAW by reference, and browse
images in Ravo Studio. The existing C++20 Engine and `ravo` CLI are the
software's foundation and headless client; desktop does not implement a second
set of business logic or algorithms.

Current implementation status:

- Foundation/recipe/engine/adapters/CLI/test scaffolding and versioned
  JSON/error contracts are complete.
- `ravo inspect` reads the first LibRaw-supported 16-bit Bayer or X-Trans RAW
  slice, reports camera-oriented display dimensions, CFA family/size and the
  sensor-default demosaic mode, and exposes owned DNG
  OpcodeList2/3 presence, supported correction counts, and explicitly skipped
  optional opcode IDs.
- `ravo render` executes the canonical bounded RAW/raster recipe, including
  crop, black/white normalization, camera WB, profile-aware camera-to-working
  conversion, bounded DNG GainMap/WarpRectilinear/FixVignetteRadial correction,
  tiled RCD Bayer demosaic by default or explicit PPG, and Markesteijn 3-pass
  X-Trans demosaic by default or explicit 1/3-pass, exposure,
  declared output-profile conversion,
  embedded ICC state, and atomic PNG output.
- Legacy XMP supports empty history, a strict nop baseline with explicit
  `colorin`/`colorout` mapping, and the frozen exposure v5/v6/v7 final-revision
  boundary. Exact default-unmasked singleton state maps to the canonical recipe;
  leftover flip/crop/ashift generic perspective, rgblevels v1, rgbcurve v1
  including middle-grey uncompensate, and Bayer/X-Trans rawdenoise v2 also map
  (ADR-0048–0054).
  Mask, custom blend, multi-instance, and conflicting-revision state rejects
  structurally.
- The catalog vertical slice is implemented: reference-only JPEG/PNG/TIFF/RAW
  import, preview cache outside the library, and the `ravo_studio` Qt Quick
  window using controls from the `GeoControls` source root. Catalog fully
  decodes JPEG/PNG/TIFF before inserting an asset; only TIFF RAW containers
  may fall through to LibRaw (ADR-0046). First-frame Bayer/X-Trans RAW/DNG uses pinned
  LibRaw crop/black/white/CFA/flip plus owned DNG OpcodeList2/3 metadata and
  RCD or Markesteijn 3-pass according to CFA; PPG and Markesteijn 1/3-pass are
  explicit recipe/CLI/Studio choices.
  GainMap executes after linear-reference normalization;
  supported List3 corrections execute after demosaic and before white balance
  and input colour. Missing, corrupt, unrecognized, oversized, malformed or
  mandatory-unsupported DNG opcodes, unsupported/non-RGB CFA, sensor/mode
  mismatch, and cancelled inputs fail structurally; optional skips remain
  visible in inspect state and no lower-quality demosaic fallback is selected.
  Catalog unpacks a RAW with no embedded JPEG before publication. A corrupt
  preview PNG is a cache miss and is rebuilt on request or after close/reopen.
  The cache has a 512 MiB hard byte budget, promotes valid hits, and evicts the
  least-recently-used rebuildable PNG deterministically across reopen
  (ADR-0047/0067).
- Browse & Review includes ratings, color labels, and reject
  state; Gallery grid/loupe and an Edit pane; a filmstrip that contains whole
  images like the grid and shows number/rating/flags in its letterbox;
  collapsible folder tree; left Import/Export; Fit/Fill/1:1; validated
  filename/metadata/camera text, media, edit/review/folder/tag/capture/numeric
  filtering and stable import/capture/name/rating/size sorting; additive
  Cmd/Ctrl click and range Shift selection; plus RGB
  Histogram/Waveform/Parade/D50-u*v*-Vectorscope/Split scopes in the right panel.
  Preview refresh computes the Curves histogram plus the currently visible
  scope instead of rebuilding all five diagnostics on every slider event.
  Photo navigation uses bounded Flickable pan plus a normalized left
  navigator; hovering the inspect photo shows a magnifier and a click animates
  to 1:1 while restoring the last Fit/Fill/custom view (ADR-0076). Active-photo,
  browse-mode, and zoom changes recenter, while review edits on the same photo
  preserve the current pan (ADR-0060).
  Capture metadata can be explicitly refreshed from the current original; the
  asset identity, capture row, and catalog revision publish transactionally.
  Schema-v7 keyset paging and the sparse Studio model expose the full logical
  library while retaining at most three 200-row pages; current-page tags,
  metadata, previews, and bounded Gallery thumbnail look-ahead replace
  whole-catalog materialization. The 10,000-row SQLite traversal pins stable
  ordering, materialized-row bounds, elapsed query metrics, and query-plan
  indexes. Studio import enumerates deterministically and dispatches one
  normal-priority photo at a time, so foreground Develop work interleaves and
  cancellation stops every undispatched item (ADR-0100).
- Studio built-in commands are projected by one C++ registry into menus,
  shortcuts, controls, and the top command palette. macOS uses
  `Cmd+Shift+P`; Windows/Linux use `Ctrl+Shift+P`; unavailable commands retain
  a visible reason. The photo context menu copies versioned English identity
  and current canonical-parameter blocks without assembling recipe text in QML.
- Live Studio control uses `ravo-studio-control/v1` over an owner-only local
  socket. CLI can discover sessions, read the revisioned current selection and
  current/saved recipe, commit an ordered strict Develop batch through the
  command controller, wait for the saved preview, and publish an exact
  no-replace probe PNG. Selection and state revisions reject stale requests;
  no assistant credential or image byte enters the socket (ADR-0090).
- Studio UI supports English, German, Spanish, French, Brazilian Portuguese,
  Simplified and Traditional Chinese, Japanese, and Korean. The desktop-owned language
  manager synchronously persists the normalized selected language, repairs a
  malformed stored value to English, resolves system-locale aliases from the
  versioned locale manifest, and loads only the build-produced Qt catalog,
  and leaves the prior language active on package or persistence failure.
  Service/engine machine errors remain outside the translation contract. Other
  current view controls are session state rather than hidden settings
  (ADR-0066). Studio also persists typed Assistant endpoint URL, model, and API
  key (ADR-0081) for the floating chat panel; assistant HTTP and credentials
  stay desktop-only. The separate Qt local-control socket exposes neither.
- Basic Develop uses the current catalog schema v9 with one canonical recipe per image,
  tags/writable metadata, and persistent history/snapshots. CPU supports RAW
  highlight reconstruction (opposed by default), adaptive Y0U0V0 edge-aware
  wavelet denoising, lensfun poly3/vignette, dt UCS `colorequal`, graduated
  filter, and nine-band toneequal with a scale-stable log-EV guided mask. Its
  five photographic controls drive all nine one-stop bands without the former
  sparse inverse; low-contrast texture is retained while strong subject/
  background edges do not leak correction halos (ADR-0092). Studio provides
  an Edit panel whose left rail lists the selected photo's recipe history and
  snapshots. Clicking a history step previews that recipe without appending;
  newer steps dim until a later parameter edit discards them in the same
  recipe transaction.
  Schema v6 additionally owns retryable per-asset recovery generations in the
  catalog support directory. `ravo catalog sidecar-status|sidecar-sync` exposes
  their state, and `catalog backup|backup-verify|backup-restore` creates,
  verifies, and restores an
  immutable catalog/sidecar backup that excludes originals and rebuildable
  previews to an explicit absent catalog path. These `.ravo.json` mirrors never
  replace SQLite authority or touch adjacent XMP. `backup` takes the live
  `--catalog`; self-contained `backup-verify` and `backup-restore` take an
  explicit `--backup`, and restore verifies complete staged state before
  support-first/catalog-last publication and an ordinary catalog reopen.
  `preview-rebuild` repairs selected or all rebuildable cache entries. Studio
  exposes the same recovery, backup, restore, rebuild, progress, and
  cancellation owners (ADR-0097/0099).
  Schema v8 persists a verified backup schedule with last success, next run,
  bytes, failure, and safe retention. Only strict, reverified current-catalog
  artifacts are quarantined, reverified, and deleted; unknown paths remain.
  Schema v9 assigns stable IDs to direct containing folders. `catalog folders`
  reports missing roots and `folder-relink` validates every replacement file
  identity before one transaction updates paths, recovery generations, and
  revision without writing an original. Studio exposes both workflows through
  the command registry (ADR-0101).
  Consecutive commits from one control update a single history row and remain
  one session Undo step; changing controls or navigation/history state starts
  a new step. Section lamps are gray at identity, green when modified, and
  black when those parameters are kept but bypassed. The default grading stack
  is White Balance, Light, Curves, Color Equalizer, Color Balance RGB wheels,
  and Camera Calibration (ADR-0082/0084/0085). Color Equalizer is an eight-band
  named mixer. Light begins
  with White Balance, then presents Exposure, Contrast, Highlights, Shadows,
  Whites, and Blacks; Exposure mode and black point, Deflicker, Sigmoid shaping,
  Gamma, and RGB Levels follow. Bayer RAW white-balance pick writes
  manual coefficients (ADR-0083);
  plus single-photo Before/After, a toolbar Left/Right comparison whose two
  panes share zoom and pan, session undo/redo, and selective Copy Parameters /
  Paste Parameters. Copy opens the same initially-empty modified-parameter
  chooser as preset saving; paste preserves every unselected destination edit
  (ADR-0078/0098).
  RAW preview retains bounded 960px interactive and 1600px settled
  scene-linear working images. An ordinary committed edit publishes the exact
  960px in-memory result first, then replaces it with the exact persisted
  1600px result. The foreground live slot also retains an exact pre-light RGB
  prefix and its bounded row team, so Exposure does not recompute unchanged
  calibration, denoise, or lens/canvas stages. Prefix changes publish only
  after successful completion and invalidate with the working generation.
  Superseded requests cancel and late results are dropped by revision;
  recipe/history/revision save atomically. Preview-cache PNG favors latency
  because it is rebuildable, while export encoding is unchanged
  (ADR-0087/0089). Catalog unit tests cover L2–L9 parameters and pixel reopen
  contracts.
- Reusable complete styles use `.rstyle.json` schema v1: a complete canonical
  Recipe template including masks, profiles, and enabled/bypass state. Schema
  v2 adds a sorted explicit field selection and overlays only those chosen
  current modifications onto the target Recipe. Studio places **Save…** to the
  right of **Import…**, starts with no modified parameter selected, and writes
  the chosen subset into the library's `Ravo Presets` folder with atomic
  complete-file publication that rejects a pre-existing path. CLI can validate
  both schemas and apply them to an explicit target Recipe; Studio applies
  through ordinary recipe history/undo.
  Legacy `.dtstyle` is structurally unsupported rather than partially dropping
  unknown IOPs (ADR-0065/0098). Lightroom
  Classic CRS XMP presets import and apply through the same explicit path onto
  accepted Develop owners. Studio keeps its imported copies in the library's
  `Ravo Presets` folder; their filename-owned labels can be renamed without
  rewriting preset contents, and deletion requires explicit confirmation.
  Exposure retains EV semantics, RAW contrast maps to
  sigmoid, highlights/shadows use calibrated scene-EV envelopes, Whites/Blacks
  use narrower monotonic hue-preserving envelopes instead of global endpoint
  subtraction, and composed point curves run on an explicit display-sRGB axis
  after sigmoid. Unknown or
  active Adobe-only state fails closed; built-in Adobe profile/look omissions
  remain reported rather than emulated (ADR-0086/0088/0091).
- RAW Repair provides `ravo.raw.hotpixels` v1 on an owned Bayer CFA copy under
  the frozen same-colour four-neighbor path. `ravo.raw.cacorrect` v1 retains
  RawTherapee two-pass tile/polynomial fitting and avoid-color-shift. Unit
  tests cover cancellation, sensor rejection, memory budget, cache immutability,
  catalog reopen, and real RAW references for both.
- Profile Denoise keeps the accepted Y0U0V0 edge-aware à-trous BayesShrink,
  calibrates actual stabilized noise from bounded deterministic MAD samples,
  and gives Radius defined multiscale spatial behavior. Luminance and Chroma
  mix separately into an owned result; scale, finite, cancellation, and exact
  memory preflight failures publish nothing (ADR-0094).
- RAW denoise v2 owns the frozen five-band square-root wavelet path before
  demosaic. Bayer processes four 2×2 CFA planes; X-Trans reconstructs one
  dense nearest-neighbour plane for each RGB channel before the same transform
  and writes back only matching sensels. Cancellation publishes no partial
  CFA, and its peak four-float-plane scratch is included in the RAW budget.
- White Balance provides `ravo.color.temperature` v1 before demosaic using
  R/G1/B/G2 four-channel coefficients. LibRaw `cam_mul` / `pre_mul` provide
  as-shot and camera-reference defaults; manual stores explicit coefficients,
  while late-reference permits only a following explicit channel-mixer CAT. The
  old Kelvin/tint RGB approximation and generic fallback are removed.
- Exposure provides `ravo.core.exposure` v2 with the frozen manual EV and black
  response, optional camera exposure-bias/highlight-preservation compensation,
  and deflicker percentile-to-EV analysis. RAW deflicker owns an immutable 65,536-bin snapshot
  from the original decoded sensor data before repair, resize, or demosaic;
  private pinned Exiv2 supplies value-only metadata without crossing the engine
  boundary. Memory, cancellation, missing-tag, metadata-read-failure, and raster
  unsupported states are explicit. CLI render, Catalog preview/save/reopen/
  export, and Studio use the same recipe/engine path. The legacy spot picker is
  not serialized exposure math, and the strict importer accepts only the exact
  default-unmasked legacy boundary.
- Input Color provides `ravo.color.input` v1 with explicit input/working
  profile identifiers, intent, gamut-normalization target, and RAW blue
  mapping. RAW publishes a camera-to-XYZ D50 matrix; raster decode preserves
  embedded ICC state. Matrix/shaper profiles use the frozen LUT and unbounded
  path, while general RGB/XYZ/Lab ICC input uses private pinned LittleCMS.
  Missing, corrupt, singular, or unsupported profiles fail structurally; no
  generic matrix or sRGB fallback is used. Canonical recipe schema v3 upgrades
  prior Ravo recipes by inserting explicit source → linear Rec709 and output
  colour boundaries. Input profile state and external ICC content participate
  in the scene-linear and preview cache keys, and Studio exposes the canonical
  Input Profile controls.
- Unbreak Input Profile provides `ravo.color.profilegamma` v1 as an explicit
  pre-input correction for profiles that expect non-linear RGB. Logarithmic
  mode retains the frozen `fastlog2` and `2^-16` floors; gamma mode retains the
  65,536-sample piecewise table and unbounded extrapolation. RAW runs it after
  demosaic and raster runs it on decoded RGB, both before Input Color. The
  operation is opt-in, cache-keyed, and never substituted by the simplified
  `ravo.core.gamma`. Studio exposes manual mode controls; legacy picker/
  autotune remains unsupported until it has a deterministic analysis contract.
- Output Color provides `ravo.color.output` v1 with built-in/file ICC output,
  four rendering intents, soft proof, gamut warning, proof intent, and black-
  point compensation. Matrix/shaper output uses the frozen 65,536-sample LUT
  and unbounded extrapolation; general RGB/XYZ/Lab and proof transforms use
  render-local LittleCMS. Preview contract v10 and `RenderedImage` carry owned
  ICC state. CLI PNG emits standard sRGB metadata or `iCCP`, Catalog PNG/JPEG/
  TIFF embeds the same declared profile, and missing/corrupt profiles fail
  before atomic publish. Studio presents the engine-owned result through Output
  & Soft Proof controls; it never infers a monitor profile or performs a QML
  colour transform.
- JPEG export uses the pinned private libjpeg-turbo encoder and one typed
  quality/subsampling request. Quality defaults to 95 within the frozen 5–100
  range; automatic sampling follows the frozen quality thresholds, while
  service callers may select 4:4:4, 4:4:0, 4:2:2, or 4:2:0 explicitly. The CLI
  exposes quality and `auto|444|440|422|420` subsampling. Studio and CLI share those typed values. Rendered JPEG embeds Catalog-owned Exif APP1,
  standard XMP APP1, and optional IPTC Photoshop APP13 from one snapshot before
  ADR-0032 publication, including validated capture time/offset/GPS;
  this newly embedded packet is not automatic sidecar interchange (ADR-0063).
- PNG export uses one typed bit-depth/compression request from CatalogService
  through the raster port. It defaults to 8-bit and compression 5, accepts
  compression 0–9, and delegates RGB8 output to a private libpng owner
  with frozen zlib/filter settings, opaque non-interlaced RGB, resolved ICC,
  and cICP only for recognized built-in profile state. Product PNG16 requests
  render engine-owned RGB16 into that encoder; an RGB8 source still rejects
  16-bit structurally instead of expanding 8-bit samples. CLI exposes
  `--png-bit-depth 8|16` and `--png-compression 0..9`; Studio exposes the same typed PNG options. Rendered PNG embeds one
  `eXIf` TIFF profile and one uncompressed XMP `iTXt`; it still writes no pHYs
  or IPTC. Validated capture time/offset/GPS from Catalog schema v5 are
  included; no adjacent sidecar is read or written (ADR-0063).
- TIFF export uses one typed sample/compression/resolution request from
  CatalogService through the raster port. It defaults to unsigned 8-bit,
  Deflate with the horizontal predictor, level 6, RGB output, and 300 dpi;
  resolution accepts 72–9600 dpi in inches. A private static LibTIFF owner from
  the pinned source root writes bounded classic little-endian, top-left, opaque
  contiguous strips and the exact resolved RGB ICC; optional grayscale retains
  the frozen interior-channel threshold. Catalog snapshots writable metadata,
  capture values, and sorted tags once for every rendered export after asset
  lookup. TIFF also keeps the normalized destination as `DocumentName`. The main
  IFD writes bounded UTF-8 `DocumentName`, description, creator, copyright,
  Make/Model, EXIFIFD, XMP 700, and optional IPTC 33723; title stays out of Exif,
  and `DocumentName` remains the destination rather than the title. CLI
  `catalog export --format tiff|tif` exposes `--tiff-sample-type` with
  `uint8|uint16|float16|float32`, `--tiff-compression` with
  `none|deflate|deflate_predictor`, `--tiff-compression-level` from 1 through 9,
  `--tiff-grayscale-if-neutral`, and `--tiff-resolution-dpi` from 72 through 9600.
  Value-bearing TIFF flags use the existing last-value-wins CLI rule, while the
  boolean flag rejects duplicates; every TIFF flag is rejected outside a TIFF export.
  Studio exposes the same typed TIFF options. Product uint16/float16/float32
  requests render engine-owned RGB16 or finite RGB float; an RGB8 source still
  fails structurally instead of fabricating precision. Automatic sidecar
  interchange is explicitly unsupported: Catalog is the edit authority,
  legacy XMP conversion is an explicit CLI operation, and rendered XMP is
  newly embedded (ADR-0063). Rendered JPEG/PNG/TIFF share typed
  `full|no-location|none` privacy modes; `none` retains ICC only and
  original-copy rejects stripping (ADR-0064). Multipage masks, shared
  consumers, and retirement remain later I13/S9 work. TIFF bytes complete in
  memory before the shared ADR-0032 atomic no-replace publication; source and
  sidecar files are never rewritten.
- Batch export accepts 1–10,000 ordered unique assets and one bounded portable
  filename template using only `{stem}`, `{asset_id}`, `{sequence}`, and
  `{ext}`. CatalogService preflights all sources, duplicate names, and existing
  targets before the first output; every item then uses the same typed options
  and atomic no-replace path as single export. A later cancellation or runtime
  failure reports stable partial-delivery context without deleting completed
  files. CLI exposes `catalog export-batch`; Studio uses a folder chooser and
  shows the template only for multi-selection (ADR-0068).
- Output Dither / Posterize is explicit recipe state after Output Color and
  before sample packing. It provides deterministic random TEA noise, all frozen
  Floyd–Steinberg gray/RGB/auto bit-depth modes, and 2–8-level per-channel
  posterization. Auto selects 256/65,536 levels for RGB8/RGB16 exports and only
  clamps preview/float output. Studio exposes all methods and random damping;
  CLI uses ordinary strict Develop fields (ADR-0069).
- Canvas and Output Frame are separate explicit operations (ADR-0070). Canvas
  grows the linear Rec.709 working image with independent left/right/top/bottom
  percentages and five opaque solid colours while retaining the original photo
  as the coordinate frame for masks and Retouch. Frame runs last after Output
  Color and optional Dither, with constant/image/custom aspect, orientation,
  basis, position, border colour, and an optional coloured line. Catalog
  preview/reopen and JPEG/PNG/TIFF export share the resulting dimensions.
  Perspective/straighten and crop transform both pixels and preview-mask alpha
  from that attached frame. Canvas followed by rotate, flip, or lens geometry,
  or by a new mask consumer after composed geometry, remains an explicit
  unsupported state.
- Perspective is one canonical scene-linear homography for angle, vertical and
  horizontal correction, shear, constrained maximal safe crop, and bilinear,
  Lanczos2, or Lanczos3 sampling. The CLI and Studio use the same bounded Hough
  line detector and robust fitter; no-line, degenerate, cancelled, malformed,
  or resource-invalid analysis fails without changing the recipe. Crop remains
  a subsequent normalized operation, and export executes the same CPU path.
- Text Watermark is the final encoded-output recipe stage after Frame and
  before sample packing. It uses a versioned built-in 5×7 font, bounded ASCII
  text, `{stem}`/`{asset_id}` expansion, RGB/opacity, scale, rotation, nine-way
  alignment, and normalized offsets. Preview, styles, reopen, and every
  rendered export share the same pixels. External SVG/PNG lookup, system fonts,
  arbitrary metadata variables, and the missing legacy `promo.svg` no-op are
  explicitly unsupported (ADR-0071).
- Final display packing is an engine-private boundary after Output Color and
  any Dither, Frame, and Watermark stages. It
  converts finite profiled float RGB to owned RGB8 by clamping negative values,
  multiplying by 255, rounding, and clamping super-white while retaining the
  exact profile and RGB channel order; it applies no second transfer curve.
  Frozen XMP `gamma` is absorbed only for the exact mandatory singleton state
  and emits no recipe operation. The old channel/mask display branches are
  unsupported presentation adapters.
- RGB Primaries provides `ravo.color.primaries` v1 with working-profile-aware
  red/green/blue hue and purity plus achromatic-axis tint. The engine derives
  the frozen custom-primary matrix from immutable RGB→XYZ D50 state before the
  linear-Rec709 compatibility bridge, retains the declared working profile,
  and publishes an owned result only after finite/cancellation checks. Studio
  exposes all eight canonical controls; hue is persisted in radians and shown
  in degrees.
- Color Calibration provides `ravo.color.channelmixerrgb` v1 with frozen V3
  CPU matrix normalization, CAT16/Bradford/XYZ/RGB, XYZ gamut, saturation,
  lightness, and grey paths. Studio exposes an explicit 3×3 matrix, while CLI,
  preview, and export reuse the same engine operation.
- Color Checker provides the independent `ravo.color.colorchecker` v1 contract
  in D50 Lab. An explicitly present operation owns 0–49 ordered source/target
  patch pairs and the frozen N=0–4 polynomial or N>4 thin-plate RBF fit, while
  absence alone skips the operation. The fit exactly retains the frozen fast-log,
  Gaussian solve, and singular fallback; Ravo privately adds the explicit
  linear-Rec709↔D50 Lab bridge around the Lab owner. Studio exposes all eight
  frozen presets and direct Lab patch editing; CLI and Catalog share the same
  recipe, cache, and CPU path. Strict XMP import accepts the one evidenced
  enabled v2 default-unmasked record and a synthetic v1 history upgrade. The
  complete 0098 history remains a structured negative because unrelated earlier
  operations are unsupported; masks, custom blend, multiple instances, and
  disabled legacy state also reject rather than acquiring invented semantics.
- Color Balance RGB provides `ravo.color.colorbalancergb` v1 in the explicit
  `linear_srgb_d50` workspace, with Filmlight Yrg three-zone luminance mask,
  grading RGB offset/slope/power, fulcrumed luminance, and DT UCS 2022 as the
  default saturation/brilliance gamut path. JzAzBz 2021 is an explicit optional
  formula. Studio exposes the complete canonical parameters; the prior
  Lift/Color gamma/Gain approximation operation is removed.
- Legacy Color Balance provides the separate `ravo.color.colorbalance` v1
  contract for the complete frozen lift/gamma/gain and slope/offset/power
  paths. Its 17 legacy fields drive the Lab D50/ProPhoto conversion, corrected
  RGBL controls, input/output saturation, and grey-fulcrum contrast. Operation
  presence is explicit because even default parameters execute the frozen
  colour-space round trip. Strict XMP import accepts only synthetic v3/v4
  default-unmasked singleton state; the real 0033/0034 histories establish
  structured mask/custom-blend/multi rejection, not positive compatibility.
  CLI, Catalog, and Studio share the same CPU implementation and cache identity.
- Color Correction provides the independent `ravo.color.colorcorrection` v1
  contract with explicit operation presence and exactly five bounded numeric
  controls: highlight/shadow a*/b* endpoints and saturation. The engine reuses
  the private source-derived linear-Rec709↔D50 Lab bridge, preserves the frozen
  float affine expression order, and does not short-circuit an explicitly
  present default operation. Strict XMP import accepts only the enabled-v1,
  singleton, priority-zero, unnamed, default-unmasked envelope represented by
  0029/0092; masks, custom blend, multi-instance, disabled, malformed, and
  unknown state reject structurally. CLI render, Catalog preview/save/reopen/
  export, and Studio's five generic Develop intents share the same operation
  and cache identity. The old GTK plane/picker, three presets, and OpenCL path
  are not product contracts; shared kernel/order/registry/style/pixmap assets
  remain separately owned cleanup.
- Color Contrast provides the independent `ravo.color.colorcontrast` v2
  contract with explicit operation presence and exactly seven fields:
  `working_space=lab_d50`, `algorithm=axis_affine_v2`, separate a*/b*
  steepness and offset values, and the bounded/unbounded switch. The engine
  privately bridges linear Rec709 through D50 Lab, narrows once to float, and
  retains the frozen per-axis multiply/add and clamp order. The frozen module-v1
  upgrade adds `unbound=false`; the former Ravo `amount` v1 recipe maps
  deterministically to both slopes, with zero retaining its historical skip.
  Explicit schema-v2 defaults remain present and observable. Strict XMP import
  accepts the verbatim enabled-v2 singleton, priority-zero, unnamed,
  default-unmasked record from 0038 plus a synthetic legacy-v1 upgrade under
  the same presentation envelope; the complete masked 0038 document and all
  custom blend/multi states reject structurally. CLI render, Catalog
  preview/save/reopen/export, and Studio's full generic Develop controls share
  the same recipe, cache, cancellation, ownership, and error path. GTK sliders,
  OpenCL execution, and canonical mask attachment are not Color Contrast
  product contracts. The owner has no 2D plane, picker, or three-preset
  algorithm and does not inherit those adjacent Color Correction presentation
  assets. Shared `extended.cl`,
  order/modulegroup/usermanual names and frozen fixtures remain
  D0.3/D0.4/S14/E1 owners. Bundled `.dtstyle` examples are retired under
  ADR-0072.
- Color Harmonizer provides the bounded `ravo.color.colorharmonizer` v1
  operation with exactly 17 flat fields, including
  `working_space=profile_linear_rgb_d50`,
  `algorithm=dt_ucs_harmony_v1`, nine predefined rules plus custom nodes,
  pull/neutral/width controls, four node saturations, and smoothing. The
  accepted CPU path clips negative RGB with frozen
  `fmaxf`, uses the declared profile matrix and private source-order dt-UCS/RYB
  geometry, cubes neutral protection, and disables float contraction. Positive
  smoothing caches JCH plus two corrections, uses the immutable canonical ROI
  scale carried by the working buffer, and applies private S2.2's source-order
  two-channel recursive Gaussian with the frozen ±1e9 per-read bound. Strict
  v1 XMP import accepts only the evidenced zero-smoothing
  singleton envelope from frozen 0176 records 12/13 and maps the greatest
  history position to one canonical 17-field operation; the complete 0176
  document is not a compatibility claim. Explicit Develop presence, CLI
  `--set`, Catalog preview/save/reopen/export, and one Studio section share
  that recipe. Studio exposes enable, the 10-rule selector, hues/strengths,
  custom nodes 2–4, four saturations, and the bounded smoothing slider. The
  independent source-order oracle matches production bits on each host, while
  libm-dependent references for the default and edited records 12/13 use a
  1e-5 component tolerance across supported platforms. Successful output owns
  RGB/profile storage and retains immutable analysis state; invalid,
  non-finite, allocation, invalid-ROI-scale, and cancellation paths publish
  nothing. S3.1 additionally gives canonical recipes an immutable typed mask
  DAG (`all`, linear gradient, circle, rotated ellipse, parametric, ordered
  group) and private ROI alpha evaluation with normal mix. S3.2 lets Studio
  author one unshared Studio-owned all, spatial, or parametric leaf for each Color
  Harmonizer/Graduated ND attachment through the same typed Recipe, live
  preview, Catalog cache/save/reopen, ordinary Develop edit, and undo/redo
  path. Studio can show a preview-only yellow mask overlay, author owned group
  children, and author path/brush leaves. External and shared attachments stay
  read-only except for explicit detach. Legacy XMP masks/custom blends remain
  rejected. Historic blend modes, leftover GTK mask-manager consumers, and C15
  remain unfinished.
- Color Zones provides optional `ravo.color.colorzones` v1 alongside the
  default Color Equalizer. A selected D50 Lab lightness/chroma/hue axis indexes
  independent lightness, chroma, and hue curves with 2–20 nodes, cubic,
  Catmull–Rom, or monotone interpolation, source-quantized 65,536-entry LUTs,
  mix strength, and canonical masks. Studio offers an eight-band editor while
  preserving imported custom nodes read-only. The exact 0022 v5 singleton maps;
  the old IOP/kernel/GTK graph settings/icons are retired (ADR-0073).
- Monochrome provides `ravo.color.monochrome` v2 with a D50 Lab a*/b* virtual
  colour filter, size, highlight preservation, and mix. Its source fast-exp
  filter is smoothed by the shared scale-aware bilateral-grid owner before
  neutral Lab output. Canonical masks, strict 0017 v2 import, CLI/Catalog/Studio
  persistence, cancellation and resource accounting replace the former
  chroma-only amount shortcut (ADR-0074).
- Split Toning provides `ravo.color.splittoning` v2 in linear Rec.709 with
  independent shadow/highlight hue and saturation, pivot balance, midtone
  compression, mix, and canonical masks. It retains the frozen HSL branch,
  doubled-distance weight, blend and clamp order; the exact 0062 v1 singleton
  maps and the old fixed-saturation/compression shortcut is removed
  (ADR-0075).
- Velvia provides `ravo.color.velvia` v2 in linear Rec.709 with strength and
  mid-tones bias. It preserves the frozen luminance/saturation weighting so
  low-saturation pixels receive the strongest colour boost, then applies the
  source per-channel clamp. Canonical masks, strict 0063 v2 import, and typed
  CLI/Catalog/Studio save/reopen/export replace the former one-slider,
  fixed-bias persistence path (ADR-0095).
- 3D LUT provides optional `ravo.color.lut3d` v1 after Velvia. A recipe names
  one bounded `.cube` file, explicit input and output colour spaces, trilinear
  or tetrahedral interpolation, and strength. The engine converts from and
  back to canonical linear Rec.709, honours per-channel `DOMAIN_MIN/MAX`, and
  blends in unbounded linear light without an output clamp. A process-wide
  eight-entry LRU retains immutable parsed snapshots keyed by canonical path
  and complete-file fingerprint; changed, missing, malformed, non-finite,
  oversized, 1D, or unsupported resources fail without stale/identity
  fallback. CLI inspection and text-field mutation, Catalog cache identity,
  Studio, and export share this owner. Frozen legacy XMP only records a mutable
  external path and is rejected as `unsupported_legacy_lut3d_resource`
  (ADR-0096).
- Offline camera-noise calibration fits
  `variance = gaussian_variance + poisson_slope × signal` from a strict,
  versioned mean/variance/count sample document in black-subtracted uint16
  sensor units. A deterministic Theil–Sen/MAD selection followed by weighted
  non-negative least squares rejects bounded outliers and never substitutes a
  default profile. `ravo noise calibrate` publishes a canonical SHA-256 profile
  only to an explicit atomic no-replace destination; `noise inspect` validates
  the checksum. Neither command mutates a catalog, source image, or implicit
  user directory, and profile lookup by denoisers remains a separate contract
  (ADR-0096).
- Color Reconstruction provides `ravo.color.colorreconstruct` v1 immediately
  before Output Color. It reproduces the frozen D50 Lab full-frame bilateral
  grid: non-clipped colors are weighted by none/chroma/hue precedence, blurred
  in spatial and lightness dimensions, and sliced into highlight a*/b* while
  retaining L*. Canonical ROI scale keeps spatial extent stable across bounded
  previews; invalid scale, memory, non-finite, and cancellation paths publish
  nothing. The sole 0052 v3 default-unmasked singleton imports strictly, while
  masks, custom blend, multiple instances, and other versions reject. CLI,
  Catalog save/reopen/export, and Studio expose the same five photographic
  parameters; GTK/OpenCL and tile-local substitutes are not supported.
- Texture provides optional `ravo.detail.texture` schema v1 before Sharpen in
  linear Rec.709. Strength zero is identity; signed strength, original-input
  detail scale, and one to five iterations drive a bounded two-band
  self-guided luminance decomposition. Output is applied as a positive RGB
  ratio, preserving hue and unclipped HDR highlights instead of clipping
  channels. The six-float-plane peak, canonical scale, finite input/output,
  cancellation, source ownership, Recipe/CLI/Catalog/Studio persistence and a
  Release 30 ms algorithm gate are tested. Studio puts the common Texture
  control first in Detail and collapses scale/iterations. Local Laplacian and
  Filmulator physical development were measured but do not enter production
  (ADR-0096).
- Sharpen provides `ravo.detail.sharpen` schema v2 in D50 Lab. Radius is scaled
  from original-input pixels, multiplied by the frozen 2.5 support, and capped
  at a 12-pixel convolution radius while retaining the requested Gaussian
  sigma. The source-order separable blur changes L* only when detail exceeds
  threshold; a*/b* and borders remain unchanged. Existing Ravo v1 values
  upgrade to this accepted meaning. Three evidenced legacy v1 singleton
  records import strictly; masks, custom blend, multi-instance, and other
  versions reject. CLI, Catalog and Studio share amount/radius/threshold,
  cancellation, resource, reopen and export contracts. Demosaic capture
  sharpening remains a separate R2 owner.
- Dehaze provides `ravo.effect.dehaze` schema v2 at the source-linear RAW
  stage. It estimates ambient RGB and characteristic haze depth from the 95%
  dark-channel/brightness quantiles, builds scale-aware transition windows,
  and refines transmission with a bounded tiled RGB covariance guided filter
  before applying the atmospheric equation. Existing Ravo v1 amount upgrades
  to the accepted strength/distance/adaptive contract; the constant-airlight
  shortcut is removed. Two evidenced legacy singleton records import strictly.
  Encoded raster, masks/custom blend/multi, invalid ambient/scale/resource and
  cancellation states fail structurally. Catalog cache/save/reopen/export and
  Studio share strength, distance, and adaptive controls. The old GTK preview
  cache and OpenCL path are not ported.
- Retouch provides `ravo.repair.retouch` schema v1 on the canonical linear
  Rec.709 D50 buffer. Ordered regions reference circle/ellipse/path/brush
  leaves and retain clone/heal source points, Gaussian/bilateral blur,
  erase/color fill, opacity, and original/detail/residual wavelet scale. Later
  regions can read earlier results. The four frozen fixture families map only
  through their evidenced Retouch and v6 mask payloads; unaccepted operations
  elsewhere in those documents still reject. Recipe/CLI/Catalog/Studio share
  save, preview, reopen, export, cancellation, and resource contracts. Studio
  authors bounded circle regions; imported canonical path/brush regions remain
  reproducible. The old GTK IOP and exclusive OpenCL kernel are removed, while
  shared DWT/heal/bilateral and historic mask/order consumers remain.
- RAW preview/export uses `ravo.display.sigmoid` v1 as the sole Standard SDR
  display transform. Recipes may adjust contrast/skew/hue preservation, while
  the default baseline is not marked as a user edit. Gallery embedded-JPEG
  thumbnails and inspect dimensions are corrected to camera orientation.
  Configure requires JPEG/GIF/WebP/TIFF imageformat plugins and the QSQLITE
  driver; missing them is a hard error.

The active product order—catalog recovery/restore first, then bounded
large-library and Gallery-to-Edit behavior—is in
[TODO_PHOTO_MANAGEMENT.md](../TODO_PHOTO_MANAGEMENT.md). Remaining legacy
absorption and retirement uses the separate `MR*` queue in
[TODO_LEGACY_MIGRATION.md](../TODO_LEGACY_MIGRATION.md); changes of direction
are recorded in dated ADRs, beginning with
[ADR-0007](docs/adr/0007-first-usable-catalog-viewer.md).

## First-version loop

The first version must complete the following:

1. Create or open a catalog database in Ravo Studio.
2. Import local files/directories, carrying at least one PNG and real
   `mire1.cr2` through tests.
3. Show assets in Gallery; select one and view it at fit, 1:1, and with pan.
4. Restart and reopen the same catalog for viewing.
5. Duplicates, corruption, missing files, non-writable paths, and cancellation
   have visible, recoverable structured results.
6. Originals are always read-only; previews are atomically written,
   rebuildable caches outside the database.

The first desktop uses Qt 6 Quick/QML. C++20 composition/presenters own
services, tasks, and resources; QML performs only layout, presentation,
binding, and input. A private QSQLITE adapter owns SQLite and a private
`QImageReader` adapter decodes the first JPEG/PNG path. UI consumes only
presenter-exposed service state and read-only previews; SQL, codecs, RAW
processing, tasks, and cache do not enter QML. The first version links no Qt
Widgets and retains no Widgets fallback.

## Build and test

First inspect active source-root state from the repository root:

```text
python3 configs/source_roots.py show --format json
python3 configs/source_roots.py resolve --format json
python3 configs/source_roots.py verify
```

After authorized first workspace preparation or an active-lock change, run:

```text
python3 configs/source_root_workflow.py --init
python3 configs/source_root_workflow.py --update
```

`--init` is the only dependency action allowed to use the network; `--update`
materializes source roots offline and generates the root `CMakePresets.json`.
Packaging runtime paths are supplied by the active lock's
`RAVO_PACKAGE_RUNTIME_SEARCH_PATHS`; the template stores only three-platform
examples. Ordinary Build/Test/Run does not implicitly run Config or dependency
updates.

macOS Debug:

```text
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug
```

Use `win_msvc_debug` / `win_msvc_release` on Windows and `linux_clang_*` on
Linux. FreeCM Config/Build/Run/Test use the same CMake preset commands.
`Ravo/tools/freecm_project.py` and `.ps1` are optional wrappers only.

Release staged install:

```text
cmake --preset mac_clang_release
cmake --build --preset mac_clang_release
cmake --install build/mac_clang_release --prefix install/mac_clang_release
```

Release package with FreeCM runtime deployment:

```text
cmake --preset mac_clang_release
cmake --build build/mac_clang_release --target RavoPackage
```

The same target produces a Windows ZIP with `win_msvc_release` and a Linux
AppDir tar.gz with `linux_clang_release`. `RavoPackage` includes Ravo Studio,
the `ravo` CLI, Qt/QML runtime dependencies, and the license. Output paths and
CI artifact ownership are documented in [Packaging](../DevDocs/Packaging.md).

FreeCM Package follows the active Config, so Debug and Release each have a
compatible Package variant. Run Config before Package; tagged CI releases
always use Release.

The repository-root CMake builds only Ravo; it must not configure, compile, or
run frozen 0.9 (`legacy/src/`). Windows/MSVC and local macOS/Clang have
previously validated the current engine/CLI graph; Linux still requires
validation on its target host. The addition of Qt Gui/Qml/Quick/Sql, QML
modules, runtime plugins, and desktop requires renewed three-platform results.

## Studio localization

Studio localization source and the locale manifest are versioned under
`desktop/i18n`, while QM files are build output. Refresh source and reuse each
locale's persistent translation memory with the project-local
[i18n workflow](../.codex/skills/i18n-translation-workflow/SKILL.md):

~~~text
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --repo-root . --part 1 --lupdate /path/to/lupdate
# Translate only <unfinished> values in the selected RavoStudio_<locale>.memory.ini files.
python3 .codex/skills/i18n-translation-workflow/run_i18n_workflow.py --repo-root . --part 2
cmake --build --preset mac_clang_debug --target ravo_studio_translations
~~~

The workflow requires the Qt kit's LinguistTools. It fails on malformed
translation memory, incomplete active translations, placeholder mismatch, or
invalid TS XML; it never writes source catalogs into the build tree.

## FreeCM project workflow

`configs/freecm.commands.jsonc` uses manifest v2. Debug/Release and
Windows/macOS/Linux are independent Configs. Build, Run, Test, and Package
explicitly bind a compatible Config and never configure secretly. Release
Package calls `RavoPackage` directly and shares the FreeCM deployment path used
by GitHub Actions.

Maintenance actions:

```text
python3 configs/source_root_workflow.py --refreshpin
python3 configs/source_root_workflow.py --pinlatest
python3 configs/source_root_workflow.py --update
python3 configs/source_root_workflow.py --cleanbuild --dry-run
```

`--pinlatest` uses only commits visible in local seeds and leaves the active
lock in `latest`; it is a dependency-refresh candidate, not a release baseline.
`--cleanbuild` preserves `build/dependency_seed_repos` and
`build/dependency_source_roots`. See [Dependency Workflow](../DevDocs/Dependency_Workflow.md)
for complete authority boundaries, FreeCM gitlink updates, manual integration,
and publication order.

## Current CLI capabilities

```text
ravo --version --json
ravo operations --json
ravo develop-fields --json
ravo inspect <input> --json
ravo lut inspect <look.cube> --json
ravo noise calibrate <samples.json> --output <profile.json> --json
ravo noise inspect <profile.json> --json
ravo recipe import-xmp <legacy-or-crs.xmp> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe validate <recipe> --json
ravo recipe style-create <recipe> --name <name> --output <style.rstyle.json> --json
ravo recipe style-validate <style.rstyle.json> --json
ravo recipe style-apply <style.rstyle.json> --asset-id <id> --input <input-uri> --output <recipe> --json
ravo recipe style-apply <style.rstyle.json> --target-recipe <current-recipe> --output <recipe> --json
ravo render <input> --recipe <recipe> --output <png> --backend cpu [--width N] [--height N] --json
ravo catalog create --path <library.sqlite> --json
ravo catalog import --catalog <library.sqlite> --input <file-or-folder> --json
ravo catalog list --catalog <library.sqlite> --json
ravo catalog folders --catalog <library.sqlite> --json
ravo catalog folder-relink --catalog <library.sqlite> --folder-id <id> --replacement <directory> --json
ravo catalog preview --catalog <library.sqlite> --asset-id <id> --json
ravo catalog preview-rebuild --catalog <library.sqlite> [--asset-id <id>]... --json
ravo catalog sidecar-status --catalog <library.sqlite> [--asset-id <id>] --json
ravo catalog sidecar-sync --catalog <library.sqlite> [--asset-id <id>] --json
ravo catalog backup --catalog <library.sqlite> --backup <absent-directory> --json
ravo catalog backup-verify --backup <directory> --json
ravo catalog backup-restore --backup <directory> --output <absent-library.sqlite> --json
ravo catalog backup-policy --catalog <library.sqlite> [--schedule-dir <directory>] [--interval-minutes N] [--retention-count N] [--enabled true|false] --json
ravo catalog backup-run --catalog <library.sqlite> --json
ravo catalog fields --json
ravo catalog probe --catalog <library.sqlite> --asset-id <id> [--baseline] [--set <field>=<number>]... [--set-text <field>=<text>]... [--max-edge N] [--output <file.png>] --json
ravo catalog rate --catalog <library.sqlite> --asset-id <id> --rating 0-5 --json
ravo catalog refresh-metadata --catalog <library.sqlite> --asset-id <id> --json
ravo catalog develop --catalog <library.sqlite> --asset-id <id> [--from-xmp <preset.xmp>] [--set <field>=<number>]... [--set-text <field>=<text>]... [--exposure-ev N] [--watermark-text <text>] --json
ravo catalog recipe --catalog <library.sqlite> --asset-id <id> --json
ravo catalog tag --catalog <library.sqlite> --asset-id <id> [--add <tags>] [--remove <tags>] --json
ravo catalog metadata --catalog <library.sqlite> --asset-id <id> [--title <text>] [--description <text>] [--creator <text>] [--copyright <text>] --json
ravo catalog history --catalog <library.sqlite> --asset-id <id> --json
ravo catalog snapshot --catalog <library.sqlite> --asset-id <id> --label <label> --json
ravo catalog restore --catalog <library.sqlite> --asset-id <id> --history-id <id> --json
ravo catalog export --catalog <library.sqlite> --asset-id <id> --output <file> --format png|jpeg|tiff|tif|original [--quality 5..100] [--jpeg-subsampling auto|444|440|422|420] \
  [--png-bit-depth 8|16] [--png-compression 0..9] \
  [--tiff-sample-type uint8|uint16|float16|float32] [--tiff-compression none|deflate|deflate_predictor] \
  [--tiff-compression-level 1..9] [--tiff-grayscale-if-neutral] [--tiff-resolution-dpi 72..9600] \
  [--metadata full|no-location|none] --json
ravo catalog export-batch --catalog <library.sqlite> --asset-id <id> [--asset-id <id>]... \
  --output-dir <directory> [--filename-template '{stem}-{sequence}{ext}'] \
  --format png|jpeg|tiff|tif|original [the same typed format/privacy options] --json
ravo studio sessions [--workspace-root <checkout>] --json
ravo studio state [--session-id <id>] [--workspace-root <checkout>] --json
ravo studio develop [--session-id <id>] [--asset-id <id>] \
  [--expect-session-revision N] [--expect-selection-revision N] \
  --set <field>=<number> [--set ...] [--output <file.png>] [--max-edge N] --json
ravo studio preview [--session-id <id>] [--asset-id <id>] \
  [--expect-session-revision N] [--expect-selection-revision N] \
  --output <file.png> [--max-edge N] --json
```

An existing output path returns structured `conflict`; it is never overwritten
implicitly. Catalog commands call the same services as Studio and serve as the
headless acceptance client.

`ravo develop-fields` and `ravo catalog fields` list every closed numeric or
text field, kind, and numeric range owned by the strict Develop helpers, plus
the canonical-mask prefixes. They do not require a catalog. `catalog probe` is
a read-only Develop diagnostic. It renders the current recipe, or the
synthesized product baseline with `--baseline`, through the same non-persistent
interactive-preview path as Studio. Repeated `--set name=value` and `--set-text
name=value` overrides accept the advertised fields, reject unknown, duplicate,
non-finite, invalid-resource, or out-of-range values, and return dimensions,
output-profile ID, RGB
sums/means/extrema/clipping counts, and display-luma mean. Optional
`--output <file.png>` writes a throwaway display PNG of that in-memory preview
with atomic no-replace publication; it is not a catalog preview record. The
command reloads the stored recipe and preview-record set after rendering and
fails if either changed. CLI logging remains file-only so machine JSON is the
only stdout content. An open Ravo Studio window also observes another client's
committed catalog revision within one second.

`ravo studio` is the selection-relative control surface. `sessions` discovers
live owner-only endpoints and marks this checkout; `state` returns the selected
asset, current and saved recipes, baseline-relative modified operations, and
preview identity. `develop` observes or accepts explicit session/selection
revisions, commits one strict ordered `--set` batch through Studio, waits for
save and preview settlement, and can emit the resulting PNG. `preview` renders
the exact current recipe without mutation. Both image commands use the existing
CatalogService/Engine path and return a no-replace caller-owned artifact with
MIME type, dimensions, profile, and SHA-256. Multiple matching windows require
an explicit session ID. Process logs, open files, cache activity, and window
screenshots remain non-authoritative (ADR-0090).

## Names and directories

| Name/directory | Purpose |
| --- | --- |
| Ravo Engine / `engine/` | RAW/raster, CPU preview/render, colour, and operations |
| `ravo` / `cli/` | Supported CLI and machine-JSON client |
| `foundation/` | errors, IDs, cancellation, and resource contracts |
| `recipe/` | versioned recipe/operation schema |
| `adapters/` | filesystem, codec, SQLite catalog, raster JPEG/PNG, preview cache |
| `domain/` | Asset/Catalog/Import/Preview state and ports |
| `services/` | create/open/import/list/preview use cases |
| `control/` | versioned live-session protocol and same-user local transport |
| Ravo Studio / `desktop/` | C++ presenters with Qt Quick/QML Gallery and viewer |
| `tests/` | unit, contract, catalog integration, fixtures, and later desktop smoke |

After a Debug build, Studio is at
`build/mac_clang_debug/Ravo/desktop/ravo_studio.app` (Windows:
`build/win_msvc_debug/Ravo/desktop/ravo_studio.exe`; Linux:
`build/linux_clang_debug/Ravo/desktop/ravo_studio`). Pass
`--catalog <library.sqlite>` to open an existing library directly. FreeCM Run
works like GeoDebugger/DwgParser: first run
`cmake --build --preset … --target ravo_studio`, then start the GUI directly.
The first manual loop is: Create Library → Import
`legacy/tests/0000-nop/expected.png` and `legacy/tests/images/mire1.cr2` →
select an asset → Fit / 1:1.

## Relationship to frozen `legacy/src/`

`legacy/src/` is the read-only factual source for 0.9 behavior; Ravo is the
only growth direction. Ravo may statically read source and fixtures, but
production targets must not include old private headers, link old libraries,
load old IOPs, or access global `darktable`. The frozen application also
receives no Ravo adapter. Handle the remaining old application only after Ravo
meets the root TODO's release-transition and rollback gates.

## Documentation entry points

- [AGENTS.md](AGENTS.md): Ravo subtree implementation constraints;
- [ARCHITECTURE.md](ARCHITECTURE.md): target, data, ownership, and thread
  boundaries;
- [MIGRATION.md](MIGRATION.md): one-way migration, ledger, and retirement
  rules;
- [TESTING.md](TESTING.md): first-version catalog/import/viewer and frozen
  fixture acceptance;
- [i18n workflow](../.codex/skills/i18n-translation-workflow/SKILL.md):
  source extraction, locale-specific translation memory, and catalog validation;
- [ADR index](docs/adr/README.md): durable architecture decisions;
- [root photo-management TODO](../TODO_PHOTO_MANAGEMENT.md): remaining private-
  corpus and non-macOS P0/P1 release evidence;
- [root legacy migration TODO](../TODO_LEGACY_MIGRATION.md): unfinished
  `MR*` algorithm-absorption and retirement gates only.

The repository is distributed under GPLv3; see the root [LICENSE](../LICENSE).
