add_subdirectory(external)

# The managed Imath subproject sets CMAKE_DEBUG_POSTFIX to "_d" in the cache.
# Keep that suffix on the dependency targets already created above, but do not
# let it change DarkTableNext's runtime module names: module discovery derives
# operation names directly from filenames and requires them to be build-type
# independent.
set(CMAKE_DEBUG_POSTFIX "")

include(CheckCSourceCompiles)
include(CheckCXXSymbolExists)


#
# Add files for libdarktable
#
FILE(GLOB SOURCE_FILES
  "bauhaus/bauhaus.c"
  "common/act_on.c"
  "common/atomic.c"
  "common/bilateral.c"
  "common/bilateralcl.c"
  "common/box_filters.cc"
  "common/cache.c"
  "common/calculator.c"
  "common/collection.c"
  "common/color_harmony.c"
  "common/color_picker.c"
  "common/color_vocabulary.c"
  "common/colorlabels.c"
  "common/colorspaces.c"
  "common/curl_tools.c"
  "common/curve_tools.c"
  "common/custom_primaries.c"
  "common/darktable.c"
  "common/database.c"
  "common/database_schema.c"
  "common/datetime.c"
  "common/dbus.c"
  "common/distance_transform.c"
  "common/dlopencl.c"
  "common/dng_opcode.c"
  "common/dtpthread.c"
  "common/dwt.c"
  "common/dynload.c"
  "common/eaw.c"
  "common/exif.cc"
  "common/file_location.c"
  "common/film.c"
  "common/gaussian.c"
  "common/gimp.c"
  "common/grouping.c"
  "common/guided_filter.c"
  "common/heal.c"
  "common/histogram.c"
  "common/history.c"
  "common/history_snapshot.c"
  "common/image.c"
  "common/image_cache.c"
  "common/imagebuf.c"
  "common/import_session.c"
  "common/interpolation.c"
  "common/iop_group.c"
  "common/iop_order.c"
  "common/iop_profile.c"
  "common/locallaplacian.c"
  "common/locallaplaciancl.c"
  "common/matrices.c"
  "common/metadata.c"
  "common/metadata_export.c"
  "common/mipmap_cache.c"
  "common/module.c"
  "common/nlmeans_core.c"
  "common/noiseprofiles.c"
  "common/opencl.c"
  "common/overlay.c"
  "common/presets.c"
  "common/pwstorage/pwstorage.c"
  "common/ratings.c"
  "common/resource_limits.c"
  "common/selection.c"
  "common/splines.cpp"
  "common/styles.c"
  "common/system_signal_handling.c"
  "common/tags.c"
  "common/undo.c"
  "common/usermanual_url.c"
  "common/utility.c"
  "common/variables.c"
  "common/wb_presets.c"
  "control/conf.c"
  "control/control.c"
  "control/crawler.c"
  "control/jobs.c"
  "control/jobs/control_jobs.c"
  "control/jobs/develop_jobs.c"
  "control/jobs/film_jobs.c"
  "control/jobs/image_jobs.c"
  "control/jobs/sidecar_jobs.c"
  "control/progress.c"
  "control/signal.c"
  "develop/blend.c"
  "develop/blend_gui.c"
  "develop/blends/blendif_lab.c"
  "develop/blends/blendif_raw.c"
  "develop/blends/blendif_rgb_hsl.c"
  "develop/blends/blendif_rgb_jzczhz.c"
  "develop/borders_helper.c"
  "develop/develop.c"
  "develop/format.c"
  "develop/imageop.c"
  "develop/imageop_gui.c"
  "develop/imageop_math.c"
  "develop/masks/brush.c"
  "develop/masks/circle.c"
  "develop/masks/ellipse.c"
  "develop/masks/gradient.c"
  "develop/masks/group.c"
  "develop/masks/masks.c"
  "develop/masks/path.c"
  "develop/pixelpipe.c"
  "develop/tiling.c"
  "dtgtk/button.c"
  "dtgtk/culling.c"
  "dtgtk/drawingarea.c"
  "dtgtk/expander.c"
  "dtgtk/gradientslider.c"
  "dtgtk/icon.c"
  "dtgtk/paint.c"
  "dtgtk/paint_cell.c"
  "dtgtk/range.c"
  "dtgtk/resetlabel.c"
  "dtgtk/sidepanel.c"
  "dtgtk/stylemenu.c"
  "dtgtk/thumbnail.c"
  "dtgtk/thumbnail_btn.c"
  "dtgtk/thumbtable.c"
  "dtgtk/togglebutton.c"
  "gui/about.c"
  "gui/accelerators.c"
  "gui/context_menu.c"
  "gui/system_commands.c"
  "gui/color_picker_proxy.c"
  "gui/workspace.c"
  "gui/gtk.c"
  "gui/gtkentry.c"
  "gui/guides.c"
  "gui/hist_dialog.c"
  "gui/import_metadata.c"
  "gui/log_history.c"
  "gui/metadata_tags.c"
  "gui/preferences.c"
  "gui/presets.c"
  "gui/splash.c"
  "gui/styles_dialog.c"
  "imageio/imageio.c"
  "imageio/imageio_dng.c"
  "imageio/imageio_jpeg.c"
  "imageio/imageio_module.c"
  "imageio/imageio_png.c"
  "imageio/imageio_qoi.c"
  "imageio/imageio_rgbe.c"
  "imageio/imageio_tiff.c"
  "libs/lib.c"
  "views/view.c"
  )

