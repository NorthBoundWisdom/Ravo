# Synthetic video fixtures

These clips contain only generated lavfi patterns and a sine wave. Ravo
contributors dedicate the fixture media to the public domain under CC0-1.0.
There are no camera recordings, personal files or external images.

Regenerate only with `python3 Ravo/tools/generate_video_fixtures.py`. The
generator owns the clips and `manifest.json`, which records its FFmpeg version
and SHA-256 values. SDR/AAC, rotated MOV and HEVC Main10 HLG/PQ exercise the
codec and presentation contracts; they are not a real-camera quality corpus.

`auxiliary_audio.mov` combines playable stereo AAC with a synthetic unknown
`apac` sample entry, a type-1 cover and six channel descriptions for stereo.
`unsupported_audio.mov` has only unknown audio entries. These are parser/policy
fixtures, not encoded APAC samples. `--metadata-only` regenerates these two
and `full_range.mov` without re-encoding the four base clips. The full-range
H.264 clip is uniform RGB 64/255 gray and checks explicit YUV range handling.

`p3_bt601_limited.mov` and `p3_bt601_full.mov` encode uniform nonlinear RGB
(144, 64, 128)/255 with SMPTE 170M matrix, P3 D65 primaries and BT.709 transfer.
`p3_bt470bg.mov` uses the equivalent BT.470BG matrix tag.
`unsupported_matrix.mov` uses YCgCo to ensure unknown conversions are rejected.
`prores_p3.mov` encodes the same colour as 10-bit 4:2:2 ProRes HQ, including
per-frame colour tags that stream probing can omit.
`--colour-only` regenerates these five clips without changing the other fixtures.
