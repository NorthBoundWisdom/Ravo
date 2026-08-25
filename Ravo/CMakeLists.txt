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

function(ravo_configure_target target)
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
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wshadow>)
endfunction()

function(ravo_deploy_qt_core target)
  if(WIN32)
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Core>
        $<TARGET_FILE_DIR:${target}>
      VERBATIM)
  endif()
endfunction()

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