FILE(GLOB HEADER_FILES "*.h" "common/*.h" "${OPENCL_PATH}/CL/*.h" "control/*.h" "gui/*.h" "iop/*.h" "libs/*.h" "views/*.h")

list(APPEND SOURCE_FILES "main.c")
if(APPLE)
  list(APPEND SOURCE_FILES "osx/osx.c" "common/pwstorage/backend_apple_keychain.c")
  list(APPEND HEADER_FILES "osx/osx.h" "common/pwstorage/backend_apple_keychain.h")
elseif(WIN32)
  list(APPEND SOURCE_FILES "common/win_file_trash.c")
  list(APPEND HEADER_FILES "common/win_file_trash.h")
endif()

set(SOURCES ${SOURCE_FILES} ${HEADER_FILES})

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/external)
include_directories(SYSTEM ${OPENCL_PATH})

# initial compiler flags
add_definitions("-DHAVE_CONFIG_H")

if(APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
              CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
        list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_LINK_LIBRARIES stdc++)
        list(APPEND CMAKE_CXX_IMPLICIT_LINK_LIBRARIES c++)
        list(REMOVE_DUPLICATES CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
endif()

# Append thread flags/libraries to LIBS
list(APPEND LIBS "${CMAKE_THREAD_LIBS_INIT}")

if(WIN32)
  find_package(PThreads4W CONFIG REQUIRED)
  list(APPEND LIBS PThreads4W::PThreads4W)
  list(APPEND LIBS psapi)
endif()

# libm is part of libSystem on macOS. Other platforms may still require an
# explicit math library when this build logic is reused.
if(NOT APPLE AND NOT WIN32)
  list(APPEND LIBS "-lm")
endif()

if(NOT MSVC)
  if(OpenMP_C_FLAGS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
  endif()

  if(OpenMP_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
  endif()

  if(OpenMP_C_INCLUDE_DIRS)
    include_directories(${OpenMP_C_INCLUDE_DIRS})
  endif()

  if(OpenMP_CXX_INCLUDE_DIRS)
    include_directories(${OpenMP_CXX_INCLUDE_DIRS})
  endif()

  list(APPEND LIBS ${OpenMP_C_LIBRARIES} ${OpenMP_CXX_LIBRARIES})
endif()

#
# Find all other required libraries for building
#
# GTK3 does pull glib, but this allows us to check for the version
find_package(Glib 2.58 REQUIRED)
add_definitions("-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_58")
add_definitions("-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED")
include_directories(SYSTEM ${Glib_INCLUDE_DIRS})
list(APPEND LIBS ${Glib_LIBRARIES})

# GTK3 pulls in ATK, GDK, GDK-PIXBUF, CAIRO, GLIB, PANGO
find_package(GTK3 3.24.15 REQUIRED)
add_definitions("-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_24")
#add_definitions("-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_MIN_REQUIRED")
include_directories(SYSTEM ${GTK3_INCLUDE_DIRS})
list(APPEND LIBS ${GTK3_LIBRARIES})

# Check for libxml2 / broken cmake module can't be included in the foreach() below
find_package(LibXml2 2.6 REQUIRED)
include_directories(SYSTEM ${LIBXML2_INCLUDE_DIR})
list(APPEND LIBS ${LIBXML2_LIBRARIES})
add_definitions(${LIBXML2_DEFINITIONS})

# Used for 16-bit (half) float TIFF export (and faster import)
# (IlmBase Half library in 2.5 and earlier and Imath 3.0 did not have C API)
if(DARKTABLE_MANAGED_CPP_DEPENDENCIES)
  list(APPEND LIBS Imath::Imath)
  add_compile_definitions(HAVE_IMATH)
else()
  find_package(Imath 3.1 CONFIG)
  if(TARGET Imath::Imath)
    list(APPEND LIBS Imath::Imath)
    add_compile_definitions(HAVE_IMATH)
  endif()
endif()

# LibRaw is always built from the pinned FreeCM source root.
message(STATUS "Building LibRaw from the pinned FreeCM source root")
list(APPEND SOURCES "imageio/imageio_libraw.c")

find_package(LensFun REQUIRED)
include_directories(SYSTEM ${LensFun_INCLUDE_DIRS})
list(APPEND LIBS ${LensFun_LIBRARIES})

find_package(SQLite3 3.26 REQUIRED)
if(NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()
list(APPEND LIBS SQLite3::SQLite3)

foreach(lib ${OUR_LIBS} GIO GThread GModule PangoCairo Rsvg2 PNG JPEG TIFF LCMS2 JsonGlib)
  find_package(${lib} REQUIRED)
  include_directories(SYSTEM ${${lib}_INCLUDE_DIRS})
  list(APPEND LIBS ${${lib}_LIBRARIES})
  add_definitions(${${lib}_DEFINITIONS})
endforeach(lib)

if(PNG_VERSION_STRING VERSION_LESS 1.5)
  message(FATAL_ERROR "libpng version 1.5 or newer is required")
endif()

# Use imported target for curl
find_package(CURL 7.56 REQUIRED)
list(APPEND LIBS CURL::libcurl)

# Require Exiv2 0.28 or newer.
if(DARKTABLE_MANAGED_CPP_DEPENDENCIES)
  set(Exiv2_VERSION "0.28.8")
  set(Exiv2_INCLUDE_DIRS "${DARKTABLE_MANAGED_EXIV2_INCLUDE_DIR}")
  set(Exiv2_LIBRARIES Exiv2::exiv2lib)
  set(EXIV2_CONFIG_HEADER "${DARKTABLE_MANAGED_EXIV2_CONFIG_HEADER}")
  include_directories(SYSTEM "${DARKTABLE_MANAGED_EXIV2_INCLUDE_DIR}" "${CMAKE_BINARY_DIR}")
else()
  find_package(Exiv2 0.28.0 REQUIRED)
  set(EXIV2_CONFIG_HEADER "${Exiv2_INCLUDE_DIRS}/exiv2/exv_conf.h")
  include_directories(SYSTEM ${Exiv2_INCLUDE_DIRS})
endif()
list(APPEND LIBS ${Exiv2_LIBRARIES})
add_definitions(${Exiv2_DEFINITIONS})
# Enable ISOBMFF support in Exiv2 when the installed library provides it.
if(Exiv2_VERSION VERSION_GREATER_EQUAL "0.27.4")
  # exiv2 >= 0.27.4 provides optional support for ISOBMFF files.
  # This must be manually enabled during exiv2 build and can be checked with
  # the EXV_ENABLE_BMFF symbol.
  check_symbol_exists(EXV_ENABLE_BMFF
    "${EXIV2_CONFIG_HEADER}"
    HAVE_EXV_ENABLE_BMFF)
  if(HAVE_EXV_ENABLE_BMFF)
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} cr3 CACHE INTERNAL "")
    message(STATUS "Exiv2 >= 0.27.4 found with ISOBMFF support (CR3)")
  else()
    message(STATUS "Exiv2 >= 0.27.4 found, but was not compiled with ISOBMFF support (CR3)")
  endif()
else()
  message(STATUS "Exiv2 < 0.27.4 found, no support for ISOBMFF files (CR3)")
endif()

#
# Detect compile of optional pwstorage backends
#
if(APPLE)
  add_definitions("-DHAVE_APPLE_KEYCHAIN")
endif()

find_package(ICU REQUIRED COMPONENTS i18n data uc)
add_definitions(-DSQLITE_CORE -DSQLITE_ENABLE_ICU)
list(APPEND LIBS ICU::i18n ICU::data ICU::uc)
set(SOURCES ${SOURCES} "common/sqliteicu.c")

add_definitions("-DHAVE_OPENCL")


# Check for __builtin_cpu_supports here due to a bug in clang/llvm. LLVM uses
# libgcc to provide the symbols, so if clang wasn't compiled with libgcc, the
# compiler will say it's available; however the linker cannot find the symbols.
# So do a quick test compile to see if it's actually available.
# See https://llvm.org/bugs/show_bug.cgi?id=25510
check_c_source_compiles("#include <stdio.h>
int main() {
  __builtin_cpu_init();
  printf(\"%d\", __builtin_cpu_supports(\"sse3\"));
}" HAVE_BUILTIN_CPU_SUPPORTS)
if(HAVE_BUILTIN_CPU_SUPPORTS)
  add_definitions("-DHAVE_BUILTIN_CPU_SUPPORTS")
endif(HAVE_BUILTIN_CPU_SUPPORTS)
MESSAGE(STATUS "Does the compiler support __builtin_cpu_supports(): ${HAVE_BUILTIN_CPU_SUPPORTS}")

if(MSVC)
  set(HAVE_TLS TRUE)
else()
check_c_source_compiles("
static __thread int tls;
int main(void)
{
  return 0;
}" HAVE_TLS)
endif()
if(NOT HAVE_TLS)
  MESSAGE(FATAL_ERROR "The compiler does not support Thread-local storage.")
endif()


include(march-mtune)

if(MSVC)
  # Keep compiler debug information in every configuration for useful crash
  # reports. CMake's configuration defaults already select the matching MSVC
  # optimization level, so do not append ignored GNU optimization flags.
  add_compile_options(/Zi)
else()
  # -g is kept in every build so crash reports retain useful symbols.
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MARCH} -g")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O2 -ftree-vectorize")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MARCH} -g")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2 -ftree-vectorize")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")

  # These are compilation semantics, not linker options. Keeping them out of
  # CMAKE_<LANG>_FLAGS_RELEASE prevents Clang from warning about
  # -fno-finite-math-only on every macOS link command.
  add_compile_options(
    "$<$<CONFIG:Release>:-ffast-math>"
    "$<$<CONFIG:Release>:-fno-finite-math-only>"
  )
endif()

add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG>")

#
# Generate config.h
#
list(REMOVE_DUPLICATES DT_SUPPORTED_EXTENSIONS)
string(REPLACE ";" "\", \"" DT_SUPPORTED_EXTENSIONS_STRING "${DT_SUPPORTED_EXTENSIONS}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.cmake.h" "${DARKTABLE_BINDIR}/config.h" @ONLY)


#
# Build external deps
#
find_package(Pugixml 1.5 REQUIRED)
include_directories(SYSTEM ${Pugixml_INCLUDE_DIRS})
list(APPEND LIBS ${Pugixml_LIBRARIES})

if(NOT MSVC)
  add_definitions(-Werror -Wfatal-errors)
endif()

# Clang reports declined OpenMP SIMD optimization requests through
# -Wpass-failed. They are useful in a dedicated vectorization report, but are
# not correctness diagnostics and otherwise make routine Release builds noisy.
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
check_c_compiler_flag("-Wno-pass-failed" DT_C_HAS_NO_PASS_FAILED)
check_cxx_compiler_flag("-Wno-pass-failed" DT_CXX_HAS_NO_PASS_FAILED)
if(DT_C_HAS_NO_PASS_FAILED)
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:-Wno-pass-failed>")
endif()
if(DT_CXX_HAS_NO_PASS_FAILED)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-pass-failed>")
endif()

# macOS system headers are supplied by the compiler sysroot. Third-party CMake
# find modules occasionally add the SDK root as an explicit include directory;
# that puts it ahead of Homebrew LLVM's builtin headers and breaks C++ OpenMP.
if(APPLE)
  get_directory_property(DT_INCLUDE_DIRECTORIES INCLUDE_DIRECTORIES)
  list(FILTER DT_INCLUDE_DIRECTORIES EXCLUDE REGEX "/SDKs/MacOSX[^/]*/usr/include$")
  set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${DT_INCLUDE_DIRECTORIES}")
endif()

if(HAVE_EXECINFO_H)
  add_definitions("-DDT_HAVE_SIGNAL_TRACE")
  message(STATUS "Signal debug: print-trace possible")
endif()

#
# set development flags for our code
#
if(DT_CMAKE_INCLUDE_WHAT_YOU_USE)
  set(CMAKE_C_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
endif()

#
# Setup generation of preference dialog files..
#
add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_SOURCE_DIR}/data/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.xml
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
  COMMAND ${Xsltproc_BIN} --nonet --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${DARKTABLE_DATADIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
  COMMENT "Generating preferences"
)
add_custom_command(
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_conf.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.xml
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
  COMMAND ${Xsltproc_BIN} --nonet --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_conf.xsl ${DARKTABLE_DATADIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
  COMMENT "Generating configuration min/max/default/type"
)

# Only the generic example styles are bundled.  Per-camera presets were
# intentionally removed from the product baseline.
file(GLOB darktable_STYLES
     "${DARKTABLE_DATADIR}/styles/examples*.dtstyle"
)

add_custom_command(
  DEPENDS ${darktable_STYLES} ${CMAKE_SOURCE_DIR}/tools/generate_styles_string.sh
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/styles_string.h
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/generate_styles_string.sh ${DARKTABLE_DATADIR}/styles ${CMAKE_CURRENT_BINARY_DIR}/styles_string.h
  COMMENT "Generating styles strings for translation"
)

add_custom_target(
  generate_styles_string ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/styles_string.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(
  generate_preferences ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_SOURCE_DIR}/data/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_custom_target(
  generate_conf ALL
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc_conf.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_dependencies(generate_preferences validate_darktableconfig_xml generate_conf)

#
# build libdarktable
#
add_library(lib_darktable SHARED ${DARKTABLE_BINDIR}/preferences_gen.h ${DARKTABLE_BINDIR}/version_gen.c ${SOURCES})

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${DARKTABLE_BINDIR}/version_gen.c PROPERTIES GENERATED TRUE)

add_dependencies(lib_darktable generate_styles_string)
add_dependencies(lib_darktable generate_conf)
add_dependencies(lib_darktable generate_version)
add_dependencies(lib_darktable generate_preferences)
target_compile_definitions(lib_darktable PRIVATE DT_LIB_DARKTABLE_BUILD)

set_target_properties(lib_darktable PROPERTIES MACOSX_RPATH TRUE INSTALL_RPATH "${RPATH_ORIGIN}")
set_target_properties(lib_darktable PROPERTIES OUTPUT_NAME darktable)
set_target_properties(lib_darktable PROPERTIES LINKER_LANGUAGE C)
if(MSVC)
  # The executable is also named darktable. Keep the core DLL name stable while
  # giving its import library a distinct name on Windows. Unix exports the core
  # API by default; ask CMake to generate the equivalent MSVC export definition.
  set_target_properties(lib_darktable PROPERTIES
                        ARCHIVE_OUTPUT_NAME darktable_core
                        WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

#
# Link external libs
#

if(LIB_DEPS)
  add_dependencies(lib_darktable ${LIB_DEPS})
endif()

target_link_libraries(lib_darktable PUBLIC ${STATIC_LIBS})
target_link_libraries(lib_darktable PRIVATE whereami)
target_link_libraries(lib_darktable PUBLIC ${LIBS})

unset(LIBS)
unset(STATIC_LIBS)
unset(LIB_DEPS)

if(MSVC)
  # CMake's automatic Windows export definition only scans objects owned by the
  # DLL target, not objects pulled from a static archive. This wrapper is part
  # of the module ABI, so compile it directly into the core DLL on MSVC.
  target_sources(lib_darktable PRIVATE imageio/imageio_rawspeed.cc)
  target_link_libraries(lib_darktable PRIVATE rawspeed)
else()
  add_library(lib_darktable_imageio_rawspeed STATIC imageio/imageio_rawspeed.cc)
  target_link_libraries(lib_darktable_imageio_rawspeed PRIVATE rawspeed)
  target_link_libraries(lib_darktable PRIVATE lib_darktable_imageio_rawspeed)
endif()

#
# Install lib_darktable
#
install(TARGETS lib_darktable DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable)

# Compile views modules
add_subdirectory(views ${DARKTABLE_LIBDIR}/views)

# Compile lighttable modules
add_subdirectory(libs ${DARKTABLE_LIBDIR}/plugins/lighttable)

# Compile darkroom modules
add_subdirectory(iop ${DARKTABLE_LIBDIR}/plugins)

# Compile export modules
add_subdirectory(imageio ${DARKTABLE_LIBDIR}/plugins/imageio)

# have a command line interface
add_subdirectory(cli)

# have some test programs for unit testing, ...
if(BUILD_TESTING)
  add_subdirectory(tests)
  if(WIN32)
    add_custom_command(
      TARGET lib_darktable
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              $<TARGET_FILE_DIR:darktable-test-variables>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:lib_darktable>
              $<TARGET_FILE_DIR:darktable-test-variables>)
  endif()
endif(BUILD_TESTING)

# build darktable executable
if(APPLE)
  set(DARKTABLE_APP_BUNDLE_DIR "${DARKTABLE_BINDIR}/darktable.app")
  set(DARKTABLE_APP_RESOURCES_DIR "${DARKTABLE_APP_BUNDLE_DIR}/Contents/Resources")
  set(DARKTABLE_APP_FRAMEWORKS_DIR "${DARKTABLE_APP_BUNDLE_DIR}/Contents/Frameworks")
  set(DARKTABLE_LAUNCHER "${DARKTABLE_BINDIR}/darktable")
  set(DARKTABLE_APP_ICON_FILE "darktable.icns")
  set(DARKTABLE_APP_ICON_SOURCE "${CMAKE_SOURCE_DIR}/data/pixmaps/macos/${DARKTABLE_APP_ICON_FILE}")
  get_filename_component(DARKTABLE_APP_LIBRARY_ROOT "${DARKTABLE_LIBDIR}/.." ABSOLUTE)
  get_filename_component(DARKTABLE_APP_SHARE_ROOT "${DARKTABLE_DATADIR}/.." ABSOLUTE)

  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/darktable-macos-Info.plist.in"
                  "${CMAKE_CURRENT_BINARY_DIR}/darktable-Info.plist" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/darktable-launcher.in" "${DARKTABLE_LAUNCHER}" @ONLY)
  file(CHMOD "${DARKTABLE_LAUNCHER}"
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

  set_source_files_properties("${DARKTABLE_APP_ICON_SOURCE}" PROPERTIES
                              MACOSX_PACKAGE_LOCATION "Resources")
  add_executable(darktable MACOSX_BUNDLE main.c "${DARKTABLE_APP_ICON_SOURCE}")
  set_target_properties(darktable PROPERTIES
                        LINKER_LANGUAGE C
                        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/darktable-Info.plist"
                        MACOSX_BUNDLE_ICON_FILE "${DARKTABLE_APP_ICON_FILE}"
                        RUNTIME_OUTPUT_DIRECTORY "${DARKTABLE_BINDIR}")
  set_target_properties(lib_darktable PROPERTIES LINK_FLAGS "-framework Security -framework CoreFoundation")
else()
  add_executable(darktable main.c)
  set_target_properties(darktable PROPERTIES
                        LINKER_LANGUAGE C
                        RUNTIME_OUTPUT_DIRECTORY "${DARKTABLE_BINDIR}")
endif()
target_link_libraries(darktable lib_darktable)

foreach(_darktable_target lib_darktable darktable darktable-cli darktable-test-variables)
  if(TARGET ${_darktable_target})
    dt_target_enable_warnings(${_darktable_target})
  endif()
endforeach()

if(APPLE)
  # The installed application loads its core library from the same bundle.  The
  # relative module rpaths keep their existing layout when lib/darktable is
  # copied below Contents/Resources by macos_bundle_install.cmake.in.
  set_target_properties(darktable PROPERTIES
                        INSTALL_RPATH "${RPATH_ORIGIN}/../Resources/lib/darktable")

  # Loading freshly linked Mach-O modules from outside the application bundle
  # makes macOS assess every file independently on first use.  That turns a full
  # rebuild into a long first launch even though later launches are fast.  The
  # darktable target therefore owns the complete development bundle: it first
  # builds every loadable module, then copies project code into the bundle and
  # signs it from the inside out.  Module outputs are also link dependencies so
  # an incremental module rebuild retriggers bundle assembly and signing.
  # Homebrew dependencies remain external for fast iteration and are collected
  # only by package-macos.
  get_property(DARKTABLE_LOADABLE_MODULE_TARGETS GLOBAL PROPERTY DT_LOADABLE_MODULE_TARGETS)
  set(DARKTABLE_LOADABLE_MODULE_FILES)
  foreach(_darktable_module_target IN LISTS DARKTABLE_LOADABLE_MODULE_TARGETS)
    add_dependencies(darktable "${_darktable_module_target}")
    list(APPEND DARKTABLE_LOADABLE_MODULE_FILES
         "$<TARGET_FILE:${_darktable_module_target}>")
  endforeach()

  configure_file(
      "${CMAKE_SOURCE_DIR}/cmake/macos_development_bundle.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/macos_development_bundle.cmake"
      @ONLY)
  set_property(TARGET darktable APPEND PROPERTY LINK_DEPENDS
               ${DARKTABLE_LOADABLE_MODULE_FILES}
               "${CMAKE_CURRENT_BINARY_DIR}/macos_development_bundle.cmake"
               "${DARKTABLE_DATADIR}/icons/hicolor/index.theme"
               "${CMAKE_SOURCE_DIR}/data/pixmaps/scalable/actions/edit-clear-symbolic.svg"
               "${CMAKE_SOURCE_DIR}/data/pixmaps/scalable/actions/edit-find-symbolic.svg"
               "${CMAKE_SOURCE_DIR}/data/pixmaps/scalable/status/image-missing.svg"
               "${CMAKE_SOURCE_DIR}/packaging/macos/entitlements.plist")
  add_custom_command(TARGET darktable POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/macos_development_bundle.cmake"
    COMMENT "Embedding and signing development application modules"
    VERBATIM)
  add_custom_target(darktable-development-bundle DEPENDS darktable)

  install(TARGETS darktable BUNDLE DESTINATION . COMPONENT DTApplication)
else()
  install(TARGETS darktable RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
endif()


dt_resolve_freecm_source_root(LIBRAW_PATH LIBRAW_SOURCE_ROOT LIBRAW_SOURCE_ROOT LibRaw libraw/libraw_version.h)
# LibRaw-cmake is a small build wrapper retained in-tree; give it the
# materialized FreeCM checkout explicitly instead of relying on its former
# adjacent-source layout.
set(LIBRAW_PATH "${LIBRAW_PATH}" CACHE PATH "Pinned LibRaw source root" FORCE)
if(MSVC)
  set(ENABLE_OPENMP OFF CACHE BOOL "" FORCE)
else()
  set(ENABLE_OPENMP ON CACHE BOOL "" FORCE)
endif()
set(ENABLE_LCMS OFF CACHE BOOL "")
set(ENABLE_JASPER OFF CACHE BOOL "")

# Build LibRaw from its pinned source root.
set(DARKTABLE_BUILD_SHARED_LIBS_SAVE "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
add_subdirectory(external/LibRaw-cmake)
set(BUILD_SHARED_LIBS "${DARKTABLE_BUILD_SHARED_LIBS_SAVE}" CACHE BOOL "Build shared libraries" FORCE)
unset(DARKTABLE_BUILD_SHARED_LIBS_SAVE)
target_link_libraries(lib_darktable PRIVATE libraw::libraw)
