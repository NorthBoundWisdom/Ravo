#!/usr/bin/env python3
"""Own the synthetic, redistributable video contract fixtures (no camera media)."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import struct
import tempfile

def box(kind, payload):
    return struct.pack(">I4s", len(payload) + 8, kind) + payload

def auxiliary_metadata(data, unsupported_only=False):
    """Synthetic MOV container edge cases, not APAC-encoded audio.

    Replace the supplemental AAC sample entry and its codec configuration with
    an unknown apac entry. Keep mdat/offsets intact (moov must follow mdat).
    Add a type-1 cover and six channel descriptions to stereo primary AAC.
    """
    audio = 0
    def rewrite(data):
        nonlocal audio
        output = bytearray()
        offset = 0
        while offset < len(data):
            size, kind = struct.unpack_from(">I4s", data, offset)
            if size < 8 or offset + size > len(data):
                raise ValueError("Malformed synthetic MOV box")
            payload = data[offset + 8:offset + size]
            if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta"):
                payload = rewrite(payload)
            if kind == b"stsd" and payload[12:16] == b"mp4a":
                audio += 1
                version = struct.unpack_from(">H", payload, 24)[0]
                header_size = 36 + (16 if version == 1 else 0)
                entry = bytearray(payload[8:8 + header_size])
                if unsupported_only or audio == 2:
                    entry[4:8] = b"apac"
                    children = box(b"free", b"synthetic unsupported auxiliary audio")
                else:
                    descriptions = b"".join(struct.pack(">IIfff", label, 0, 0, 0, 0)
                                            for label in range(1, 7))
                    children = payload[8 + header_size:] + box(b"chan", bytes(4) + struct.pack(">III", 0, 0, 6) + descriptions)
                entry[0:4] = struct.pack(">I", len(entry) + len(children))
                payload = payload[:8] + entry + children
            if kind == b"udta":
                cover = box(b"data", struct.pack(">II", 1, 0) + b"synthetic unknown cover")
                handler = box(b"hdlr", bytes(8) + b"mdir" + bytes(13))
                payload += box(b"meta", bytes(4) + handler + box(b"ilst", box(b"covr", cover)))
            output += box(kind, payload)
            offset += size
        return bytes(output)
    result = rewrite(data)
    if audio != 2 or data.find(b"mdat") > data.find(b"moov"):
        raise ValueError("Expected two audio tracks and trailing moov")
    return result

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--check", action="store_true", help="Verify committed hashes without regenerating")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--metadata-only", action="store_true", help="Regenerate additional metadata/range/colour fixtures, preserving the four base clips")
    group.add_argument("--colour-only", action="store_true", help="Regenerate only colour-matrix fixtures")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "tests/fixtures/video"
    names = ("sdr_audio.mp4", "rotated.mov", "hlg.mp4", "pq.mp4", "auxiliary_audio.mov", "unsupported_audio.mov", "full_range.mov",
             "p3_bt601_limited.mov", "p3_bt601_full.mov", "p3_bt470bg.mov", "unsupported_matrix.mov", "prores_p3.mov")
    if args.check:
        manifest = json.loads((root / "manifest.json").read_text())
        if set(manifest["files"]) != set(names):
            raise ValueError("Video fixture inventory does not match the generator")
        for name in names:
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != manifest["files"][name]:
                raise ValueError(f"Video fixture hash mismatch: {name}")
        print("video fixture hashes verified")
        return
    root.mkdir(parents=True, exist_ok=True)
    def run(*command):
        subprocess.run([args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", *command], check=True)
    if not args.metadata_only and not args.colour_only:
        generate_base(root, run)
    if not args.colour_only:
        generate_metadata(root, run)
    for name, pixel_format, colour_range, full_range in (
        ("p3_bt601_limited.mov", "yuv420p", "tv", 0),
        ("p3_bt601_full.mov", "yuvj420p", "pc", 1)):
        run("-f", "lavfi", "-i", "color=c=0x904080:size=96x64:rate=10:duration=1",
            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", pixel_format,
            "-color_range", colour_range, "-color_primaries", "smpte432", "-color_trc", "bt709",
            "-colorspace", "smpte170m", "-bsf:v",
            f"h264_metadata=colour_primaries=12:transfer_characteristics=1:matrix_coefficients=6:video_full_range_flag={full_range}",
            str(root / name))
    for name, matrix, code in (("p3_bt470bg.mov", "bt470bg", 5),
                               ("unsupported_matrix.mov", "ycgco", 8)):
        run("-i", str(root / "p3_bt601_limited.mov"), "-c", "copy", "-colorspace", matrix,
            "-bsf:v", f"h264_metadata=matrix_coefficients={code}", str(root / name))
    run("-f", "lavfi", "-i", "color=c=0x904080:size=96x64:rate=10:duration=1",
        "-vf", "setparams=color_primaries=smpte432:color_trc=bt709:colorspace=smpte170m",
        "-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le",
        "-color_primaries", "smpte432", "-color_trc", "bt709", "-colorspace", "smpte170m",
        str(root / "prores_p3.mov"))
    manifest = {"provenance": "FFmpeg lavfi synthetic patterns and sine; generated by generate_video_fixtures.py; apac entries are synthetic unsupported-track metadata, not APAC audio",
        "generator_version": subprocess.check_output([args.ffmpeg, "-version"], text=True).splitlines()[0],
        "license": "CC0-1.0", "files": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in sorted(names)}}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

def generate_metadata(root, run):
    run("-f", "lavfi", "-i", "color=c=0x404040:size=96x64:rate=10:duration=1",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuvj420p",
        "-color_range", "pc", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-colorspace", "bt709", str(root / "full_range.mov"))
    with tempfile.TemporaryDirectory(prefix="ravo-video-fixtures-") as temporary:
        base = Path(temporary) / "stereo.mov"
        run("-i", str(root / "sdr_audio.mp4"), "-map", "0:v", "-map", "0:a", "-map", "0:a",
            "-c:v", "copy", "-c:a", "aac", "-ac", "2", "-disposition:a:0", "default",
            "-disposition:a:1", "0", str(base))
        data = base.read_bytes()
        (root / "auxiliary_audio.mov").write_bytes(auxiliary_metadata(data))
        (root / "unsupported_audio.mov").write_bytes(auxiliary_metadata(data, unsupported_only=True))
def generate_base(root, run):
    run("-f", "lavfi", "-i", "testsrc2=size=96x64:rate=10:duration=1",
        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=1",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
        "-c:a", "aac", "-metadata", "creation_time=2026-01-02T03:04:05Z",
        "-movflags", "+faststart", str(root / "sdr_audio.mp4"))
    run("-display_rotation:v:0", "90", "-i", str(root / "sdr_audio.mp4"), "-c", "copy", str(root / "rotated.mov"))
    for name, transfer, transfer_id in (("hlg", "arib-std-b67", 18), ("pq", "smpte2084", 16)):
        run("-f", "lavfi", "-i", "color=c=gray:size=96x64:rate=10:duration=1",
            "-c:v", "libx265", "-preset", "ultrafast", "-pix_fmt", "yuv420p10le",
            "-x265-params", f"log-level=error:pools=1:frame-threads=1:colorprim=9:transfer={transfer_id}:colormatrix=9",
            "-color_primaries", "bt2020", "-color_trc", transfer, "-colorspace", "bt2020nc",
            "-bsf:v", f"hevc_metadata=colour_primaries=9:transfer_characteristics={transfer_id}:matrix_coefficients=9",
            "-tag:v", "hvc1", str(root / f"{name}.mp4"))

if __name__ == "__main__":
    main()
