# Develop

## Goal

Make non-destructive edits to a selected photo, inspect their effect, and keep
or recover the result through the catalog recipe and history.

**Last verified:** 2026-08-29 against the current Develop recipe and Studio
presenter contracts.

## Applies to

- Ravo Studio with an open library and selected photo.
- CPU Develop previews and exports.

## Prerequisites

- A library is open.
- One photo is selected.
- The original is readable for preview generation.

## How Develop works

1. Select a photo in Gallery, Loupe, or the filmstrip.
2. Switch the top view control to **Edit**, or choose **View → Edit**.
3. Adjust a control, wait for the preview, and commit the value when you finish
   editing it.
4. Use **Before / After** to compare the current rendered result with the
   unedited product baseline.
5. Continue editing, create a snapshot, or return to Gallery.

Ravo stores a versioned recipe for a non-baseline edit in the catalog. The
original file is not changed. Preview, `ravo catalog`, and export use the same
engine path, so an edit shown in Studio is the edit sent to local export. When
`ravo catalog develop` (or another catalog client) commits while Studio is
open, Studio reloads that photo's recipe and history from the catalog.

Interactive slider movement can show a lower-size preview while the control is
being moved. A committed change is saved atomically and then a normal preview
is refreshed. If several requests are made quickly, superseded previews are
cancelled or discarded; a late result must not replace the newest edit.

## Edit pane controls

The current Edit pane is a grading stack first: White Balance, Light, Curves,
Color Equalizer, Color, then Camera Calibration. Geometry, Tone equalizer,
Graduated ND, Detail, Effects, RAW repair, and profiles follow. Every numeric
control has a reset action; each section can also be reset from its section
menu.

Scopes stay above the Edit list, so they remain visible while you change White
Balance, Color Equalizer, or Color Balance RGB.

### Geometry

- Rotate left or right by 90 degrees.
- Flip horizontally or vertically.
- Enter **Crop & Rotate** mode.
- Choose a free, 1:1, 3:2, 4:3, 5:4, or 16:9 crop aspect.
- Drag the crop frame to crop.
- Drag outside the crop frame, or use Option/Alt-drag, to straighten.
- Enable **Enlarge Canvas** to reveal independent left, right, top, and bottom
  growth from 0 through 100 percent, using Green, Red, Blue, Black, or White
  fill. The checkbox is off by default; those extra controls stay hidden until
  it is enabled.

Crop and straighten values are normalized recipe coordinates. The crop overlay
is a preview aid; commit the frame to store the change.

Canvas keeps masks and Retouch coordinates attached to the original photo and
does not select the added area. A recipe that enables rotate, flip, straighten,
crop, or lens geometry after Canvas currently reports an explicit unsupported
composition instead of applying stale coordinates.

### Input Profile

Input Profile controls declare how the source enters the working color space.
The current choices include source metadata, sRGB, Adobe RGB, Linear Rec709,
Linear Rec2020, Rec709, Linear ProPhoto RGB, Display P3, and HLG P3. The
working-profile list includes Linear Rec709, Linear Rec2020, Linear ProPhoto
RGB, Display P3, and Adobe RGB.

Rendering intent options are Perceptual, Relative colorimetric, Saturation, and
Absolute colorimetric. Gamut normalization can be disabled or clipped to sRGB,
Adobe RGB, Linear Rec709, or Linear Rec2020. **RAW blue mapping** is available
for RAW inputs.

Ravo does not silently substitute sRGB when a required profile is missing,
corrupt, singular, or unsupported. The edit or export reports the failure.

### Unbreak input profile

Enable the opt-in pre-input correction for a profile that expects non-linear RGB.
Choose **Logarithmic** or **Gamma** mode and adjust the mode's displayed
parameters. This correction is part of the recipe and cache identity; it is not
the same as the general Gamma control in Light.

### Output & Soft Proof

Choose an output profile such as sRGB, Adobe RGB, Linear Rec709, Linear Rec2020,
Rec709, Linear ProPhoto RGB, PQ/HLG Rec2020, PQ/HLG P3, or Display P3. Choose
the output rendering intent, then select:

- **Proof off** — show the normal output profile.
- **Soft proof** — preview through a declared proof profile and proof intent.
- **Gamut warning** — show out-of-gamut regions using the declared proof state.

Black-point compensation is explicit. Studio never infers a monitor profile and
does not perform a separate QML color transform.

### White Balance

The RAW-aware modes are **As shot**, **Camera reference**, **As shot →
reference**, and **Manual coefficients**. Automatic modes use camera metadata
before demosaic. Manual mode exposes four coefficients: Red, Green, Blue, and
Fourth (G1/G2 or the relevant fourth CFA channel).

