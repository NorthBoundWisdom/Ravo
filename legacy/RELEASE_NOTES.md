# DarkTableNext 0.9.0 Release Notes

DarkTableNext 0.9.0 establishes the project's frozen reference baseline.
It keeps the proven photo-processing core and GTK interface while narrowing the
product to a focused desktop workflow.

## Highlights

- macOS (Apple Silicon and Intel), Windows, and Linux are maintained build
  targets.
- CMake is the supported build entry point.
- FreeCM manages pinned third-party source roots through
  `source_roots.lock.jsonc.in`.
- The application retains local import and cataloguing, Lighttable, Darkroom,
  non-destructive editing, colour management, history, masks, and local-disk
  export.
- The existing CPU image pipeline remains the correctness reference. Its
  OpenCL implementation remains frozen and is not the basis of Ravo GPU work.

## Scope reduction

This baseline intentionally removes historical functionality that is outside
the current product boundary, including Lua support, slideshow, printing, map,
tethering, MIDI and game-controller input, email and Piwigo export, and AI or
ONNX features. Local export is limited to JPEG, PNG, TIFF, and copying the
original file.

Support for removed plugins, scripting interfaces, formats, packaging paths,
and legacy UI behaviour is not provided.

## Development status

Version 0.9.0 is a frozen baseline, not a general-availability release. No
further feature reduction, IOP cleanup, architecture work, or Metal migration
is planned in 0.9. Those unfinished goals now belong to Ravo: first its
CPU-first headless engine and CLI, then product services and independently
designed GPU adapters. The old OpenCL path remains unchanged until the entire
0.9 application is retired.

See [the README](README.md) for build instructions and
[the frozen 0.9 and Ravo implementation plan](TODO_REWRITE.md) for the product
boundary and roadmap.
