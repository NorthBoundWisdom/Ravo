set(DARKTABLE_SOURCE_ROOTS_DIR "${CMAKE_SOURCE_DIR}/build/dependency_source_roots" CACHE PATH
    "FreeCM materialized dependency source roots")

if(MSVC)
  # Dependency sources are managed by FreeCM and are not part of the
  # DarkTableNext warning baseline. Their public headers are still consumed as
  # external includes by first-party targets.
  add_compile_options(/w)
endif()

function(dt_resolve_freecm_source_root output_variable cache_variable environment_variable repository_name required_path)
  set(_source_root "")

  if(DEFINED ${cache_variable} AND NOT "${${cache_variable}}" STREQUAL "")
    set(_source_root "${${cache_variable}}")
  elseif(DEFINED ENV{${environment_variable}} AND NOT "$ENV{${environment_variable}}" STREQUAL "")
    set(_source_root "$ENV{${environment_variable}}")
  else()
    set(_source_root "${DARKTABLE_SOURCE_ROOTS_DIR}/${repository_name}")
  endif()

  if(NOT EXISTS "${_source_root}/${required_path}")
    message(FATAL_ERROR
      "FreeCM source root for ${repository_name} is unavailable at '${_source_root}'.\n"
      "Run: python3 configs/source_root_workflow.py --init && "
      "python3 configs/source_root_workflow.py --update\n"
      "Or configure with -D${cache_variable}=<source-root>.")
  endif()

  set(${output_variable} "${_source_root}" PARENT_SCOPE)
endfunction()

dt_resolve_freecm_source_root(WHEREAMI_PATH WHEREAMI_SOURCE_ROOT WHEREAMI_SOURCE_ROOT whereami src/whereami.c)
dt_resolve_freecm_source_root(OPENCL_PATH OPENCL_HEADERS_SOURCE_ROOT OPENCL_HEADERS_SOURCE_ROOT OpenCL CL/opencl.h)
dt_resolve_freecm_source_root(RAWSPEED_PATH RAWSPEED_SOURCE_ROOT RAWSPEED_SOURCE_ROOT rawspeed CMakeLists.txt)
set(OPENCL_PATH "${OPENCL_PATH}" PARENT_SCOPE)

