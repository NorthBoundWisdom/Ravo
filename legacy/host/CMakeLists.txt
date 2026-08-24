cmake_minimum_required(VERSION 3.26)

# CMake 3.27+ recognizes upper-case <PACKAGENAME>_ROOT hints.  Keep the
# behavior consistent when configuring with newer CMake releases.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

### Add defaults for cmake
# These defaults need to be included before the project() call.
include(DefineCMakeDefaults)

If(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not permitted. Make a separate folder for building:\nmkdir build; cd build; cmake ..\nBefore that, remove the files already created:\nrm -rf CMakeCache.txt CMakeFiles")
endif(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)

if(DEFINED PROJECT_VERSION)
  project(darktable VERSION ${PROJECT_VERSION} LANGUAGES CXX C)
else()
  project(darktable VERSION 0.9.0 LANGUAGES CXX C)
endif()

# All first-party C targets, including those created before src/, use the
# same portable language baseline.  Compiler-specific capabilities remain
# separately detected where C11 has no equivalent.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Tests are opt-in: application presets must not pull test-only dependencies
# or assets into a normal configure/build.
option(BUILD_TESTING "Build DarkTableNext tests" OFF)
option(DT_MSVC_STRICT_CONVERSIONS
       "Enable broad MSVC implicit numeric conversion diagnostics" OFF)

# Loadable modules validate their own DT_MODULE_VERSION at runtime. Re-linking every
# module for a private lib_darktable implementation change only rewrites the module
# files and turns the next launch into a cold dynamic-loader pass. Keep that legacy
# behaviour available for conservative ABI investigations, but leave normal builds
# to rebuild modules when their own sources or ABI headers change.
option(DT_MODULE_RELINK_ON_CORE_CHANGE
       "Relink loadable modules whenever a linked shared library changes" OFF)

function(dt_module_configure_link_dependencies target)
  set_property(GLOBAL APPEND PROPERTY DT_LOADABLE_MODULE_TARGETS ${target})
  dt_target_enable_warnings(${target})
  if(NOT DT_MODULE_RELINK_ON_CORE_CHANGE)
    set_property(TARGET ${target} PROPERTY LINK_DEPENDS_NO_SHARED TRUE)
  endif()
endfunction()

if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU|MSVC)$")
  message(FATAL_ERROR "Unsupported C compiler: ${CMAKE_C_COMPILER_ID}. Use Clang, GCC, or MSVC.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU|MSVC)$")
  message(FATAL_ERROR "Unsupported C++ compiler: ${CMAKE_CXX_COMPILER_ID}. Use Clang, GCC, or MSVC.")
endif()

if(MSVC)
  add_compile_options("/FI${CMAKE_CURRENT_SOURCE_DIR}/src/win_msvc_compat.h"
                      "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
                      /utf-8)
  if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/Zc:preprocessor>"
                        "$<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>")
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>")
  endif()
  add_compile_definitions(_CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
                          _ITERATOR_DEBUG_LEVEL=0)
endif()

include(CheckCSourceCompiles)
check_c_source_compiles(
  "#include <complex.h>
   int main(void)
   {
     const float complex value = 1.0f + 2.0f * I;
     return crealf(value) == 1.0f && cimagf(value) == 2.0f ? 0 : 1;
   }"
  DT_HAVE_C99_COMPLEX)
if(NOT DT_HAVE_C99_COMPLEX)
  add_compile_definitions(DT_NO_C99_COMPLEX)
endif()

include(DefineOptions.cmake)

# Include GNUInstallDirs, which sets sensible defaults for install directories.
# See https://cmake.org/cmake/help/v3.0/module/GNUInstallDirs.html for further information.
# These values can be easily overridden if required.
# Some defaults are set for OpenBSD as well (info and man pages).

include(GNUInstallDirs)
include(FeatureSummary)