On Bayer RAW, enable **Pick white on photo** and click a neutral patch. Ravo
samples the CFA around that point and writes manual coefficients. Straighten
and Canvas must be off. JPEG/PNG/TIFF originals report an unsupported pick.

If the source does not provide the metadata required by an automatic mode, Ravo
reports the missing state rather than reverting to a generic Kelvin/tint
approximation. `ravo inspect` reports as-shot and camera-reference coefficients
for RAW. `ravo catalog develop --pick-white=x,y` uses the same sample path.

### Color Calibration

**Color Calibration** (the later Calibration section) exposes a 3×3
output-row-by-input-channel matrix in the linear sRGB D50 workspace. Keep the
diagonal at its identity values when no matrix mix is required. Camera
Calibration on the default grading path is the separate RGB-primaries editor.

### Light

Light controls include:

- **Exposure mode**: Manual or Deflicker.
- Exposure black and manual Exposure EV.
- Optional exposure-bias and highlight-preservation compensation.
- Deflicker percentile and target EV when Deflicker is selected.
- Contrast, Highlights, Shadows, Whites, Blacks, and Gamma.

### Curves

**Curves** is a first-class grading tool after Light:

- **RGB** (default) is a working-space RGB curve with linked RGB or
  independent Red/Green/Blue channels, a histogram behind the plot, and
  Monotonic / Centripetal / Cubic interpolation.
- Linked RGB also has parametric **Shadows / Darks / Lights / Highlights**
  sliders. Point curve is applied after the parametric map.
- **Tone** is the Lab/XYZ/RGB-linked luminance curve. Lab independent exposes
  Master, a, and b.
- Click to add a point (up to 20); drag to reshape; double-click or Delete
  removes an interior point; arrow keys nudge the selected point.

Compensate middle grey and region splits stay under **Curves · more**.

For RAW, the **Sigmoid Display · Standard SDR** group exposes Contrast, Skew,
and Preserve Hue. RAW uses Sigmoid as its default display transform; a default
RAW Sigmoid is not itself marked as a user edit.

Deflicker is a RAW analysis mode. It does not synthesize an automatic result for
unsupported raster analysis cases.

### Color Equalizer

**Color Equalizer** is the default eight-band hue partition (Red, Orange,
Yellow, Green, Aqua, Blue, Purple, Magenta). Choose Saturation, Hue, or
Lightness, then edit all eight bands. It is separate from Graduated ND;
bypassing one does not bypass the other.

### Camera Calibration

**Camera Calibration** adjusts working-space primaries: shadow-tint hue/purity
and red/green/blue hue and saturation. It is the same `ravo.color.primaries`
operation as before, placed on the default grading path.

### Color

The Color section currently exposes:

- Vibrance, Saturation, and Velvia.
- **Color Balance RGB · linear sRGB D50 / Filmlight Yrg**, with Shadows /
  Midtones / Highlights wheels for hue and chroma plus a luminance slider.
  Formula, global, and extra numeric fields stay under **Color Balance RGB ·
  more**.
- **Split Toning**, with independent shadow/highlight hue and saturation,
  balance pivot, midtone compression, and mix. It preserves HSL lightness while
  blending the selected endpoint outside the compressed midtone band. Imported
  canonical masks remain preserved/read-only in this panel.
- **Monochrome**, with a D50 Lab a*/b* virtual colour filter, filter size,
  highlight preservation, and mix. It uses a scale-aware bilateral base before
  neutral output; it is not a simple saturation slider. Imported canonical
  masks remain preserved/read-only in this panel.

**Color · Advanced** keeps the overlapping tools off the default path:

- A D50 Lab **Color look-up table** with eight built-in presets and direct source
  and target patch fields.
- **Color Balance · legacy Lab / ProPhoto RGB**, with Lift/Gamma/Gain or
  Slope/Offset/Power paths.
- **Color Correction · D50 Lab**, with highlight/shadow a*/b* endpoints and
  saturation.
- **Color contrast**, with independent a*/b* slopes and offsets plus an
  **Allow extended chroma** switch.
- **Color Harmonizer**, with an enable switch, ten harmony rules, anchor
  hue, pull strength, neutral protection, pull width, custom nodes 2–4,
  four custom hues, four node saturations, and smoothing from 0 through 2.
  The engine applies positive smoothing through its canonical working-image
  scale and private recursive path.
- **Color Reconstruction**, with an enable switch, none/saturated-color/hue
  precedence, highlight threshold, spatial and lightness range extents, and a
  hue selector when hue precedence is active. It propagates surrounding D50
  Lab colour through a full-frame bilateral grid immediately before Output
  Color. A non-proportional preview fails explicitly instead of using a
  tile-local approximation.
