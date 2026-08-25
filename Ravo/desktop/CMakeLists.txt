qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

add_library(ravo_desktop STATIC)

target_sources(ravo_desktop
  PRIVATE
    src/asset_list_model.cpp
    src/folder_list_model.cpp
    src/studio_command_controller.cpp
    src/studio_presenter.cpp
    src/studio_preview.cpp
    src/studio_develop.cpp
    include/ravo/desktop/asset_list_model.h
    include/ravo/desktop/folder_list_model.h
    include/ravo/desktop/studio_command_controller.h
    include/ravo/desktop/studio_presenter.h)

target_include_directories(ravo_desktop
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_desktop
  PUBLIC ravo_services ravo_adapters
  PRIVATE Qt6::Gui)

set_target_properties(ravo_desktop PROPERTIES AUTOMOC ON)
ravo_configure_target(ravo_desktop)

set(RAVO_APP_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/icons/AppIcon.png")
set(RAVO_APP_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/icons/AppIcon.icns")
set(RAVO_APP_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/icons/AppIcon.ico")
set(RAVO_WINDOWS_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/ravo_studio.rc")
set(RAVO_COMPANY "Ravo")
set(RAVO_DESCRIPTION "Ravo Studio photo library")
set(RAVO_COPYRIGHT "Copyright (c) 2026 Ravo contributors")

foreach(_ravo_icon IN ITEMS
    "${RAVO_APP_ICON_PNG}"
    "${RAVO_APP_ICON_ICNS}"
    "${RAVO_APP_ICON_ICO}")
  if(NOT EXISTS "${_ravo_icon}")
    message(FATAL_ERROR "Ravo Studio app icon not found: ${_ravo_icon}")
  endif()
endforeach()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/windows_app.rc.in"
  "${RAVO_WINDOWS_RC_FILE}"
  @ONLY
  NEWLINE_STYLE WIN32)

set(RAVO_STUDIO_SOURCES src/main.cpp)
if(APPLE)
  list(APPEND RAVO_STUDIO_SOURCES "${RAVO_APP_ICON_ICNS}")
  set_source_files_properties("${RAVO_APP_ICON_ICNS}" PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
  list(APPEND RAVO_STUDIO_SOURCES "${RAVO_WINDOWS_RC_FILE}")
endif()

add_executable(ravo_studio ${RAVO_STUDIO_SOURCES})
target_link_libraries(ravo_studio
  PRIVATE
    ravo_desktop
    GeoControls::AppShell
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::QuickLayouts)

set_target_properties(ravo_studio PROPERTIES AUTOMOC ON)
if(APPLE)
  set_target_properties(ravo_studio PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos_bundle_info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Ravo Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.ravo.studio"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "${RAVO_COPYRIGHT}"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
    MACOSX_BUNDLE_INFO_STRING "${RAVO_DESCRIPTION}"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
endif()

ravo_configure_target(ravo_studio)
ravo_deploy_qt_core(ravo_studio)
if(APPLE)
  add_custom_command(
    TARGET ravo_studio
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/ravo_studio"
    VERBATIM)
endif()

qt_add_qml_module(ravo_studio
  URI Ravo.Studio
  VERSION 1.0
  DEPENDENCIES
    GeoControls
    GeoControls.AppShell
  QML_FILES
    qml/Main.qml
    qml/theme/DarkThemePalette.qml
    qml/gallery/LibrarySidePanel.qml
    qml/gallery/FilmStripBar.qml
    qml/gallery/GalleryReviewBar.qml
    qml/gallery/ThumbnailCell.qml
    qml/inspect/InspectorSidePanel.qml
    qml/inspect/PhotoInfoPanel.qml
    qml/inspect/DevelopPanel.qml
    qml/inspect/ToneCurveEditor.qml
    qml/inspect/ScopePanel.qml
    qml/chrome/FolderDialogPage.qml
    qml/chrome/SettingsPage.qml
    qml/chrome/StudioActions.qml
    qml/chrome/StudioCommandMenuItems.qml
    qml/chrome/StudioCommandPalette.qml
    qml/chrome/StudioCommandShortcuts.qml
    qml/chrome/StudioMenuBar.qml
    qml/chrome/PhotoContextMenu.qml)

qt_add_resources(ravo_studio "studio_app_icon"
  PREFIX "/ravo/studio"
  FILES icons/AppIcon.png icons/AppIcon.ico)

if(NOT Python3_Interpreter_FOUND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
add_custom_command(
  TARGET ravo_studio
  POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/smoke_ravo_studio.py"
          "$<TARGET_FILE:ravo_studio>"
  COMMENT "Smoke-load Ravo Studio QML and exit"
  VERBATIM)
