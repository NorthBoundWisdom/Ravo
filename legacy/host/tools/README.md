# Build-generation tools

`tools/` is intentionally a build-input whitelist. Every file here is consumed
by a current CMake target; it is not a home for release, packaging, migration,
or one-off maintenance scripts.

| Input | CMake consumer | Output |
| --- | --- | --- |
| `create_version_c.sh` | root `version` target | generated `version_gen.c` |
| `generate_darktablerc.xsl` | `data/darktablerc_file` | default `darktablerc` |
| `generate_darktablerc_conf.xsl`, `generate_prefs.xsl` | `src` configuration targets | generated preference/configuration headers |
| `generate_styles_string.sh` | `src/generate_styles_string` | generated styles header |
| `introspection/` | `src/iop` introspection target | generated IOP parameter metadata |

Changes to these inputs require a CMake configure and build verification. New
standalone tooling belongs outside the application source tree unless a CMake
target consumes it.
