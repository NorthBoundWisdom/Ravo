# Frozen Darktable 0.9 (reference only)

This tree is the retired DarktableNext 0.9 application. Ravo may read it as a
behavioural oracle. Do not configure, compile, run, or test anything here.

| Path | Contents |
| --- | --- |
| `src/` | Former application C/C++ sources (GTK, IOP, pixelpipe) |
| `host/` | Former root CMake graph, `cmake/`, `data/`, `packaging/`, and host tools |
| `tests/` | Former `darktable-tests` fixtures consumed by Ravo as read-only assets |
| `docs/` | 0.9 source maps (IOP/GTK/pixelpipe). Live Ravo docs stay in `DevDocs/` and `Ravo/docs/` |
| `benchmarks/` | Historical 0.9 CPU/OpenCL measurement scripts |

The live CMake project is the repository-root `CMakeLists.txt`, which builds
only `Ravo/`. Deleting this `legacy/` directory must not be required for a Ravo
configure/build/test cycle except for tests that still read `legacy/tests`
fixtures.
