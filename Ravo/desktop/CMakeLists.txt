qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

add_library(ravo_desktop STATIC)

qt_add_resources(ravo_desktop "studio_locale_manifest"
  PREFIX "/ravo/studio/i18n"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
  FILES i18n/locales.json)

target_sources(ravo_desktop
  PRIVATE
    src/asset_list_model.cpp
    src/folder_list_model.cpp
    src/library_set_list_model.cpp
    src/import_candidate_list_model.cpp
    src/preview_request_owner.cpp
    src/studio_command_controller.cpp
    src/studio_command_internal.cpp
    src/studio_command_ids.h
    src/studio_command_presentation.cpp
    src/studio_file_manager.cpp
    src/studio_file_manager.h
    src/studio_assistant_controller.cpp
    src/studio_language_manager.cpp
    src/studio_live_session_controller.cpp
    src/studio_presenter.cpp
    src/studio_presenter_export.cpp
    src/studio_presenter_selection.cpp
    src/studio_import.cpp
    src/studio_recovery.cpp
    src/studio_preview.cpp
    src/studio_develop.cpp
    src/studio_develop_color_properties.cpp
    src/studio_develop_commands.cpp
    src/studio_develop_history.cpp
    src/studio_develop_internal.cpp
    src/studio_develop_properties.cpp
    src/studio_develop_raw_properties.cpp
    src/studio_develop_work.cpp
    src/export_option_conversion.cpp
    include/ravo/desktop/asset_list_model.h
    include/ravo/desktop/export_option_conversion.h
    include/ravo/desktop/folder_list_model.h
    include/ravo/desktop/library_set_list_model.h
    include/ravo/desktop/import_candidate_list_model.h
    include/ravo/desktop/preview_request_owner.h
    include/ravo/desktop/studio_assistant_controller.h
    include/ravo/desktop/studio_command_controller.h
    include/ravo/desktop/studio_live_session_controller.h
    include/ravo/desktop/studio_presenter.h
    src/studio_language_manager.h)

target_include_directories(ravo_desktop
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_desktop
  PUBLIC ravo_services ravo_adapters ravo_control Qt6::Network
  PRIVATE Qt6::Gui Qt6::Qml)

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
ravo_copy_windows_runtime_dlls(ravo_studio)
ravo_deploy_windows_qt_runtime(ravo_studio
  QML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qml")
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
    qml/gallery/LibraryFilterBar.qml
    qml/gallery/LibrarySidePanel.qml
    qml/gallery/FilmStripBar.qml
    qml/gallery/GalleryReviewBar.qml
    qml/gallery/PhotoInformationOverlay.qml
    qml/gallery/ThumbnailCell.qml
    qml/inspect/InspectorSidePanel.qml
    qml/inspect/PhotoInfoPanel.qml
    qml/inspect/DevelopHistoryPanel.qml
    qml/inspect/DevelopPresetPanel.qml
    qml/inspect/DevelopPanel.qml
    qml/inspect/DevelopSection.qml
    qml/inspect/CurveOptionButton.qml
    qml/inspect/MixerSlider.qml
    qml/inspect/PrimariesSlider.qml
    qml/inspect/MaskEditor.qml
    qml/inspect/ColorCheckerNumberField.qml
    qml/inspect/ColorContrastOffsetField.qml
    qml/inspect/DevelopLightSection.qml
    qml/inspect/DevelopCurvesSection.qml
    qml/inspect/DevelopColorCoreSection.qml
    qml/inspect/DevelopColorEqualizerSection.qml
    qml/inspect/DevelopColorAdvancedSection.qml
    qml/inspect/DevelopAdvancedLooks.qml
    qml/inspect/DevelopAdvancedToning.qml
    qml/inspect/DevelopAdvancedColorOps.qml
    qml/inspect/DevelopPrimariesSection.qml
    qml/inspect/DevelopGeometrySection.qml
    qml/inspect/DevelopToneEqualizerSection.qml
    qml/inspect/DevelopGraduatedSection.qml
    qml/inspect/DevelopEffectsSection.qml
    qml/inspect/DevelopDetailSection.qml
    qml/inspect/DevelopRawSection.qml
    qml/inspect/DevelopCalibrationSection.qml
    qml/inspect/DevelopInputProfileSection.qml
    qml/inspect/DevelopProfileGammaSection.qml
    qml/inspect/DevelopOutputProfileSection.qml
    qml/inspect/DevelopColorSlider.qml
    qml/inspect/ColorGradeWheel.qml
    qml/inspect/ToneCurveEditor.qml
    qml/inspect/ScopePanel.qml
    qml/chrome/FolderDialogPage.qml
    qml/chrome/BackupScheduleDialog.qml
    qml/chrome/ExportOptionsDialog.qml
    qml/chrome/SettingsPage.qml
    qml/chrome/ImportPage.qml
    qml/chrome/AssistantPanel.qml
    qml/chrome/StudioActions.qml
    qml/chrome/StudioCommandMenuItems.qml
    qml/chrome/StudioCommandPalette.qml
    qml/chrome/StudioCommandShortcuts.qml
    qml/chrome/StudioMenuBar.qml
    qml/chrome/StudioContextMenu.qml
    qml/chrome/StudioContextMenuItem.qml
    qml/chrome/StudioContextMenuSeparator.qml
    qml/chrome/StudioContextSubMenu.qml
    qml/chrome/PresetRenameDialog.qml
    qml/chrome/PresetSaveDialog.qml
    qml/chrome/PhotoContextMenu.qml)

qt_add_resources(ravo_studio "studio_app_icon"
  PREFIX "/ravo/studio"
  FILES icons/AppIcon.png icons/AppIcon.ico)

if(NOT Python3_Interpreter_FOUND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/StudioTranslations.cmake")
add_custom_command(
  TARGET ravo_studio
  POST_BUILD
  COMMAND
    "${CMAKE_COMMAND}" -E env
    QT_QPA_PLATFORM=offscreen
    QSG_RHI_BACKEND=software
    QT_QUICK_BACKEND=software
    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/smoke_ravo_studio.py"
    "$<TARGET_FILE:ravo_studio>"
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:ravo_studio>"
  COMMENT "Smoke-load Ravo Studio QML and exit"
  VERBATIM)
if(BUILD_TESTING)
  add_test(
    NAME ravo_studio_qml_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/smoke_ravo_studio.py"
            "$<TARGET_FILE:ravo_studio>")
  set_tests_properties(ravo_studio_qml_smoke PROPERTIES
    LABELS "ravo-desktop-smoke"
    TIMEOUT 90
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ravo_studio>"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_QUICK_BACKEND=software")
endif()
