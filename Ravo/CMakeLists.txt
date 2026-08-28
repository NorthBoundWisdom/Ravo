if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Configure Ravo from the repository root with cmake --preset")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Included from the repository-root CMakeLists.txt. Frozen 0.9 is in
# legacy/src/ and is not part of this graph.

# Qt's Qml package auto-includes every shipped QML plugin. Optional plugins such
# as AssetDownloader require Qt TaskTree, which is not part of the Ravo prefix
# and is unused. Runtime plugins still load from the Qt installation.
set(QT_SKIP_AUTO_QML_PLUGIN_INCLUSION ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS
  Core
  Gui
  Sql
  Qml
  Quick
  QuickControls2
  QuickDialogs2
  QuickLayouts
  LinguistTools
  Svg)

set(_ravo_required_qt_runtime_plugins
  Qt6::QJpegPlugin
  Qt6::QGifPlugin
  Qt6::QWebpPlugin
  Qt6::QTiffPlugin
  Qt6::QSQLiteDriverPlugin)
foreach(_ravo_plugin_target IN LISTS _ravo_required_qt_runtime_plugins)
  if(NOT TARGET ${_ravo_plugin_target})
    message(FATAL_ERROR
      "Required Qt runtime plugin target is missing: ${_ravo_plugin_target}. Install a complete Qt kit including qtimageformats.")
  endif()
endforeach()
unset(_ravo_plugin_target)
unset(_ravo_required_qt_runtime_plugins)

find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(quill CONFIG REQUIRED)
if(ZLIB_INCLUDE_DIR AND EXISTS "${ZLIB_INCLUDE_DIR}/QtCore/qarraydata.h")
  message(STATUS
    "ZLIB include ${ZLIB_INCLUDE_DIR} also contains Qt headers; "
    "Ravo prepends the selected Qt 6.11 kit so those umbrella headers cannot shadow QList")
endif()

function(ravo_require_migration_source_root output_variable environment_variable required_path)
  if(NOT DEFINED ENV{${environment_variable}})
    message(FATAL_ERROR
      "${environment_variable} is required; run the FreeCM source-root update")
  endif()
  file(TO_CMAKE_PATH "$ENV{${environment_variable}}" _source_root)
  if(NOT EXISTS "${_source_root}/${required_path}")
    message(FATAL_ERROR
      "${environment_variable} does not contain ${required_path}: ${_source_root}")
  endif()
  set(${output_variable} "${_source_root}" PARENT_SCOPE)
endfunction()

# These approved migration inputs are materialized and validated so metadata,
# lens-database, and codec adapters cannot fall back to host-selected source.
# Each owning migration item links only the source required by its private
# adapter contract.
ravo_require_migration_source_root(
  RAVO_EXIV2_SOURCE_ROOT EXIV2_SOURCE_ROOT include/exiv2/exiv2.hpp)
ravo_require_migration_source_root(
  RAVO_LIBJPEG_TURBO_SOURCE_ROOT LIBJPEG_TURBO_SOURCE_ROOT src/jpeglib.h)
ravo_require_migration_source_root(
  RAVO_LENSFUN_SOURCE_ROOT LENSFUN_SOURCE_ROOT data/db/timestamp.txt)
ravo_require_migration_source_root(
  RAVO_LIBTIFF_SOURCE_ROOT LIBTIFF_SOURCE_ROOT libtiff/tiffio.h)

# libjpeg-turbo intentionally rejects add_subdirectory integration. Build the
# pinned source root in an isolated ExternalProject and import only its static
# classic libjpeg archive; no host-selected JPEG fallback or dependency install
# surface enters the Ravo graph.
include(ExternalProject)
set(_ravo_libjpeg_turbo_prefix
  "${CMAKE_BINARY_DIR}/_deps/ravo-libjpeg-turbo")
set(_ravo_libjpeg_turbo_install
  "${_ravo_libjpeg_turbo_prefix}/install")
