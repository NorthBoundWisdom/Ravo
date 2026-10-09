"""Exercise Windows FFmpeg linkage generation with isolated CMake/tool fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = {"avformat": 61, "avcodec": 61, "avutil": 59, "swscale": 8, "swresample": 5}


@unittest.skipIf(os.name == "nt", "Mock tool executables use POSIX shebangs; real MSVC runs in build CI")
class VideoDependencyTests(unittest.TestCase):
    def configure(self, failure: str = "", missing: bool = False) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory(prefix="ravo-video-kit-") as temp:
            base = Path(temp)
            kit = base / "kit with spaces"
            tools = base / "tools"
            for directory in (kit / "bin", tools, base / "build/_deps/ffmpeg-headers/libavutil"):
                directory.mkdir(parents=True)
            (base / "build/_deps/ffmpeg-headers/libavutil/avconfig.h").touch()
            for name, major in COMPONENTS.items():
                if not (missing and name == "avformat"):
                    (kit / "bin" / f"{name}-{major}.dll").write_bytes(b"fixture DLL identity")
            dumpbin = tools / "dumpbin"
            dumpbin.write_text(
                f"#!{sys.executable}\n"
                "from pathlib import Path\nimport sys\n"
                f"mode = {failure!r}\n"
                "if mode == 'dumpbin': sys.exit(7)\n"
                "print(' ordinal hint RVA      name')\n"
                "if mode != 'exports':\n"
                " name = Path(sys.argv[-1]).stem.split('-')[0]\n"
                " print('       1    0 00001000 ' + name + '_version')\n"
                " print('       2    1 00001020 ' + name + '_configuration')\n",
                encoding="utf-8")
            librarian = tools / "lib"
            librarian.write_text(
                f"#!{sys.executable}\n"
                "from pathlib import Path\nimport sys\n"
                f"mode = {failure!r}\n"
                "if mode == 'lib': sys.exit(9)\n"
                "output = next(x[5:] for x in sys.argv[1:] if x.startswith('/out:'))\n"
                "definition = next(x[5:] for x in sys.argv[1:] if x.startswith('/def:'))\n"
                "assert 'EXPORTS' in Path(definition).read_text()\n"
                "Path(output).write_bytes(b'fixture import library')\n",
                encoding="utf-8")
            dumpbin.chmod(0o755)
            librarian.chmod(0o755)
            source = f'''cmake_minimum_required(VERSION 3.26)
project(VideoKitContract LANGUAGES NONE)
set(WIN32 TRUE)
set(MSVC TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_AR "{librarian.as_posix()}")
set(CMAKE_PROGRAM_PATH "{tools.as_posix()}")
set(RAVO_FFMPEG_RUNTIME_ROOT "{kit.as_posix()}")
function(ravo_require_migration_source_root)
endfunction()
include("{(ROOT / 'Ravo/cmake/RavoVideoDependencies.cmake').as_posix()}")
foreach(component IN ITEMS avformat avcodec avutil swscale swresample)
  get_target_property(location ravo_ffmpeg_${{component}} IMPORTED_IMPLIB)
  if(NOT location STREQUAL "${{CMAKE_BINARY_DIR}}/_deps/ffmpeg-import/${{component}}.lib")
    message(FATAL_ERROR "Import library escaped build output: ${{location}}")
  endif()
  file(READ "${{CMAKE_BINARY_DIR}}/_deps/ffmpeg-import/${{component}}.def" definition)
  if(NOT definition MATCHES "${{component}}_version")
    message(FATAL_ERROR "DLL exports were not preserved")
  endif()
endforeach()
'''
            (base / "CMakeLists.txt").write_text(source, encoding="utf-8")
            return subprocess.run(["cmake", "-S", str(base), "-B", str(base / "build")],
                                  text=True, capture_output=True, check=False)

    def test_official_qt_dll_only_layout_generates_build_owned_imports(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_runtime_remains_fatal(self):
        result = self.configure(missing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not find _ravo_runtime", result.stderr)

    def test_empty_exports_remain_fatal(self):
        result = self.configure("exports")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no named C exports", result.stderr)

    def test_export_tool_failure_remains_fatal(self):
        result = self.configure("dumpbin")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot read FFmpeg DLL exports", result.stderr)

    def test_librarian_failure_remains_fatal(self):
        result = self.configure("lib")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot generate FFmpeg MSVC import library", result.stderr)


if __name__ == "__main__":
    unittest.main()
