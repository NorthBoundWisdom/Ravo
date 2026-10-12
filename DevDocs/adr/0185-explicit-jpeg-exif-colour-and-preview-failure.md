# ADR-0185: Honour explicit JPEG EXIF colour and settle failed previews

- Status: Accepted
- Date: 2026-10-12

## Context

A JPEG can declare sRGB through EXIF ColorSpace (0xA001, SHORT, count 1,
value 1) without an APP2 ICC profile. Treating that declaration as missing
rejects a supported source. The [CIPA EXIF specification](https://www.cipa.jp/std/documents/e/DC-X008-Translation-2019-E.pdf)
distinguishes sRGB from uncalibrated (65535); this is source metadata, not a
default for untagged pixels.

A progressive Develop failure also retained its loading flag even though no
settled follow-up had been queued.

## Decision

- Extend ADR-0018/0023 in the existing private JPEG adapter. Read the bounded
  APP1 EXIF TIFF header, IFD0 Exif pointer and ColorSpace entry in either byte
  order. Follow no IFD chains or thumbnail pointers. Entry offsets, types,
  counts and directory bounds are validated before reads. Malformed or
  conflicting declarations return structured JPEG validation errors.
- A valid embedded RGB ICC profile remains authoritative. Without ICC, only
  EXIF ColorSpace 1 publishes built-in `srgb`. Missing, uncalibrated and reserved
  values remain missing. A corrupt ICC is never replaced by the EXIF profile.
- File and memory decode share that parser. Cancellation remains in the
  bounded marker scan and pixel-row owner. Source bytes, recipe schema,
  catalog schema, dependencies and thread ownership do not change.
- A current failed Develop preview clears loading and publishes the existing
  error. A settled follow-up owns loading only after a successful progressive
  result. Existing revision/session guards protect newer work.

## Validation and consequences

JPEG adapter tests cover both byte orders, file/memory equality, Engine render,
ICC precedence, unknown/uncalibrated values, malformed types/counts/offsets,
conflicting EXIF and corrupt ICC, with unchanged source hashes. The presenter
test verifies failed first-frame loading completion, unchanged source-profile
recipe, and recovery through an explicit ordinary input-profile edit.

The Engine still rejects untagged source-profile rendering. There is no decoder
fallback, automatic recipe rewrite, source-image mutation or separate renderer.

## Rejected alternatives

Assuming sRGB for every missing profile would violate ADR-0006/0018. Retrying a
failed preview or hiding its error would leave ownership and colour ambiguous.