# resolve symbolic links into real path?
get_filename_component(CMAKE_INSTALL_BINDIR_ABS "${CMAKE_INSTALL_FULL_BINDIR}" REALPATH "${CMAKE_INSTALL_PREFIX}")
# get relative paths specific to the system
file(RELATIVE_PATH REL_BIN_TO_LIBDIR    ${CMAKE_INSTALL_FULL_BINDIR} "${CMAKE_INSTALL_FULL_LIBDIR}/darktable")
file(RELATIVE_PATH REL_BIN_TO_SHAREDIR  ${CMAKE_INSTALL_FULL_BINDIR} "${CMAKE_INSTALL_FULL_DATADIR}")
set(REL_BIN_TO_DATADIR  "${REL_BIN_TO_SHAREDIR}/darktable")
set(DARKTABLE_BINDIR    "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}")
set(DARKTABLE_LIBDIR    "${DARKTABLE_BINDIR}/${REL_BIN_TO_LIBDIR}")
set(DARKTABLE_SHAREDIR  "${DARKTABLE_BINDIR}/${REL_BIN_TO_SHAREDIR}")
set(DARKTABLE_DATADIR   "${DARKTABLE_BINDIR}/${REL_BIN_TO_DATADIR}") # DATAROOTDIR == DATADIR # darktablerc, themes, icons, ... Cannot use ${CMAKE_BINDIR}/data because not everything resides there. Like icons (pixmaps)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# macOS uses loader-relative install names for bundled libraries and modules.
if(APPLE)
  set(CMAKE_MACOSX_RPATH ON)
  set(RPATH_ORIGIN @loader_path)
endif()

check_c_compiler_flag("-fopenmp-version=51" DT_C_HAS_OPENMP_51_FLAG)
if(DT_C_HAS_OPENMP_51_FLAG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fopenmp-version=51")
endif()
check_cxx_compiler_flag("-fopenmp-version=51" DT_CXX_HAS_OPENMP_51_FLAG)
if(DT_CXX_HAS_OPENMP_51_FLAG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp-version=51")
endif()

if(MSVC)
  # MSVC does not implement the OpenMP declare-simd directives used by the
  # image core. Keep the portable serial paths active for this bootstrap.
  set(OpenMP_C_FLAGS "" CACHE STRING "" FORCE)
  set(OpenMP_CXX_FLAGS "" CACHE STRING "" FORCE)
  set(OpenMP_C_LIBRARIES "" CACHE STRING "" FORCE)
  set(OpenMP_CXX_LIBRARIES "" CACHE STRING "" FORCE)
  set(OpenMP_FOUND FALSE)
else()
  find_package(OpenMP 4.5 REQUIRED)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  foreach(_openmp_library IN LISTS OpenMP_C_LIBRARIES OpenMP_CXX_LIBRARIES)
    get_filename_component(_openmp_library_extension "${_openmp_library}" LAST_EXT)
    if(IS_ABSOLUTE "${_openmp_library}" AND
       _openmp_library_extension STREQUAL "${CMAKE_SHARED_LIBRARY_SUFFIX}")
      get_filename_component(_openmp_library_dir "${_openmp_library}" DIRECTORY)
      list(APPEND CMAKE_BUILD_RPATH "${_openmp_library_dir}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES CMAKE_BUILD_RPATH)
endif()

# Check for base threading library
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
find_package(Threads REQUIRED)

# Fail if returned library is not pthread on platforms that use POSIX threads.
if(NOT WIN32 AND NOT CMAKE_USE_PTHREADS_INIT)
    message(FATAL_ERROR "POSIX threads: not found")
endif()

#
# Set platform defaults...
#
if(APPLE)
  message("-- macOS build")
  find_program(HOMEBREW_EXISTS "brew")
  if(NOT HOMEBREW_EXISTS)
    message(FATAL_ERROR "DarkTableNext currently requires Homebrew on macOS.")
  endif()
  execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HOMEBREW_PREFIX)
  string(STRIP "${HOMEBREW_PREFIX}" HOMEBREW_PREFIX)
  message("-- Homebrew detected, setting CMAKE_PREFIX_PATH to ${HOMEBREW_PREFIX}")
  list(APPEND CMAKE_PREFIX_PATH ${HOMEBREW_PREFIX} ${HOMEBREW_PREFIX}/opt/llvm ${HOMEBREW_PREFIX}/opt/icu4c)
  set(CMAKE_FIND_FRAMEWORK "LAST")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_DARWIN_C_SOURCE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DARWIN_C_SOURCE")
elseif(WIN32)
  message("-- Windows build")
else()
  message("-- ${CMAKE_SYSTEM_NAME} build")
endif()

#
# Set package version
#
file(MAKE_DIRECTORY "${DARKTABLE_BINDIR}/") # the bin/ subdirectory won't exist yet

# Runtime persistence behavior is deliberately a build input rather than an
# application-version heuristic.  The ignored active source-roots lock may
# enable per-checkout developer state; the committed template remains the
# production-safe default.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(DT_BUILD_RUNTIME_CONFIG "${DARKTABLE_BINDIR}/build_runtime_config.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/configs/generate_build_runtime_config.py"
    "${CMAKE_SOURCE_DIR}/configs/source_roots.py"
    "${CMAKE_SOURCE_DIR}/source_roots.lock.jsonc.in"
    "${CMAKE_SOURCE_DIR}/source_roots.lock.jsonc")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/generate_build_runtime_config.py"
            --repo-root "${CMAKE_SOURCE_DIR}" --output "${DT_BUILD_RUNTIME_CONFIG}"
    RESULT_VARIABLE DT_RUNTIME_CONFIG_RESULT
    OUTPUT_VARIABLE DT_RUNTIME_CONFIG_OUTPUT
    ERROR_VARIABLE DT_RUNTIME_CONFIG_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT DT_RUNTIME_CONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR
      "Unable to generate the DarkTableNext runtime mode configuration:\n${DT_RUNTIME_CONFIG_ERROR}")
