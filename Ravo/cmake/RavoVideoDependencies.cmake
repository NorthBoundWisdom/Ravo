# Public FFmpeg headers come from the pinned source root. Runtime ownership is
# the selected Qt kit, shared by the private decoder and Qt Multimedia backend.
ravo_require_migration_source_root(RAVO_FFMPEG_SOURCE_ROOT FFMPEG_SOURCE_ROOT libavformat/avformat.h)
get_filename_component(_ravo_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
set(RAVO_FFMPEG_RUNTIME_ROOT "${_ravo_qt_prefix}" CACHE PATH
    "Qt-compatible FFmpeg runtime prefix (no system-library fallback)")
set(_ravo_ffmpeg_headers "${CMAKE_BINARY_DIR}/_deps/ffmpeg-headers")
file(MAKE_DIRECTORY "${_ravo_ffmpeg_headers}")
find_program(_ravo_ffmpeg_shell NAMES bash sh REQUIRED)
set(_ravo_ffmpeg_configure_args --disable-all --disable-autodetect --disable-programs
    --disable-doc --disable-asm --disable-network)
if(MSVC)
  list(APPEND _ravo_ffmpeg_configure_args --toolchain=msvc --cc=cl)
else()
  list(APPEND _ravo_ffmpeg_configure_args "--cc=${CMAKE_C_COMPILER}")
endif()
if(NOT EXISTS "${_ravo_ffmpeg_headers}/libavutil/avconfig.h")
  execute_process(COMMAND "${_ravo_ffmpeg_shell}" "${RAVO_FFMPEG_SOURCE_ROOT}/configure"
    ${_ravo_ffmpeg_configure_args} WORKING_DIRECTORY "${_ravo_ffmpeg_headers}"
    RESULT_VARIABLE _ravo_ffmpeg_result OUTPUT_VARIABLE _ravo_ffmpeg_output
    ERROR_VARIABLE _ravo_ffmpeg_error)
  if(NOT _ravo_ffmpeg_result EQUAL 0)
    message(FATAL_ERROR "FFmpeg header configuration failed: ${_ravo_ffmpeg_output}\n${_ravo_ffmpeg_error}")
  endif()
endif()
foreach(_ravo_spec IN ITEMS "avformat;61" "avcodec;61" "avutil;59" "swscale;8" "swresample;5")
  list(GET _ravo_spec 0 _ravo_lib)
  list(GET _ravo_spec 1 _ravo_major)
  unset(_ravo_runtime CACHE)
  if(WIN32)
    find_file(_ravo_runtime NAMES "${_ravo_lib}-${_ravo_major}.dll"
      PATHS "${RAVO_FFMPEG_RUNTIME_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
    unset(_ravo_import CACHE)
    find_library(_ravo_import NAMES "${_ravo_lib}" "${_ravo_lib}-${_ravo_major}"
      PATHS "${RAVO_FFMPEG_RUNTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  elseif(APPLE)
    find_file(_ravo_runtime NAMES "lib${_ravo_lib}.${_ravo_major}.dylib"
      PATHS "${RAVO_FFMPEG_RUNTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  else()
    find_file(_ravo_runtime NAMES "lib${_ravo_lib}.so.${_ravo_major}"
      PATHS "${RAVO_FFMPEG_RUNTIME_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  endif()
  add_library(ravo_ffmpeg_${_ravo_lib} SHARED IMPORTED GLOBAL)
  set_target_properties(ravo_ffmpeg_${_ravo_lib} PROPERTIES IMPORTED_LOCATION "${_ravo_runtime}"
    SYSTEM FALSE
    INTERFACE_INCLUDE_DIRECTORIES "${_ravo_ffmpeg_headers};${RAVO_FFMPEG_SOURCE_ROOT}")
  if(WIN32)
    set_target_properties(ravo_ffmpeg_${_ravo_lib} PROPERTIES IMPORTED_IMPLIB "${_ravo_import}")
  endif()
endforeach()