- **Color Zones**, an optional D50 Lab editor separate from the default Color
  Equalizer. Choose lightness, chroma, or hue as the selection axis; edit the
  current band's lightness/chroma/hue curves, choose cubic, Catmull–Rom, or
  monotone interpolation for each curve, and set mix strength. Imported custom
  2–20-node curves and masks remain visible/read-only in the eight-band Studio
  projection so ordinary edits do not reshape them.

The two Color Balance paths are separate operations. Ravo does not treat one as
an automatic fallback or alias for the other.

### Canonical masks for Color Harmonizer and Graduated ND

Both **Color Harmonizer** and **Graduated ND** include a **Mask**
editor. Choose None, All, Linear gradient, Circle, Ellipse, Parametric, Group,
Path, or Brush. The available controls change with the selected kind; parametric
thresholds remain ordered while you edit them. Selecting a non-None kind makes
that operation an explicit enabled Develop edit, including a Graduated ND with
density 0.

**Show mask overlay** tints the live preview with the selected operation's mask.
It is a view setting only: it is not saved with the recipe and does not change
export. Group attachments can add and combine children. Path and brush masks
expose ordered points plus, for a brush, radius, hardness, and density.

Some recipes can contain masks that Studio did not create, masks shared by two
operations, or group masks. Studio shows those attachments as read-only so an
ordinary control cannot alter the wider graph. You can still use **Detach
mask** to remove the attachment from the current operation without deleting
the external/shared graph. **Reset to all** keeps an editable attachment and
returns it to the All kind; use **Detach mask** when you want no attachment.
Detaching does not disable or reset the owning operation, because Studio cannot
infer whether that operation was already an explicit edit before the mask was
created. Use the operation's enable/reset control separately when needed.

### Detail and Effects

The current pane exposes:

- **Detail**: Sharpen, Radius, Masking, luminance/color denoise plus radius,
  Retouch, Clarity, and Grain. Sharpen applies a scale-aware separable unsharp
  mask to D50 Lab lightness only; Radius uses a 0–8 Studio working range while
  versioned recipes retain the full 0–99 source range. Masking is the existing
  sharpen threshold (0–100). Denoise is the accepted post-demosaic profile
  denoise and follows the Detail bypass lamp, including on JPEG. Borders and
  chroma remain unchanged by sharpen. Retouch adds ordered circle regions for
  Clone, Heal, Gaussian/Bilateral Blur, or Erase/Color Fill. Target position,
  radius/feather/opacity, clone/heal source position, blur radius, fill color,
  and fill brightness are explicit. Remove deletes that region; reset of the
  Detail section removes all Retouch regions.
- **Effects**: Vignette (signed amount, midpoint, feather, roundness, and
  centre), Bloom, Soften, Dehaze, Output Dither / Posterize, Output Frame, and
  Text Watermark. Positive vignette darkens the corners; negative lightens
  them. Highlight-priority, colour-priority, and paint-overlay vignette styles
  are not offered. Dehaze exposes Strength, Distance, and adaptive window
  scaling. It runs on source-linear RAW using dark-channel ambient/depth
  estimation and a guided transmission filter; encoded raster input returns an
  explicit unsupported result. Output Dither runs after Output Color and offers
  deterministic random noise, all supported Floyd–Steinberg bit-depth/gray/RGB
  modes, and 2–8-level posterization. Auto applies Floyd–Steinberg only to
  integer export targets; preview and float output are range-clipped without
  automatic diffusion. Random damping is in dB, and **Reset output dither**
  removes the explicit operation. Output Frame runs after Dither and exposes
  orientation, dimension basis, constant/image/custom aspect, size, horizontal
  and vertical image position, border colour, plus optional line size, offset,
  and colour. It is visible in preview and is part of JPEG, PNG, and TIFF
  export dimensions. Text Watermark runs after the frame and uses a portable
  fixed 5×7 font. Set its text, colour, opacity, short-side height, rotation,
  alignment, and offsets. `{stem}` and `{asset_id}` expand from the current
  recipe source; unsupported characters or tokens fail explicitly.

Use the reset button beside a control when you want to remove only that effect.

## Reuse edits as a Recipe Style

Use **File → Save Edits as Style…** to write the selected photo's complete
canonical edit state to `.rstyle.json`. The artifact includes operation order,
profiles, masks, Retouch regions, and section bypass state, but not the source
asset identity. Use **File → Apply Recipe Style…** on another selected photo to
replace its current edit recipe; the result enters normal history and can be
undone during the session.

Ravo validates the complete template before applying it. Legacy `.dtstyle`
files are not partially imported because their dynamic IOP parameters may not
have accepted Ravo equivalents.

