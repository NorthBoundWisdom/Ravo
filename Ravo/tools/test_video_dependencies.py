"""Exercise the production FFmpeg CMake resolver against isolated Windows kits."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPONENTS = {"avformat": 61, "avcodec": 61, "avutil": 59, "swscale": 8, "swresample": 5}


class VideoDependencyTests(unittest.TestCase):
    def configure(self, import_directory: str, missing: bool = False) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory(prefix="ravo-video-kit-") as temp:
            base = Path(temp)
            kit = base / "kit with spaces"
            for directory in (kit / "bin", kit / "lib", base / "build/_deps/ffmpeg-headers/libavutil"):
                directory.mkdir(parents=True)
            (base / "build/_deps/ffmpeg-headers/libavutil/avconfig.h").touch()
            for name, major in COMPONENTS.items():
                (kit / "bin" / f"{name}-{major}.dll").touch()
                if not (missing and name == "avformat"):
                    (kit / import_directory / f"{name}.lib").touch()
            source = f'''cmake_minimum_required(VERSION 3.26)
project(VideoKitContract LANGUAGES NONE)
set(WIN32 TRUE)
set(CMAKE_FIND_LIBRARY_PREFIXES "")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
set(RAVO_FFMPEG_RUNTIME_ROOT "{kit.as_posix()}")
function(ravo_require_migration_source_root)
endfunction()
include("{(ROOT / 'Ravo/cmake/RavoVideoDependencies.cmake').as_posix()}")
foreach(component IN ITEMS avformat avcodec avutil swscale swresample)
  get_target_property(location ravo_ffmpeg_${{component}} IMPORTED_IMPLIB)
  if(NOT location STREQUAL "{kit.as_posix()}/{import_directory}/${{component}}.lib")
    message(FATAL_ERROR "Import library escaped explicit runtime kit: ${{location}}")
  endif()
endforeach()
'''
            (base / "CMakeLists.txt").write_text(source, encoding="utf-8")
            return subprocess.run(["cmake", "-S", str(base), "-B", str(base / "build")],
                                  text=True, capture_output=True, check=False)

    def test_qt_windows_import_libraries_beside_dlls(self):
        result = self.configure("bin")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_explicit_runtime_lib_directory(self):
        result = self.configure("lib")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_import_library_remains_fatal(self):
        result = self.configure("bin", missing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not find _ravo_import", result.stderr)


if __name__ == "__main__":
    unittest.main()
