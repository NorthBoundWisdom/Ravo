# ADR-0165: Video library import, SDR presentation and playback

- Status: Accepted by explicit user request
- Date: 2026-10-09

Ravo admits MOV/MP4/M4V H.264/HEVC/standard ProRes assets, including independent Live Photo
MOV files. Video assets support ordinary Add/Copy/Move, catalog review,
thumbnails, original-byte export and Studio playback with sound. Video Develop,
transcoding, Live Photo association and native HDR output are outside this scope.

Domain owns versioned video metadata and bounded immutable frame values.
The private FFmpeg adapter reads local media on managed service workers.
FFmpeg 7.1.5 source headers are pinned by FreeCM; the selected Qt 6.11.2 kit
owns the matching shared runtime used by both this adapter and Qt Multimedia.
Configuration and runtime identity checks reject mismatches. There is no
host-library search fallback or external ffmpeg/ffprobe subprocess dependency.

The 2026-10-10 runtime qualification amendment admits the selected Qt Windows
kit's `bin` import-library layout and declares PipeWire/VA-API host dependencies
for Linux; no runtime search outside the explicit kit or diagnostic suppression
is added. Qt 6.11.2 software playback normalizes YUVJ420P to limited-range
YUV420P while dropping colour tags. The desktop adapter interprets this
unlabelled normalized buffer as limited range, while explicit frame range and
hardware NV12 retain their declared semantics. This follows the
[Qt buffer conversion](https://github.com/qt/qtmultimedia/blob/v6.11.2/src/plugins/multimedia/ffmpeg/qffmpegvideobuffer.cpp)
contract. Forced software and ordinary playback must both match poster pixels
within the existing three-code-value bound; the test tolerance is unchanged.

Engine owns CPU HLG/PQ-to-SDR presentation. Services own import, thumbnail
publication and identity validation. Desktop C++ owns QMediaPlayer, audio,
selection-bound playback and resource lifetime; QML displays frames and forwards
intents only. Work and decode queues are bounded. Cancellation, replacement and
window close invalidate late results and release their owners.

Schema 18 stores video metadata separately. Video assets do not acquire a photo
recipe. Existing photo catalogs migrate transactionally. Ingest transports keep
their source-preserving restrictions; ordinary folder Move reuses the existing
copy/verify/catalog/cleanup contract.

Unsupported streams, colour interpretations, missing codecs, audio devices,
corrupt inputs, timeouts and resource exhaustion return explicit reasons.
An unsupported auxiliary audio stream does not disqualify video with a supported
AAC/PCM track. Playback selects supported audio; stable warnings explicitly
record unsupported auxiliary audio, skipped unknown covers and capped excess
channel descriptions. No supported audio track remains an explicit rejection.
HDR previews are SDR and preserve original bytes. Dolby Vision support is
limited to independently decodable PQ/HLG-compatible base layers with that
limitation reported; incompatible profiles reject.
SDR BT.601/SMPTE 170M or BT.470BG matrices and P3 D65 primaries are admitted
independently from transfer tags. Poster and playback share the matrix mapping
and Engine gamut conversion; unknown matrices remain explicit rejections.
ProRes frame-header colour tags are resolved through the existing bounded
decoder owner. ProRes RAW remains unsupported.

Acceptance requires CLI/service tests for metadata, frame artifacts, import
atomicity, source hashes, migration/reopen/backup/restore and cancellation;
desktop tests for playback state, seeking and destruction; owned-pixel HDR
fixtures and isolated package/runtime checks on each claimed platform.