The Edit left rail lists **Presets** above History. **Import…** copies a
Lightroom Classic `.xmp` or a Ravo `.rstyle.json` into a `Ravo Presets` folder
next to the open library, then applies it to the selected photo. Click a listed
preset to apply it again. **File → Import Preset…** is the same command.
Right-click a listed preset and choose **Copy Info** to copy its name, path,
kind, size, and SHA-256. Paste that block together with a photo **Copy Info**
block to identify the exact library photo and preset file.

**File → Apply Recipe Style…** also accepts a Lightroom Classic `.xmp` preset
(`crs:` Camera Raw settings). Ravo maps that look onto White Balance, Light,
Curves, Color Equalizer, Color, Camera Calibration, Detail, and Effects, then
commits through ordinary history. Crop, masks, Retouch, and profiles stay with
the destination photo. Adobe Standard is not loaded. Unknown CRS keys and
Kelvin/tint white balance fail instead of applying a partial look.

### RAW Repair / Denoise / Lens

For supported Bayer RAW inputs, this group includes:

- Hot pixels, threshold, and the permissive three-neighbor mode.
- RAW chromatic aberration iterations and **Avoid CA color shift**.
- Highlight reconstruction.
- Denoise.
- Manual lens distortion and lens vignetting controls.

RAW-only operations can return an explicit unsupported result for raster,
non-Bayer, X-Trans, or otherwise unsupported sources. They are not replaced by
a raster approximation.

### Tone equalizer and Graduated ND

**Tone equalizer** has five bands: Blacks, Shadows, Midtones, Highlights, and
Whites.

**Graduated ND** provides graduated density and rotation. A positive graduated
density follows the current gradient orientation; use the on-image crop/edit
surface and the control values together when checking the result. Color
Equalizer is a separate earlier section.

## Before and After

Press **Before / After** in the Edit pane or use the View compare command. The
image surface switches between the current edited result and the selected
photo's product baseline. It does not replace the stored recipe and does not
rewrite the preview cache as a new edit.

## Undo, redo, reset, and snapshots

- **Undo**, **Redo**, **Before / After**, **Reset all**, **Copy**, and **Paste**
  live on the left History rail. Undo and Redo are the session stack for
  Develop-panel commits, history restore, Original, snapshot restore, paste,
  and reset. Live slider or crop drags stay in memory until they are committed.
  Undo and Redo then save the resulting recipe through the same catalog
  transaction.
- Consecutive commits from the same control replace that control's latest
  ordinary History row, so repeated adjustments keep only the final value and
  one Undo returns to the value before the sequence. Changing controls, leaving
  Edit, selecting another photo, using Undo/Redo or History, creating a
  snapshot, or an intervening external edit starts a new row.
- **Copy Edits** (`Cmd/Ctrl+Shift+C`) stores the current complete develop
  recipe in a session clipboard. **Paste** (`Cmd/Ctrl+Alt+V`) applies that
  whole recipe to the active photo as a normal history step, including masks.
  **Paste Light** applies White Balance, Light, and Curves. **Paste Color** applies
  Color, Color Equalizer, and Camera Calibration and keeps the destination
  photo's mask attachments. The clipboard is not a file and is not the system pasteboard.
  Style save/apply remains the portable artifact.
- A control reset changes one field back to its default.
- A mask-control reset keeps its attached editable mask. **Reset to all**
  changes that mask to All, while **Detach mask** removes only the current
  operation's attachment. Resetting one parametric threshold restores the
  complete canonical four-threshold ramp so it remains ordered.
- A section reset clears the fields owned by that section.
- **Reset all** returns the photo to the product baseline. RAW keeps its default
  Sigmoid display behavior.
- **Snapshot** stores the current recipe into the separate Snapshots list as
  `Snapshot 1`, `Snapshot 2`, and so on. Double-click a snapshot name to rename
  it. History keeps only sequential edits. The bottom row is **Original**; click
  it to restore the product baseline. Click any later edit or a snapshot to
  restore that recipe as the current edit.

## Result

- The current edit is visible in the preview and marked as **Edited** in the
  photo information and thumbnail when it differs from the baseline.
- Reopening the catalog restores the recipe and history.
- Export uses the committed recipe, not a stale interactive slider preview.

## Common questions

### Why did the preview briefly show Loading?

Develop previews are asynchronous. A newer request cancels or supersedes an
older one, and the UI keeps the newest accepted result.

### Why did an automatic white-balance or profile control fail?

Those modes require a valid camera/profile contract. Ravo reports missing,
corrupt, singular, or unsupported state instead of silently choosing a generic
fallback.

### Does Reset all restore the original file?

No. It removes the stored Develop changes and returns to the Ravo baseline. The
original file was never modified by normal editing.

### Why is a control visible but unavailable for my file?

Some controls are media-specific, especially RAW repair, camera metadata, and
profile-dependent transforms. Availability is evaluated by the current engine
contract, not by converting the file to a different hidden path.
