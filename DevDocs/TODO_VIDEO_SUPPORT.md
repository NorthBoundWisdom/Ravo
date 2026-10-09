# Video support qualification

Ownership and implemented contracts belong in
[ADR-0165](adr/0165-video-library-and-playback.md),
[ARCHITECTURE.md](ARCHITECTURE.md) and [TESTING.md](TESTING.md).

- Validate real iPhone SDR, HLG/PQ and compatible Dolby Vision base layers,
  including long recordings, variable frame rate, rotation and interrupted files.
- Measure native-resolution decode and 1600-edge playback latency, dropped
  frames and sustained A/V synchronization on claimed hosts; tiny synthetic
  clips do not establish 4K/60 real-time playback performance.
- Verify audible output, device removal and recovery on native desktop sessions.
  Muted/offscreen frame delivery is not an audible test.
- Run Windows/MSVC and Linux builds and isolated package probes with shared
  FFmpeg 7.1.5. A different Qt-kit version does not permit disabling identity checks.
  Include standard ProRes decoder availability and 4:2:2/10-bit mapped playback.
- Validate display ICC moves and HDR-to-SDR appearance on the real corpus;
  retain explicit unsupported states for other Dolby Vision profiles/transforms.

Acceptance commands:

```text
python3 configs/source_roots.py verify
cmake --preset mac_clang_debug -DBUILD_TESTING=ON
cmake --build --preset mac_clang_debug --target ravo_catalog_tests ravo_desktop_command_tests RavoCodeQuality
ctest --test-dir build/mac_clang_debug --output-on-failure
cmake --build --preset mac_clang_debug --target RavoPackage
python3 Ravo/tools/check_packaged_runtime.py <artifact> --require-smoke
```