if(MSVC)
  set(_ravo_libjpeg_turbo_archive
    "${_ravo_libjpeg_turbo_install}/lib/jpeg-static${CMAKE_STATIC_LIBRARY_SUFFIX}")
else()
  set(_ravo_libjpeg_turbo_archive
    "${_ravo_libjpeg_turbo_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()
set(_ravo_libjpeg_turbo_cmake_args
  "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
  "-DCMAKE_INSTALL_LIBDIR=lib"
  "-DCMAKE_INSTALL_INCLUDEDIR=include"
  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
  "-DENABLE_SHARED=OFF"
  "-DENABLE_STATIC=ON"
  "-DWITH_ARITH_DEC=OFF"
  "-DWITH_ARITH_ENC=OFF"
  "-DWITH_JPEG7=OFF"
  "-DWITH_JPEG8=OFF"
  "-DWITH_SIMD=OFF"
  "-DWITH_TURBOJPEG=OFF"
  "-DWITH_TOOLS=OFF"
  "-DWITH_TESTS=OFF"
  "-DWITH_JNA="
  "-DWITH_FUZZ=OFF"
  "-DWITH_PROFILE=OFF")
if(CMAKE_BUILD_TYPE)
  list(APPEND _ravo_libjpeg_turbo_cmake_args
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
if(CMAKE_C_COMPILER)
  list(APPEND _ravo_libjpeg_turbo_cmake_args
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
  list(APPEND _ravo_libjpeg_turbo_cmake_args
    "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
if(CMAKE_OSX_ARCHITECTURES)
  list(APPEND _ravo_libjpeg_turbo_cmake_args
    "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND _ravo_libjpeg_turbo_cmake_args
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
ExternalProject_Add(ravo_libjpeg_turbo_external
  PREFIX "${_ravo_libjpeg_turbo_prefix}/project"
  SOURCE_DIR "${RAVO_LIBJPEG_TURBO_SOURCE_ROOT}"
  BINARY_DIR "${_ravo_libjpeg_turbo_prefix}/build"
  INSTALL_DIR "${_ravo_libjpeg_turbo_install}"
  CMAKE_ARGS ${_ravo_libjpeg_turbo_cmake_args}
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  TEST_COMMAND ""
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${_ravo_libjpeg_turbo_archive}"
  EXCLUDE_FROM_ALL TRUE)
file(MAKE_DIRECTORY "${_ravo_libjpeg_turbo_install}/include")
add_library(ravo_libjpeg_turbo STATIC IMPORTED GLOBAL)
set_target_properties(ravo_libjpeg_turbo PROPERTIES
  IMPORTED_LOCATION "${_ravo_libjpeg_turbo_archive}"
  INTERFACE_INCLUDE_DIRECTORIES "${_ravo_libjpeg_turbo_install}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ravo_libjpeg_turbo_install}/include")
add_dependencies(ravo_libjpeg_turbo ravo_libjpeg_turbo_external)
unset(_ravo_libjpeg_turbo_archive)
unset(_ravo_libjpeg_turbo_cmake_args)
unset(_ravo_libjpeg_turbo_install)
unset(_ravo_libjpeg_turbo_prefix)

# Build the pinned LibTIFF source in a second isolated ExternalProject. Ravo
# imports only the classic static C library with Deflate enabled; tools,
# tests, documentation, C++, shared libraries, and unrelated codecs stay out
# of the product graph. The imported target also links the already-authorized
# ZLIB owner, so no host-selected TIFF package is consulted.
set(_ravo_libtiff_prefix
  "${CMAKE_BINARY_DIR}/_deps/ravo-libtiff")
set(_ravo_libtiff_install
  "${_ravo_libtiff_prefix}/install")
if(MSVC)
  set(_ravo_libtiff_archive_debug
    "${_ravo_libtiff_install}/lib/tiffd${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_ravo_libtiff_archive_release
    "${_ravo_libtiff_install}/lib/tiff${CMAKE_STATIC_LIBRARY_SUFFIX}")
  if(CMAKE_CONFIGURATION_TYPES)
    set(_ravo_libtiff_archive "${_ravo_libtiff_archive_release}")
    set(_ravo_libtiff_build_byproduct
      "${_ravo_libtiff_install}/lib/tiff$<$<CONFIG:Debug>:d>${CMAKE_STATIC_LIBRARY_SUFFIX}")
  elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_ravo_libtiff_archive "${_ravo_libtiff_archive_debug}")
    set(_ravo_libtiff_build_byproduct "${_ravo_libtiff_archive_debug}")
  else()
    set(_ravo_libtiff_archive "${_ravo_libtiff_archive_release}")
    set(_ravo_libtiff_build_byproduct "${_ravo_libtiff_archive_release}")
  endif()
else()
  set(_ravo_libtiff_archive
    "${_ravo_libtiff_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}tiff${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_ravo_libtiff_build_byproduct "${_ravo_libtiff_archive}")
endif()
set(_ravo_libtiff_cmake_args
  "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
  "-DCMAKE_INSTALL_LIBDIR=lib"
  "-DCMAKE_INSTALL_INCLUDEDIR=include"
  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
  "-DBUILD_SHARED_LIBS=OFF"
  "-Dtiff-static=ON"
  "-Dtiff-install=ON"
  "-Dtiff-tools=OFF"
  "-Dtiff-tests=OFF"
  "-Dtiff-contrib=OFF"
  "-Dtiff-docs=OFF"
  "-Dtiff-cxx=OFF"
  "-Dtiff-opengl=OFF"
  "-Dzlib=ON"
  "-Dlibdeflate=OFF"
  "-Dpixarlog=OFF"
  "-Djpeg=OFF"
  "-Dold-jpeg=OFF"
  "-Djbig=OFF"
  "-Dlerc=OFF"
  "-Dlzma=OFF"
  "-Dzstd=OFF"
  "-Dwebp=OFF"
  "-Dccitt=OFF"
  "-Dpackbits=OFF"
  "-Dlzw=OFF"
  "-Dthunder=OFF"
  "-Dnext=OFF"
  "-Dlogluv=OFF"
  "-Dmdi=OFF"
  "-Dld-version-script=OFF"
  "-Dstrip-chopping=OFF"
  "-Ddefer-strile-load=OFF"
  "-Dchunky-strip-read=OFF"
  "-Dextrasample-as-alpha=OFF"
  "-Dcheck-ycbcr-subsampling=OFF")
if(CMAKE_BUILD_TYPE)
  list(APPEND _ravo_libtiff_cmake_args
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
if(CMAKE_C_COMPILER)
  list(APPEND _ravo_libtiff_cmake_args
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
  list(APPEND _ravo_libtiff_cmake_args
    "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
if(CMAKE_OSX_ARCHITECTURES)
  list(APPEND _ravo_libtiff_cmake_args
    "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND _ravo_libtiff_cmake_args
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(ZLIB_ROOT)
  list(APPEND _ravo_libtiff_cmake_args "-DZLIB_ROOT=${ZLIB_ROOT}")
endif()
if(ZLIB_INCLUDE_DIR)
  list(APPEND _ravo_libtiff_cmake_args "-DZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIR}")
endif()
if(ZLIB_LIBRARY_RELEASE)
  list(APPEND _ravo_libtiff_cmake_args
    "-DZLIB_LIBRARY_RELEASE=${ZLIB_LIBRARY_RELEASE}")
endif()
if(ZLIB_LIBRARY_DEBUG)
  list(APPEND _ravo_libtiff_cmake_args
    "-DZLIB_LIBRARY_DEBUG=${ZLIB_LIBRARY_DEBUG}")
endif()
ExternalProject_Add(ravo_libtiff_external
  PREFIX "${_ravo_libtiff_prefix}/project"
  SOURCE_DIR "${RAVO_LIBTIFF_SOURCE_ROOT}"
  BINARY_DIR "${_ravo_libtiff_prefix}/build"
  INSTALL_DIR "${_ravo_libtiff_install}"
  CMAKE_ARGS ${_ravo_libtiff_cmake_args}
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  TEST_COMMAND ""
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${_ravo_libtiff_build_byproduct}"
  EXCLUDE_FROM_ALL TRUE)
file(MAKE_DIRECTORY "${_ravo_libtiff_install}/include")
add_library(ravo_libtiff STATIC IMPORTED GLOBAL)
set_target_properties(ravo_libtiff PROPERTIES
  IMPORTED_LOCATION "${_ravo_libtiff_archive}"
  INTERFACE_INCLUDE_DIRECTORIES "${_ravo_libtiff_install}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ravo_libtiff_install}/include")
if(MSVC)
  set_target_properties(ravo_libtiff PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
    IMPORTED_LOCATION_DEBUG "${_ravo_libtiff_archive_debug}"
    IMPORTED_LOCATION_RELEASE "${_ravo_libtiff_archive_release}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_ravo_libtiff_archive_release}"
    IMPORTED_LOCATION_MINSIZEREL "${_ravo_libtiff_archive_release}")
endif()
add_dependencies(ravo_libtiff ravo_libtiff_external)
unset(_ravo_libtiff_archive)
unset(_ravo_libtiff_archive_debug)
unset(_ravo_libtiff_archive_release)
unset(_ravo_libtiff_build_byproduct)
unset(_ravo_libtiff_cmake_args)
unset(_ravo_libtiff_install)
unset(_ravo_libtiff_prefix)

if(NOT DEFINED ENV{GEOCONTROLS_SOURCE_ROOT})
  message(FATAL_ERROR "GEOCONTROLS_SOURCE_ROOT is required; run the FreeCM source-root update")
endif()
file(TO_CMAKE_PATH "$ENV{GEOCONTROLS_SOURCE_ROOT}" _ravo_geocontrols_root)
set(GEOCONTROLS_BUILD_DEMO OFF CACHE BOOL "GeoControls demo stays out of the Ravo graph" FORCE)
set(GEOCONTROLS_INSTALL_SDK OFF CACHE BOOL "GeoControls is consumed from its source root" FORCE)
add_subdirectory("${_ravo_geocontrols_root}" "${CMAKE_BINARY_DIR}/_deps/geocontrols")
if(NOT TARGET GeoControlsControls)
  message(FATAL_ERROR "GeoControls source root did not provide GeoControlsControls")
endif()
if(NOT TARGET GeoControlsAppShell)
  message(FATAL_ERROR "GeoControls source root did not provide GeoControlsAppShell")
endif()
if(NOT TARGET GeoControls::Controls)
  add_library(GeoControls::Controls ALIAS GeoControlsControls)
endif()
if(NOT TARGET GeoControls::AppShell)
  add_library(GeoControls::AppShell ALIAS GeoControlsAppShell)
endif()

if(NOT DEFINED ENV{LIBRAW_SOURCE_ROOT})
  message(FATAL_ERROR "LIBRAW_SOURCE_ROOT is required; run the FreeCM source-root update")
endif()

file(TO_CMAKE_PATH "$ENV{LIBRAW_SOURCE_ROOT}" _ravo_libraw_root)
file(GLOB_RECURSE _ravo_libraw_sources CONFIGURE_DEPENDS
  "${_ravo_libraw_root}/src/*.cpp")
file(GLOB_RECURSE _ravo_libraw_placeholders CONFIGURE_DEPENDS
  "${_ravo_libraw_root}/src/*_ph.cpp")
list(REMOVE_ITEM _ravo_libraw_sources ${_ravo_libraw_placeholders})

add_library(ravo_libraw STATIC ${_ravo_libraw_sources})
target_include_directories(ravo_libraw SYSTEM PUBLIC "${_ravo_libraw_root}")
target_compile_definitions(ravo_libraw
  PUBLIC LIBRAW_NODLL
  PRIVATE LIBRAW_NOTHREADS USE_ZLIB)
target_compile_options(ravo_libraw PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/w>
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
target_link_libraries(ravo_libraw PRIVATE ZLIB::ZLIB)
set_target_properties(ravo_libraw PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_compile_definitions(ravo_libraw PRIVATE
    LIBRAW_BUILDLIB
    _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(ravo_libraw PRIVATE ws2_32)
elseif(UNIX)
  target_link_libraries(ravo_libraw PRIVATE m)
endif()

# Exposure metadata stays behind the engine's RAW adapter. Keep the pinned
# Exiv2 source fully offline and omit every unrelated application/test surface.
function(ravo_add_private_exiv2)
  # Exiv2's 3.11 policy floor predates CMP0077/CMP0074. Supply the parent
  # defaults so its option() calls honor this function-scoped offline profile.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0074 NEW)
  set(BUILD_SHARED_LIBS OFF)
  set(EXIV2_ENABLE_XMP OFF)
  set(EXIV2_ENABLE_EXTERNAL_XMP OFF)
  set(EXIV2_ENABLE_PNG OFF)
  set(EXIV2_ENABLE_NLS OFF)
  set(EXIV2_ENABLE_LENSDATA OFF)
  set(EXIV2_ENABLE_WEBREADY OFF)
  set(EXIV2_ENABLE_CURL OFF)
  set(EXIV2_ENABLE_BMFF ON)
  set(EXIV2_ENABLE_BROTLI OFF)
  set(EXIV2_ENABLE_VIDEO OFF)
  set(EXIV2_ENABLE_INIH OFF)
  set(EXIV2_ENABLE_FILESYSTEM_ACCESS ON)
  # Ravo already found Zlib for its own codec graph. Do not let that parent
  # result define EXV_HAVE_LIBZ after Exiv2's PNG sources were disabled.
  set(ZLIB_FOUND OFF)
  set(EXIV2_BUILD_SAMPLES OFF)
  set(EXIV2_BUILD_EXIV2_COMMAND OFF)
  set(EXIV2_BUILD_UNIT_TESTS OFF)
  set(EXIV2_BUILD_FUZZ_TESTS OFF)
  set(EXIV2_BUILD_DOC OFF)
  set(EXIV2_TEAM_PACKAGING OFF)
  set(BUILD_WITH_CCACHE OFF)
  set(BUILD_WITH_COVERAGE OFF)

  # FindIconv can expose the active Xcode SDK's usr/include as a system include.
  # AppleClang already searches that directory through its SDK, while repeating
  # it after libc++ changes the normal C/C++ header ordering. Remove only that
  # exact redundant Xcode-SDK spelling; custom/Homebrew Iconv roots stay intact.
  if(APPLE)
    find_package(Iconv)
    if(TARGET Iconv::Iconv)
      get_target_property(_ravo_iconv_includes Iconv::Iconv INTERFACE_INCLUDE_DIRECTORIES)
      if(_ravo_iconv_includes)
        set(_ravo_filtered_iconv_includes "")
        foreach(_ravo_iconv_include IN LISTS _ravo_iconv_includes)
          if(NOT _ravo_iconv_include MATCHES
              "/Xcode[^/]*/Contents/Developer/Platforms/MacOSX[.]platform/Developer/SDKs/MacOSX[^/]*[.]sdk/usr/include/?$")
            list(APPEND _ravo_filtered_iconv_includes "${_ravo_iconv_include}")
          endif()
        endforeach()
        set_property(TARGET Iconv::Iconv PROPERTY
          INTERFACE_INCLUDE_DIRECTORIES "${_ravo_filtered_iconv_includes}")
      endif()
    endif()
  endif()
  add_subdirectory("${RAVO_EXIV2_SOURCE_ROOT}" "${CMAKE_BINARY_DIR}/_deps/exiv2" EXCLUDE_FROM_ALL)
  if(TARGET exiv2lib)
    target_include_directories(exiv2lib SYSTEM INTERFACE
      "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>")
  endif()
endfunction()
ravo_add_private_exiv2()
if(NOT TARGET Exiv2::exiv2lib)
  message(FATAL_ERROR "Exiv2 source root did not provide Exiv2::exiv2lib")
endif()

if(NOT DEFINED ENV{LITTLECMS_SOURCE_ROOT})
  message(FATAL_ERROR "LITTLECMS_SOURCE_ROOT is required; run the FreeCM source-root update")
endif()
file(TO_CMAKE_PATH "$ENV{LITTLECMS_SOURCE_ROOT}" _ravo_littlecms_root)
set(LCMS2_BUILD_SHARED OFF CACHE BOOL "Ravo links LittleCMS privately and statically" FORCE)
set(LCMS2_BUILD_STATIC ON CACHE BOOL "Ravo requires the LittleCMS static library" FORCE)
set(LCMS2_BUILD_TOOLS OFF CACHE BOOL "LittleCMS command-line tools stay out of Ravo" FORCE)
set(LCMS2_BUILD_TESTS OFF CACHE BOOL "Ravo owns its ICC contract tests" FORCE)
set(LCMS2_WITH_FASTFLOAT OFF CACHE BOOL "LittleCMS GPL plugin is not part of Ravo" FORCE)
set(LCMS2_WITH_THREADED_PLUGIN OFF CACHE BOOL "Ravo owns render scheduling" FORCE)
if(WIN32)
  # LittleCMS does find_library(m). After setenv loads Intel oneAPI, that search
  # resolves to oneAPI libm.lib and LNK2005-conflicts with the MSVC CRT.
  set(LCMS2_MATH_LIB "" CACHE FILEPATH "Windows CRT already provides libm" FORCE)
endif()
add_subdirectory("${_ravo_littlecms_root}" "${CMAKE_BINARY_DIR}/_deps/littlecms" EXCLUDE_FROM_ALL)
if(NOT TARGET lcms2)
  message(FATAL_ERROR "LittleCMS source root did not provide the lcms2 target")
endif()

# Homebrew/linuxbrew expose ZLIB/PNG as <prefix>/include. That umbrella also
# contains a second Qt tree (QtCore/qarraydata.h). If those SYSTEM includes
# precede the selected Qt kit, QList templates compile against the brew ABI
# while the binary links the official kit — SIGSEGV on Linux in QStringList.
function(ravo_prefer_selected_qt_headers target)
  if(NOT TARGET Qt6::Core)
    return()
  endif()
  set(_ravo_qt_includes "$<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>")
  if(TARGET Qt6::Gui)
    list(APPEND _ravo_qt_includes
      "$<TARGET_PROPERTY:Qt6::Gui,INTERFACE_INCLUDE_DIRECTORIES>")
  endif()
  target_include_directories(${target} SYSTEM BEFORE PRIVATE ${_ravo_qt_includes})
endfunction()

function(ravo_configure_target target)
  ravo_prefer_selected_qt_headers(${target})
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
    $<$<CXX_COMPILER_ID:MSVC>:/permissive->
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wconversion>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wshadow>
    # Source-derived colour and codec goldens pin expression order. Apply the
    # same no-contraction rule to production owners and their test oracles.
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)
endfunction()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RavoWindowsRuntime.cmake")

add_subdirectory(foundation)
add_subdirectory(recipe)
add_subdirectory(engine)
add_subdirectory(domain)
add_subdirectory(adapters)
add_subdirectory(services)
add_subdirectory(cli)
add_subdirectory(desktop)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(
  TARGETS ravo_foundation ravo_recipe ravo_engine ravo_domain ravo_adapters ravo_services ravo_desktop ravo_cli ravo_libraw ravo ravo_studio
  BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(
  DIRECTORY
    foundation/include/
    recipe/include/
    engine/include/
    domain/include/
    adapters/include/
    services/include/
    desktop/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/Ravo")

if(WIN32)
  install(
    DIRECTORY "$<TARGET_FILE_DIR:ravo>/"
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILES_MATCHING
      PATTERN "*.dll"
      PATTERN "CMakeFiles" EXCLUDE)
endif()

# Release deployment is owned by FreeCM. Ravo supplies only the product
# targets, platform metadata, and dependency search roots.
include("${CMAKE_SOURCE_DIR}/FreeCM/repomgrcpp/cmake/CppKitPackage.cmake")

cppkit_qt_bin_dir(RAVO_PACKAGE_QT_BIN_DIR)
set(RAVO_PACKAGE_QML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/desktop/qml")
set(RAVO_PACKAGE_DIST_ROOT "${CMAKE_BINARY_DIR}/dist")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/package")

set(RAVO_PACKAGE_RUNTIME_SEARCH_PATHS "" CACHE STRING
  "Runtime library directories supplied by the active FreeCM lock")
if(NOT RAVO_PACKAGE_RUNTIME_SEARCH_PATHS)
  message(FATAL_ERROR
    "RAVO_PACKAGE_RUNTIME_SEARCH_PATHS is required; update the active source-roots lock and run --update")
endif()
set(_ravo_package_runtime_search_paths
  ${RAVO_PACKAGE_RUNTIME_SEARCH_PATHS})
list(REMOVE_DUPLICATES _ravo_package_runtime_search_paths)
cppkit_json_string_array(
  RAVO_PACKAGE_RUNTIME_SEARCH_PATHS_JSON
  _ravo_package_runtime_search_paths)

set(RAVO_PACKAGE_ARCHITECTURE "" CACHE STRING
  "Artifact architecture supplied by the active FreeCM lock")
if(NOT RAVO_PACKAGE_ARCHITECTURE MATCHES "^[A-Za-z0-9_]+$")
  message(FATAL_ERROR
    "RAVO_PACKAGE_ARCHITECTURE is required and must be an artifact-safe name; update the active source-roots lock and run --update")
endif()

if(APPLE)
  set(RAVO_PACKAGE_DIST_NAME
    "RavoStudio-${PROJECT_VERSION}-${RAVO_PACKAGE_ARCHITECTURE}-macOS")
  set(RAVO_PACKAGE_CONFIG "${CMAKE_BINARY_DIR}/package/package_mac.json")
  configure_file(
    "${CMAKE_SOURCE_DIR}/configs/package_mac.json.in"
    "${RAVO_PACKAGE_CONFIG}"
    @ONLY)

  add_custom_target(RavoPackagePayload
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_BUNDLE_DIR:ravo_studio>/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:ravo>"
      "$<TARGET_BUNDLE_DIR:ravo_studio>/Contents/MacOS/ravo"
    DEPENDS ravo ravo_studio
    COMMENT "Adding the ravo CLI to the Ravo Studio bundle"
    VERBATIM)
  set_target_properties(RavoPackagePayload PROPERTIES FOLDER "Deployment")

  cppkit_add_package_deploy(ravo_studio
    CONFIG "${RAVO_PACKAGE_CONFIG}"
    PLATFORM mac
    TARGET_NAME RavoDeploy
    DEPENDS RavoPackagePayload)
  add_custom_target(RavoPackage DEPENDS RavoDeploy)
elseif(WIN32)
  set(RAVO_PACKAGE_DIST_NAME
    "RavoStudio-${PROJECT_VERSION}-${RAVO_PACKAGE_ARCHITECTURE}-Windows")
  set(RAVO_PACKAGE_CONFIG "${CMAKE_BINARY_DIR}/package/package_win.json")
  set(RAVO_PACKAGE_ARCHIVE
    "${CMAKE_BINARY_DIR}/package/${RAVO_PACKAGE_DIST_NAME}.zip")
  configure_file(
    "${CMAKE_SOURCE_DIR}/configs/package_win.json.in"
    "${RAVO_PACKAGE_CONFIG}"
    @ONLY)

  cppkit_add_package_deploy(ravo_studio
    CONFIG "${RAVO_PACKAGE_CONFIG}"
    PLATFORM win
    TARGET_NAME RavoDeploy
    DEPENDS ravo)
  add_custom_target(RavoPackage
    COMMAND ${CMAKE_COMMAND} -E rm -f "${RAVO_PACKAGE_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${RAVO_PACKAGE_ARCHIVE}"
      --format=zip "${RAVO_PACKAGE_DIST_NAME}"
    WORKING_DIRECTORY "${RAVO_PACKAGE_DIST_ROOT}"
    DEPENDS RavoDeploy
    COMMENT "Creating ${RAVO_PACKAGE_ARCHIVE}"
    VERBATIM)
elseif(UNIX)
  set(RAVO_PACKAGE_ARTIFACT_STEM
    "RavoStudio-${PROJECT_VERSION}-${RAVO_PACKAGE_ARCHITECTURE}-Linux")
  set(RAVO_PACKAGE_DIST_NAME "${RAVO_PACKAGE_ARTIFACT_STEM}.AppDir")
  set(RAVO_PACKAGE_CONFIG "${CMAKE_BINARY_DIR}/package/package_linux.json")
  set(RAVO_PACKAGE_DEB_OUTPUT
    "${CMAKE_BINARY_DIR}/package/${RAVO_PACKAGE_ARTIFACT_STEM}.deb")
  set(RAVO_PACKAGE_APPIMAGE_TOOL "" CACHE STRING
    "AppImage tool supplied by the active FreeCM lock")
  set(RAVO_PACKAGE_DEB_TOOL "" CACHE STRING
    "Debian package tool supplied by the active FreeCM lock")
  set(RAVO_PACKAGE_DEB_ARCHITECTURE "" CACHE STRING
    "Debian architecture supplied by the active FreeCM lock")
  if(NOT RAVO_PACKAGE_APPIMAGE_TOOL
      OR NOT RAVO_PACKAGE_DEB_TOOL
      OR NOT RAVO_PACKAGE_DEB_ARCHITECTURE)
    message(FATAL_ERROR
      "Linux package tools and DEB architecture are required; update the active source-roots lock and run --update")
  endif()

  set(_ravo_linux_runtime_libraries "")
  foreach(_ravo_library_dir IN LISTS _ravo_package_runtime_search_paths)
    file(GLOB _ravo_prefix_runtime_libraries LIST_DIRECTORIES FALSE
      "${_ravo_library_dir}/libQt6*.so"
      "${_ravo_library_dir}/libQt6*.so.*"
      "${_ravo_library_dir}/libpng*.so"
      "${_ravo_library_dir}/libpng*.so.*"
      "${_ravo_library_dir}/libquill*.so"
      "${_ravo_library_dir}/libquill*.so.*"
      "${_ravo_library_dir}/libsqlite3.so"
      "${_ravo_library_dir}/libsqlite3.so.*"
      "${_ravo_library_dir}/libz.so"
      "${_ravo_library_dir}/libz.so.*")
    list(APPEND _ravo_linux_runtime_libraries
      ${_ravo_prefix_runtime_libraries})
  endforeach()
  list(REMOVE_DUPLICATES _ravo_linux_runtime_libraries)
  if(NOT _ravo_linux_runtime_libraries)
    message(FATAL_ERROR
      "Ravo packaging could not find runtime libraries under RAVO_PACKAGE_RUNTIME_SEARCH_PATHS")
  endif()
  cppkit_json_string_array(
    RAVO_PACKAGE_LINUX_RUNTIME_LIBRARIES_JSON
    _ravo_linux_runtime_libraries)
  configure_file(
    "${CMAKE_SOURCE_DIR}/configs/package_linux.json.in"
    "${RAVO_PACKAGE_CONFIG}"
    @ONLY)

  cppkit_add_package_deploy(ravo_studio
    CONFIG "${RAVO_PACKAGE_CONFIG}"
    PLATFORM linux
    TARGET_NAME RavoDeploy
    DEPENDS ravo)
  add_custom_target(RavoPackage DEPENDS RavoDeploy)
endif()

set_target_properties(RavoPackage PROPERTIES FOLDER "Deployment")