# Homebrew C++ packages are built with libc++, whereas Homebrew GCC uses
# libstdc++. The pinned source-root build keeps the GCC ABI boundary coherent
# until FreeCM provides compiler-neutral C++ package artifacts.
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  dt_resolve_freecm_source_root(IMATH_PATH IMATH_SOURCE_ROOT IMATH_SOURCE_ROOT Imath CMakeLists.txt)
  dt_resolve_freecm_source_root(EXIV2_PATH EXIV2_SOURCE_ROOT EXIV2_SOURCE_ROOT exiv2 CMakeLists.txt)
  dt_resolve_freecm_source_root(INIH_PATH INIH_SOURCE_ROOT INIH_SOURCE_ROOT inih cpp/INIReader.cpp)

  message(STATUS "Building Imath and Exiv2 from FreeCM source roots for GCC ABI compatibility")

  set(IMATH_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${IMATH_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/Imath" EXCLUDE_FROM_ALL)
  # inih ships Meson metadata only. Build its C parser and C++ wrapper from
  # the locked source root, then satisfy Exiv2's standard Findinih module with
  # these targets instead of Homebrew's libc++ binaries.
  add_library(darktable_inih STATIC "${INIH_PATH}/ini.c")
  target_include_directories(darktable_inih PUBLIC "${INIH_PATH}")
  set_target_properties(darktable_inih PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(darktable_inireader STATIC "${INIH_PATH}/cpp/INIReader.cpp")
  target_include_directories(darktable_inireader PUBLIC "${INIH_PATH}/cpp")
  target_link_libraries(darktable_inireader PUBLIC darktable_inih)
  set_target_properties(darktable_inireader PROPERTIES POSITION_INDEPENDENT_CODE ON)

  set(inih_INCLUDE_DIR "${INIH_PATH}" CACHE PATH "" FORCE)
  set(inih_inireader_INCLUDE_DIR "${INIH_PATH}/cpp" CACHE PATH "" FORCE)
  set(inih_LIBRARY darktable_inih CACHE STRING "" FORCE)
  set(inih_inireader_LIBRARY darktable_inireader CACHE STRING "" FORCE)

  set(EXIV2_BUILD_EXIV2_COMMAND OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_FUZZ_TESTS OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_DOC OFF CACHE BOOL "" FORCE)
  add_subdirectory("${EXIV2_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/exiv2" EXCLUDE_FROM_ALL)

  set(DARKTABLE_MANAGED_CPP_DEPENDENCIES ON PARENT_SCOPE)
  set(DARKTABLE_MANAGED_EXIV2_INCLUDE_DIR "${EXIV2_PATH}/include" PARENT_SCOPE)
  set(DARKTABLE_MANAGED_EXIV2_CONFIG_HEADER "${CMAKE_BINARY_DIR}/exv_conf.h" PARENT_SCOPE)
endif()

add_library(whereami STATIC "${WHEREAMI_PATH}/src/whereami.c")
target_include_directories(whereami PUBLIC "${WHEREAMI_PATH}/src/")

################################################################################
# RawSpeed block

string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPERCASE)

set(RAWSPEED_${CMAKE_BUILD_TYPE_UPPERCASE}_BUILD 1)
if(RAWSPEED_RELWITHDEBINFO_BUILD)
  set(CMAKE_BUILD_TYPE "Release")
endif()

SET(COMPILER_SUPPORTS_DEBUG_INFO_COMPRESSION OFF)
SET(COMPILER_SUPPORTS_DEBUG_INFO_COMPRESSION_LINK OFF)
SET(COMPILER_SUPPORTS_SPLIT_DEBUG_INFO OFF)
SET(LINKER_SUPPORTS_GDB_INDEX OFF)

set(BUILD_TESTING_SAVE "${BUILD_TESTING}")
set(BUILD_BENCHMARKING_SAVE "${BUILD_BENCHMARKING}")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
if(MSVC)
  # RawSpeed applies an OpenMP `for` directive to a range-for loop, which the
  # MSVC OpenMP frontend rejects. Its sequential decoder path is equivalent.
  set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
else()
  set(WITH_OPENMP ON CACHE BOOL "" FORCE)
endif()
# RawSpeed's own warnings are useful upstream, but they are not part of the
# application's warning contract.
set(RAWSPEED_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(RAWSPEED_ENABLE_CLANG_TIDY_WERROR OFF CACHE BOOL "" FORCE)

# RawSpeed probes warning flags while -w is active. Clang therefore reports
# GCC's -Wstack-usage as supported, then warns about it for every source file
# once normal diagnostics are restored. Seed the probe result to its real
# value; the other stack-size warnings used by RawSpeed are supported by Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
  set(RAWSPEED_HAVE_CXX_FLAG_WSTACK_USAGE_4096 OFF CACHE INTERNAL
      "Clang does not support -Wstack-usage=4096" FORCE)
endif()

# RawSpeed's CheckZLIB.cmake appends Clang-only warning suppressions to its
# CMake probes. MSVC rejects those flags before it can test the vcpkg Zlib
# headers and library, which RawSpeed still resolves as a required package.
if(MSVC)
  foreach(_rawspeed_zlib_check
      HAVE_ZLIB_H
      HAVE_ZLIB_Z_OK
      HAVE_ZLIB_UNCOMPRESS_PROTOTYPE
      HAVE_ZLIB_ZERROR_PROTOTYPE
      HAVE_ZLIB_UNCOMPRESS
      HAVE_ZLIB_ZERROR)
    set(${_rawspeed_zlib_check} TRUE CACHE INTERNAL
        "Prevalidated by the vcpkg Zlib package for the MSVC RawSpeed build" FORCE)
  endforeach()

  # CheckTypeSize(uLongf HAVE_ZLIB_ULONGF) writes its try-compile status to
  # HAVE_HAVE_ZLIB_ULONGF, while RawSpeed consumes the size variable. vcpkg's
  # x64-windows zlib typedefs uLongf to the 32-bit unsigned long type.
  set(HAVE_HAVE_ZLIB_ULONGF TRUE CACHE INTERNAL
      "Prevalidated by the vcpkg Zlib package for the MSVC RawSpeed build" FORCE)
  set(HAVE_ZLIB_ULONGF 4 CACHE INTERNAL
      "sizeof(uLongf) in the vcpkg x64-windows Zlib package" FORCE)
endif()

set(RAWSPEED_BINARY_PREFIX "darktable")

if(NOT(EXISTS "${RAWSPEED_PATH}" AND IS_DIRECTORY "${RAWSPEED_PATH}" AND EXISTS "${RAWSPEED_PATH}/CMakeLists.txt"))
  message(FATAL_ERROR
    "RawSpeed FreeCM source root not found. Run: python3 configs/source_root_workflow.py --init && "
    "python3 configs/source_root_workflow.py --update")
endif()

set(CMAKE_INSTALL_DATAROOTDIR_SAVE "${CMAKE_INSTALL_DATAROOTDIR}")

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR}/darktable")

add_subdirectory("${RAWSPEED_PATH}" ${DARKTABLE_LIBDIR}/rawspeed)

# RawSpeed's public headers are dependency code as well. Mark the include paths
# as system paths when they propagate into DarkTableNext targets so the project
# warning baseline remains scoped to first-party sources.
get_target_property(_rawspeed_interface_includes rawspeed INTERFACE_INCLUDE_DIRECTORIES)
if(_rawspeed_interface_includes)
  set_property(TARGET rawspeed APPEND PROPERTY
               INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rawspeed_interface_includes}")
endif()

# Copying rawspeed resources to where dt expects them
add_custom_target("deploy" ALL)
SET(RAWSPEED_DATADIR "${RAWSPEED_PATH}/data")
add_custom_command(TARGET "deploy" PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_DATADIR}/rawspeed/
  COMMAND ${CMAKE_COMMAND} -E copy ${RAWSPEED_DATADIR}/cameras.xml ${RAWSPEED_DATADIR}/showcameras.xsl ${DARKTABLE_DATADIR}/rawspeed/
)

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR_SAVE}")

set(BUILD_TESTING "${BUILD_TESTING_SAVE}" CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING "${BUILD_BENCHMARKING_SAVE}" CACHE BOOL "" FORCE)