endif()
message(STATUS "${DT_RUNTIME_CONFIG_OUTPUT}")

# Adds a custom command to generate the version source from PROJECT_VERSION.
function(generate_version_gen_c)
  set(_VERSION "${ARGV0}")
  set(_TYPE "${ARGV1}")

  add_custom_target(
    create_version_gen ALL
    COMMAND sh ${CMAKE_SOURCE_DIR}/tools/create_version_c.sh ${DARKTABLE_BINDIR}/version_gen.c ${_VERSION}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/create_version_c.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Updating version string (${_TYPE})"
    VERBATIM # else might break when export-subst was needed but did not happen
  )
endfunction(generate_version_gen_c)

generate_version_gen_c(${PROJECT_VERSION} "project version")

# needed to make sure that version string is actually updated.
add_custom_command(
  OUTPUT ${DARKTABLE_BINDIR}/version_gen.c
  COMMAND ${CMAKE_COMMAND} -E echo
  DEPENDS create_version_gen
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# WARNING: no target should reference version_gen.c directly. instead, they should add_dependencies(yourtarget generate_version)
add_custom_target(
  generate_version ALL
  DEPENDS ${DARKTABLE_BINDIR}/version_gen.c
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Add a sensible build type default and warning because empty means no optimization and no debug info.
if(NOT CMAKE_BUILD_TYPE)
	message("WARNING: CMAKE_BUILD_TYPE is not defined!\n         Defaulting to CMAKE_BUILD_TYPE=RelWithDebInfo. Use ccmake to set a proper value.")
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)

include(compiler-warnings)

# we need some external programs for building darktable
message(STATUS "Looking for external programs")

# we need perl for introspection
find_program(perl_BIN perl REQUIRED)
message(STATUS "Found perl")

if(TESTBUILD_OPENCL_PROGRAMS)
  set(TESTBUILD_OPENCL_PROGRAMS OFF)

  find_package(LLVM CONFIG)

  if(LLVM_FOUND)
    if(${LLVM_VERSION} VERSION_LESS 7)
      set(LLVM_FOUND FALSE)
    endif()
  endif()

  if (LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

    find_program(CLANG_OPENCL_COMPILER
      NAMES clang-${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR} clang-${LLVM_PACKAGE_VERSION} clang${LLVM_VERSION_MAJOR}${LLVM_VERSION_MINOR} clang-${LLVM_VERSION_MAJOR} clang${LLVM_VERSION_MAJOR}
    )

    if (NOT ${CLANG_OPENCL_COMPILER} STREQUAL "CLANG_OPENCL_COMPILER-NOTFOUND")
      message(STATUS "Found clang compiler - ${CLANG_OPENCL_COMPILER}")

      find_path(CLANG_OPENCL_INCLUDE_DIR opencl-c.h
        HINTS ${LLVM_INSTALL_PREFIX}/lib/clang ${LLVM_INSTALL_PREFIX}/lib64/clang
        PATH_SUFFIXES include ${LLVM_PACKAGE_VERSION}/include ${LLVM_VERSION_MAJOR}/include
        NO_DEFAULT_PATH
      )

      if (NOT ${CLANG_OPENCL_INCLUDE_DIR} STREQUAL "CLANG_OPENCL_INCLUDE_DIR-NOTFOUND")
        message(STATUS "Found clang opencl-c.h header in ${CLANG_OPENCL_INCLUDE_DIR}")
        set(TESTBUILD_OPENCL_PROGRAMS ON)
      else()
        message(WARNING "Could not find clang opencl-c.h header include dir")
        message(WARNING "Test-compilation of OpenCL programs can not be done.")
      endif()
    else()
      message(WARNING "Could not find appropriate clang compiler")
      message(WARNING "Test-compilation of OpenCL programs can not be done.")
    endif()
  else()
    message(WARNING "Could not find LLVM 7 or above")
    message(WARNING "Test-compilation of OpenCL programs can not be done.")
  endif()
endif()

if(TESTBUILD_OPENCL_PROGRAMS)
  message(STATUS "Will be able to test-compile OpenCL programs. Nice.")
else()
  message(STATUS "Test-compilation of OpenCL programs is unavailable.")
endif()

# we need jsonschema to check noiseprofiles.json
find_program(jsonschema_BIN jsonschema)
if(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")
  message(STATUS "Missing jsonschema, problems in noiseprofiles.json might go unnoticed")
  set(VALIDATE_JSON 0)
else(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")
  message(STATUS "Found jsonschema")
  set(VALIDATE_JSON 1)
endif(${jsonschema_BIN} STREQUAL "jsonschema_BIN-NOTFOUND")

# xsltproc generates the checked build artifacts from the configuration schema.
find_program(Xsltproc_BIN xsltproc REQUIRED)

# xmllint validates the generated configuration schema during every build.
find_program(Xmllint_BIN xmllint REQUIRED)
message(STATUS "Found xmllint")

# done with looking for programs
message(STATUS "All external programs found")

# OpenCL is a required runtime capability.
SET(HAVE_OPENCL 1)

if(BUILD_TESTING)
  find_package(cmocka 1.1.0 CONFIG REQUIRED)
  include(AddCMockaTest)
  include_directories(${CMOCKA_INCLUDE_DIR} include)
endif(BUILD_TESTING)

# macOS and Linux use the POSIX 2008 interfaces throughout the core.
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_definitions("-D_XOPEN_SOURCE=700")
endif()

# Set default component name - that way external modules like RawSpeed will install their
# materials under the default component and not under 'Unspecified'
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME DTApplication)

# lets continue into build directories
include(data/supported_extensions.cmake) # this file needs to be included first as it gets amended in bin/
add_subdirectory(src "${DARKTABLE_BINDIR}") # directory needs to be there before data directory so that the correct CSS file gets installed
add_subdirectory(data "${DARKTABLE_DATADIR}")

# FreeCM owns the final macOS deployment step.  Prepare a clean staging prefix
# through the normal install graph first, then hand its native application
# bundle to the generic (non-Qt) macOS packager.
if(APPLE)
set(DARKTABLE_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package")
set(DARKTABLE_PACKAGE_STAGE "${DARKTABLE_PACKAGE_ROOT}/stage")
set(DARKTABLE_PACKAGE_DIST_DIR "${DARKTABLE_PACKAGE_ROOT}/dist")
set(DT_MACOS_CODESIGN_IDENTITY "-" CACHE STRING
    "codesign identity for the macOS package ('-' keeps developer builds ad hoc signed)")
find_program(DT_PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
execute_process(
    COMMAND "${DT_PKG_CONFIG_EXECUTABLE}" --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0
    OUTPUT_VARIABLE DARKTABLE_GDK_PIXBUF_MODULE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT IS_DIRECTORY "${DARKTABLE_GDK_PIXBUF_MODULE_DIR}")
  message(FATAL_ERROR
      "gdk-pixbuf loader directory was not found: ${DARKTABLE_GDK_PIXBUF_MODULE_DIR}")
endif()
get_filename_component(DARKTABLE_GDK_PIXBUF_MODULE_ROOT
    "${DARKTABLE_GDK_PIXBUF_MODULE_DIR}" DIRECTORY)
set(DARKTABLE_GDK_PIXBUF_MODULE_CACHE
    "${DARKTABLE_GDK_PIXBUF_MODULE_ROOT}/loaders.cache")
if(NOT EXISTS "${DARKTABLE_GDK_PIXBUF_MODULE_CACHE}")
  message(FATAL_ERROR
      "gdk-pixbuf loader cache was not found: ${DARKTABLE_GDK_PIXBUF_MODULE_CACHE}")
endif()
if(CMAKE_OSX_ARCHITECTURES)
  list(JOIN CMAKE_OSX_ARCHITECTURES "-" DARKTABLE_PACKAGE_ARCH)
else()
  set(DARKTABLE_PACKAGE_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
set(DARKTABLE_PACKAGE_DMG
    "${CMAKE_BINARY_DIR}/darktable-${PROJECT_VERSION}-macos-${DARKTABLE_PACKAGE_ARCH}.dmg")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/package.json.in"
    "${DARKTABLE_PACKAGE_ROOT}/package.json"
    @ONLY)

add_custom_target(prepare-macos-package
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${DARKTABLE_PACKAGE_STAGE}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${DARKTABLE_PACKAGE_STAGE}"
    DEPENDS darktable-development-bundle
    COMMENT "Preparing the darktable macOS package staging bundle"
    VERBATIM
    USES_TERMINAL)

include("${CMAKE_SOURCE_DIR}/FreeCM/repomgrcpp/cmake/CppKitPackage.cmake")
cppkit_add_package_deploy(prepare-macos-package
    CONFIG "${DARKTABLE_PACKAGE_ROOT}/package.json"
    PLATFORM mac
    TARGET_NAME package-macos)

# The native app target is installed by src/CMakeLists.txt.  All project
# libraries, modules and data have been installed by this point, so the final
# script can replace the build-tree convenience links with real bundle
# contents.  FreeCM then adds the third-party GTK/Homebrew dependency closure.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos_bundle_install.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/macos_bundle_install.cmake"
    @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/macos_bundle_install.cmake"
        COMPONENT DTApplication)
endif()

# Linux packages use the normal install graph for first-party files. A
# GTK-aware FreeCM package script adds the Homebrew runtime closure and creates
# the AppImage using assets pinned by source_roots.lock.jsonc.in.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DARKTABLE_LINUX_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package/linux")
  set(DARKTABLE_LINUX_APPDIR "${DARKTABLE_LINUX_PACKAGE_ROOT}/AppDir")
  set(DARKTABLE_LINUX_PACKAGE_WORK "${DARKTABLE_LINUX_PACKAGE_ROOT}/tools")
  set(DARKTABLE_LINUX_APPIMAGE
      "${CMAKE_BINARY_DIR}/DarkTableNext-${PROJECT_VERSION}-x86_64.AppImage")
  set(DARKTABLE_APPIMAGE_ASSET_ROOT
      "${CMAKE_SOURCE_DIR}/build/dependency_seed_repos/appimage-tools")
  set(DARKTABLE_APPIMAGETOOL
      "${DARKTABLE_APPIMAGE_ASSET_ROOT}/appimagetool-x86_64.AppImage")
  set(DARKTABLE_APPIMAGE_RUNTIME
      "${DARKTABLE_APPIMAGE_ASSET_ROOT}/runtime-x86_64")

  find_program(DT_LINUX_PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
  find_program(DT_GDK_PIXBUF_QUERY_LOADERS gdk-pixbuf-query-loaders REQUIRED)
  find_program(DT_GTK_QUERY_IMMODULES gtk-query-immodules-3.0 REQUIRED)
  find_program(DT_GLIB_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)
  find_program(DT_HOMEBREW_EXECUTABLE brew REQUIRED)

  function(dt_linux_pkg_config_variable output_variable package variable)
    execute_process(
        COMMAND "${DT_LINUX_PKG_CONFIG_EXECUTABLE}" --variable=${variable} ${package}
        OUTPUT_VARIABLE value
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(value STREQUAL "")
      message(FATAL_ERROR
          "pkg-config returned an empty ${variable} for ${package}")
    endif()
    set(${output_variable} "${value}" PARENT_SCOPE)
  endfunction()

  dt_linux_pkg_config_variable(
      DARKTABLE_GDK_PIXBUF_MODULE_DIR gdk-pixbuf-2.0 gdk_pixbuf_moduledir)
  dt_linux_pkg_config_variable(DARKTABLE_GTK_LIBDIR gtk+-3.0 libdir)
  dt_linux_pkg_config_variable(
      DARKTABLE_GTK_BINARY_VERSION gtk+-3.0 gtk_binary_version)
  dt_linux_pkg_config_variable(DARKTABLE_GLIB_PREFIX glib-2.0 prefix)
  dt_linux_pkg_config_variable(DARKTABLE_FONTCONFIG_SYSCONFDIR fontconfig sysconfdir)
  execute_process(
      COMMAND "${DT_LINUX_PKG_CONFIG_EXECUTABLE}" --variable=giomoduledir gio-2.0
      OUTPUT_VARIABLE DARKTABLE_GIO_MODULE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
  if(NOT IS_DIRECTORY "${DARKTABLE_GIO_MODULE_DIR}")
    set(DARKTABLE_GIO_MODULE_DIR "${DARKTABLE_GLIB_PREFIX}/lib/gio/modules")
  endif()
  execute_process(
      COMMAND "${DT_HOMEBREW_EXECUTABLE}" --prefix
      OUTPUT_VARIABLE DARKTABLE_HOMEBREW_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
      COMMAND "${DT_HOMEBREW_EXECUTABLE}" --cellar
      OUTPUT_VARIABLE DARKTABLE_HOMEBREW_CELLAR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
  set(DARKTABLE_GLIB_SCHEMA_DIR
      "${DARKTABLE_GLIB_PREFIX}/share/glib-2.0/schemas")
  set(DARKTABLE_ICON_THEME_DIR "${DARKTABLE_HOMEBREW_PREFIX}/share/icons")
  set(DARKTABLE_FONTCONFIG_DIR "${DARKTABLE_FONTCONFIG_SYSCONFDIR}/fonts")
  get_filename_component(
      DARKTABLE_OPENMP_RUNTIME_DIR "${OpenMP_omp_LIBRARY}" DIRECTORY)

  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "package-linux is available only from a single-config Release preset")
  elseif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(STATUS "package-linux is unavailable: only x86_64 AppImage assets are pinned")
  else()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/package.json.in"
        "${DARKTABLE_LINUX_PACKAGE_ROOT}/package.json"
        @ONLY)

    get_property(DARKTABLE_LINUX_PACKAGE_TARGETS GLOBAL
                 PROPERTY DT_LOADABLE_MODULE_TARGETS)
    list(APPEND DARKTABLE_LINUX_PACKAGE_TARGETS
         darktable darktable-cli darktablerc_file)
    if(TARGET darktable-rs-identify)
      list(APPEND DARKTABLE_LINUX_PACKAGE_TARGETS darktable-rs-identify)
    endif()

    add_custom_target(prepare-linux-package
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${DARKTABLE_LINUX_APPDIR}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${DARKTABLE_LINUX_APPDIR}/usr"
        DEPENDS ${DARKTABLE_LINUX_PACKAGE_TARGETS}
        COMMENT "Preparing the DarkTableNext Linux AppDir"
        VERBATIM
        USES_TERMINAL)

    add_custom_target(package-linux
        COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}/FreeCM"
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/package.py"
                --config "${DARKTABLE_LINUX_PACKAGE_ROOT}/package.json"
        DEPENDS prepare-linux-package
        COMMENT "Creating the DarkTableNext Linux AppImage through FreeCM"
        VERBATIM
        USES_TERMINAL)
  endif()
endif()

# Windows packages use the normal install graph as their first-party payload.
# The package script then reuses FreeCM's dependency inspection and WiX
# fragment APIs to add the relocatable GTK runtime and produce an MSI.
if(WIN32)
  set(DARKTABLE_WINDOWS_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package/windows")
  set(DARKTABLE_WINDOWS_PACKAGE_STAGE "${DARKTABLE_WINDOWS_PACKAGE_ROOT}/stage")
  set(DARKTABLE_WINDOWS_PACKAGE_WORK "${DARKTABLE_WINDOWS_PACKAGE_ROOT}/wix")
  set(DARKTABLE_WINDOWS_PACKAGE_MSI
      "${CMAKE_BINARY_DIR}/DarkTableNext-${PROJECT_VERSION}-win64.msi")
  set(DARKTABLE_WINDOWS_UPGRADE_CODE
      "A7391ACD-F5E6-5EA2-B772-5E1C61383A32")

  if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR
        "Windows packaging requires the vcpkg installed root and target triplet from the FreeCM preset")
  endif()
  set(DARKTABLE_WINDOWS_RUNTIME_ROOT
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

  set(DT_WIX_BIN_DIR "C:/Program Files (x86)/WiX Toolset v3.14/bin" CACHE PATH
      "Directory containing the WiX v3 candle.exe and light.exe tools")
  find_program(DT_WIX_CANDLE_EXECUTABLE candle.exe HINTS "${DT_WIX_BIN_DIR}"
               NO_DEFAULT_PATH)
  find_program(DT_WIX_LIGHT_EXECUTABLE light.exe HINTS "${DT_WIX_BIN_DIR}"
               NO_DEFAULT_PATH)
  find_program(DT_GLIB_COMPILE_SCHEMAS_EXECUTABLE glib-compile-schemas.exe
               HINTS "${DARKTABLE_WINDOWS_RUNTIME_ROOT}/tools/glib"
               NO_DEFAULT_PATH)
  find_program(DT_DUMPBIN_EXECUTABLE dumpbin.exe)

  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "package-windows is available only from a single-config Release preset")
  elseif(DT_WIX_CANDLE_EXECUTABLE AND DT_WIX_LIGHT_EXECUTABLE AND
     DT_GLIB_COMPILE_SCHEMAS_EXECUTABLE AND DT_DUMPBIN_EXECUTABLE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/package.json.in"
        "${CMAKE_BINARY_DIR}/windows-package.json"
        @ONLY)

    get_property(DARKTABLE_WINDOWS_PACKAGE_TARGETS GLOBAL
                 PROPERTY DT_LOADABLE_MODULE_TARGETS)
    list(APPEND DARKTABLE_WINDOWS_PACKAGE_TARGETS darktable darktable-cli)
    if(TARGET darktable-rs-identify)
      # RawSpeed registers this helper in the install graph. The packaged
      # product does not expose it, but it must exist before CMake can stage
      # a clean build; package.py removes it from the final payload.
      list(APPEND DARKTABLE_WINDOWS_PACKAGE_TARGETS darktable-rs-identify)
    endif()

    add_custom_target(prepare-windows-package
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${DARKTABLE_WINDOWS_PACKAGE_STAGE}"
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${DARKTABLE_WINDOWS_PACKAGE_STAGE}"
        DEPENDS ${DARKTABLE_WINDOWS_PACKAGE_TARGETS}
        COMMENT "Preparing the DarkTableNext Windows package staging tree"
        VERBATIM
        USES_TERMINAL)

    add_custom_target(package-windows
        COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}/FreeCM"
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/package.py"
                --config "${CMAKE_BINARY_DIR}/windows-package.json"
        DEPENDS prepare-windows-package
        COMMENT "Creating the DarkTableNext Windows MSI through FreeCM and WiX"
        VERBATIM
        USES_TERMINAL)
  else()
    message(STATUS
        "package-windows is unavailable: install WiX Toolset v3.14, load the MSVC environment, and reconfigure")
  endif()
endif()

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    @ONLY)

if(NOT TARGET uninstall)
  add_custom_target(uninstall
      COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()

feature_summary(WHAT ALL)
