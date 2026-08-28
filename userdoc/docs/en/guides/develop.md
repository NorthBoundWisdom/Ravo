# Develop

## Goal

Make non-destructive edits to a selected photo, inspect their effect, and keep
or recover the result through the catalog recipe and history.

**Last verified:** 2026-08-28 against the current Develop recipe and Studio
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
engine path, so an edit shown in Studio is the edit sent to local export.

Interactive slider movement can show a lower-size preview while the control is
being moved. A committed change is saved atomically and then a normal preview
is refreshed. If several requests are made quickly, superseded previews are
cancelled or discarded; a late result must not replace the newest edit.

## Edit pane controls

The current Edit pane groups controls by task. Every numeric control has a reset
action; each section can also be reset from its section menu.

### Geometry

- Rotate left or right by 90 degrees.
- Flip horizontally or vertically.
- Enter **Crop & Rotate** mode.
- Choose a free, 1:1, 3:2, 4:3, 5:4, or 16:9 crop aspect.
- Drag the crop frame to crop.
- Drag outside the crop frame, or use Option/Alt-drag, to straighten.
- Set the straighten angle between −45 and +45 degrees.

Crop and straighten values are normalized recipe coordinates. The crop overlay
is a preview aid; commit the frame to store the change.

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

If the source does not provide the metadata required by an automatic mode, Ravo
reports the missing state rather than reverting to a generic Kelvin/tint
approximation.

### Color Calibration and RGB Primaries

**Color Calibration** exposes a 3×3 output-row-by-input-channel matrix in the
linear sRGB D50 workspace. Keep the diagonal at its identity values when no
calibration is required.

**RGB Primaries** exposes hue and purity for red, green, and blue, plus achromatic
tint hue and purity. Hue is shown in degrees in Studio and stored in the
versioned recipe.

### Light

Light controls include:

- **Exposure mode**: Manual or Deflicker.
- Exposure black and manual Exposure EV.
- Optional exposure-bias and highlight-preservation compensation.
- Deflicker percentile and target EV when Deflicker is selected.
- Contrast, Highlights, Shadows, Whites, Blacks, and Gamma.
- A draggable monotone Tone Curve. Click to add a point; double-click an
  interior point to remove it; use **Reset curve** to return to identity.

For RAW, the **Sigmoid Display · Standard SDR** group exposes Contrast, Skew,
and Preserve Hue. RAW uses Sigmoid as its default display transform; a default
RAW Sigmoid is not itself marked as a user edit.

Deflicker is a RAW analysis mode. It does not synthesize an automatic result for
unsupported raster analysis cases.

### Color

The Color section currently exposes:

- Vibrance, Saturation, and Velvia.
- A D50 Lab **Color look-up table** with eight built-in presets and direct source
  and target patch fields.
- **Color Balance · legacy Lab / ProPhoto RGB**, with Lift/Gamma/Gain or
  Slope/Offset/Power paths.
- **Color Balance RGB · linear sRGB D50 / Filmlight Yrg**, with darktable UCS
  (2022) or JzAzBz (2021) formula selection and global/shadow/midtone/highlight
  grading controls.
- **Color Correction · D50 Lab**, with highlight/shadow a*/b* endpoints and
  saturation.
- **Color contrast**, with independent a*/b* slopes and offsets plus an
  **Allow extended chroma** switch.
- **Color Harmonizer**, with an enable switch, ten harmony rules, anchor
  hue, pull strength, neutral protection, pull width, custom nodes 2–4,
  four custom hues, four node saturations, and smoothing from 0 through 2.
  The engine applies positive smoothing through its canonical working-image
  scale and private recursive path.
- Monochrome and split-toning controls for amount, shadow/highlight hue, and
  balance.

The two Color Balance paths are separate operations. Ravo does not treat one as
an automatic fallback or alias for the other.

### Canonical masks for Color Harmonizer and Graduated ND

Both **Color Harmonizer** and **Graduated ND / Color EQ** include a **Mask**
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

- **Detail**: Sharpen, Radius, Clarity, and Grain.
- **Effects**: Vignette, Bloom, Soften, and Dehaze.

Use the reset button beside a control when you want to remove only that effect.

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

### Tone equalizer and Graduated ND / Color EQ

**Tone equalizer** has five bands: Blacks, Shadows, Midtones, Highlights, and
Whites.

**Graduated ND / Color EQ** provides graduated density and rotation, eight color
equalizer bands, and band saturation/hue. A positive graduated density follows
the current gradient orientation; use the on-image crop/edit surface and the
control values together when checking the result.

## Before and After

Press **Before / After** in the Edit pane or use the View compare command. The
image surface switches between the current edited result and the selected
photo's product baseline. It does not replace the stored recipe and does not
rewrite the preview cache as a new edit.

## Undo, redo, reset, and snapshots

- **Undo** and **Redo** operate on the current Studio editing session and save
  the resulting recipe state through the same catalog transaction.
- A control reset changes one field back to its default.
- A mask-control reset keeps its attached editable mask. **Reset to all**
  changes that mask to All, while **Detach mask** removes only the current
  operation's attachment. Resetting one parametric threshold restores the
  complete canonical four-threshold ramp so it remains ordered.
- A section reset clears the fields owned by that section.
- **Reset all** returns the photo to the product baseline. RAW keeps its default
  Sigmoid display behavior.
- **Snapshot** stores the current recipe with a label.
- The Photo panel lists history and snapshots. Choose **Restore** beside an
  entry to make that recipe the current edit.

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
