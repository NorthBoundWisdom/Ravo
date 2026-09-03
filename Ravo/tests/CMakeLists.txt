find_package(GTest 1.17 CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(GoogleTest)

function(ravo_add_repository_check name script)
  add_test(
    NAME ${name}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/${script}"
      --repository-root "${CMAKE_SOURCE_DIR}" ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS "ravo-contract")
endfunction()

function(ravo_add_root_repository_check name script)
  add_test(
    NAME ${name}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/${script}"
      --repository-root "${CMAKE_SOURCE_DIR}" ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS "ravo-contract")
endfunction()

add_executable(ravo_unit_tests
  domain_test.cpp
  foundation_test.cpp
  recipe_test.cpp
  recipe_develop_test.cpp
  recipe_style_test.cpp)
target_link_libraries(ravo_unit_tests PRIVATE GTest::gtest_main ravo_domain ravo_recipe)
target_include_directories(ravo_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_unit_tests)
ravo_copy_windows_runtime_dlls(ravo_unit_tests)
gtest_discover_tests(ravo_unit_tests PROPERTIES LABELS "ravo-unit")

add_executable(ravo_export_metadata_encoder_tests export_metadata_encoder_test.cpp)
target_link_libraries(ravo_export_metadata_encoder_tests
  PRIVATE GTest::gtest_main ravo_adapters)
target_include_directories(ravo_export_metadata_encoder_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Ravo/adapters/src)
ravo_configure_target(ravo_export_metadata_encoder_tests)
ravo_copy_windows_runtime_dlls(ravo_export_metadata_encoder_tests)
gtest_discover_tests(ravo_export_metadata_encoder_tests PROPERTIES LABELS "ravo-unit")

add_executable(ravo_preview_cache_tests preview_cache_test.cpp)
target_link_libraries(ravo_preview_cache_tests
  PRIVATE GTest::gtest_main ravo_adapters Qt6::Core)
target_include_directories(ravo_preview_cache_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_preview_cache_tests)
ravo_copy_windows_runtime_dlls(ravo_preview_cache_tests)
gtest_discover_tests(ravo_preview_cache_tests PROPERTIES LABELS "ravo-unit")

add_executable(ravo_contract_tests
  cli_test_support.cpp
  engine_color_test_support.cpp
  engine_harmony_test_support.cpp
  engine_test_support.cpp
  cli_test.cpp
  cli_recipe_test.cpp
  cli_legacy_xmp_test.cpp
  cli_catalog_test.cpp
  cli_studio_test.cpp
  cli_lut_test.cpp
  crs_xmp_test.cpp
  bayer_demosaic_test.cpp
  canvas_frame_test.cpp
  color_correction_test.cpp
  color_harmonizer_test.cpp
  color_reconstruction_test.cpp
  color_zones_test.cpp
  dehaze_test.cpp
  dng_opcodes_test.cpp
  film_development_research_test.cpp
  mask_graph_test.cpp
  lut3d_test.cpp
  local_detail_research_test.cpp
  monochrome_test.cpp
  noise_calibration_test.cpp
  output_dither_test.cpp
  recursive_gaussian_test.cpp
  retouch_test.cpp
  sharpen_test.cpp
  split_toning_test.cpp
  texture_test.cpp
  velvia_test.cpp
  watermark_test.cpp
  xtrans_demosaic_test.cpp
  engine_test.cpp
  engine_raw_test.cpp
  engine_color_test.cpp
  engine_dt_ucs_test.cpp
  engine_harmony_geometry_test.cpp
  engine_color_harmonizer_test.cpp
  engine_color_harmonizer_smoothing_test.cpp
  engine_harmony_contrast_test.cpp
  engine_color_grading_test.cpp
  engine_controls_test.cpp
  engine_detail_test.cpp
  engine_raw_reference_test.cpp
  input_color_test.cpp
  jpeg_cli_test.cpp
  output_color_test.cpp
  perspective_test.cpp
  profile_gamma_test.cpp
  primaries_test.cpp)
target_link_libraries(ravo_contract_tests
  PRIVATE GTest::gtest_main ravo_cli ravo_libtiff Qt6::Gui ZLIB::ZLIB)
target_include_directories(ravo_contract_tests PRIVATE
  $<TARGET_PROPERTY:lcms2,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:PNG::PNG,INTERFACE_INCLUDE_DIRECTORIES>
  ${CMAKE_SOURCE_DIR}/Ravo/engine/src)
target_include_directories(ravo_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_contract_tests PRIVATE
  RAVO_CLI_EXECUTABLE="$<TARGET_FILE:ravo>"
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}"
  RAVO_TEST_VERSION="${PROJECT_VERSION}")
add_dependencies(ravo_contract_tests ravo)
ravo_configure_target(ravo_contract_tests)
ravo_copy_windows_runtime_dlls(ravo_contract_tests)
gtest_discover_tests(ravo_contract_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-contract")

add_executable(ravo_catalog_tests
  catalog_test_support.cpp
  catalog_test.cpp
  catalog_recovery_test.cpp
  catalog_library_test.cpp
  catalog_recipe_test.cpp
  catalog_develop_test.cpp
  catalog_develop_batch_test.cpp
  catalog_ai_proposal_test.cpp
  catalog_capture_test.cpp
  catalog_import_test.cpp
  catalog_xmp_interchange_test.cpp
  catalog_external_editor_test.cpp
  export_pro_export_test.cpp
  jpeg_adapter_test.cpp
  runtime_dependency_test.cpp)
target_link_libraries(ravo_catalog_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_catalog_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Ravo/adapters/src
  ${CMAKE_SOURCE_DIR}/Ravo/services/src)
target_compile_definitions(ravo_catalog_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_catalog_tests)
ravo_copy_windows_runtime_dlls(ravo_catalog_tests)
gtest_discover_tests(ravo_catalog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-catalog")

add_executable(ravo_png_adapter_tests png_adapter_test.cpp)
target_link_libraries(ravo_png_adapter_tests
  PRIVATE GTest::gtest ravo_adapters Qt6::Core Qt6::Gui ZLIB::ZLIB)
target_include_directories(ravo_png_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_png_adapter_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_png_adapter_tests)
ravo_copy_windows_runtime_dlls(ravo_png_adapter_tests)
gtest_discover_tests(ravo_png_adapter_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-png-adapter")

add_executable(ravo_png_export_tests png_export_test.cpp)
target_link_libraries(ravo_png_export_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_png_export_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_png_export_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_png_export_tests)
ravo_copy_windows_runtime_dlls(ravo_png_export_tests)
gtest_discover_tests(ravo_png_export_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-png-export")

add_executable(ravo_png_cli_tests png_cli_test.cpp)
target_link_libraries(ravo_png_cli_tests
  PRIVATE
    GTest::gtest
    ravo_cli
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_png_cli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_png_cli_tests)
ravo_copy_windows_runtime_dlls(ravo_png_cli_tests)
gtest_discover_tests(ravo_png_cli_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-png-cli")

add_executable(ravo_tiff_export_tests tiff_export_test.cpp)
target_link_libraries(ravo_tiff_export_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_tiff_export_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_tiff_export_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_tiff_export_tests)
ravo_copy_windows_runtime_dlls(ravo_tiff_export_tests)
gtest_discover_tests(ravo_tiff_export_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-tiff-export")

add_executable(ravo_tiff_metadata_export_tests tiff_metadata_export_test.cpp)
target_link_libraries(ravo_tiff_metadata_export_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_tiff_metadata_export_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_tiff_metadata_export_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_tiff_metadata_export_tests)
ravo_copy_windows_runtime_dlls(ravo_tiff_metadata_export_tests)
gtest_discover_tests(ravo_tiff_metadata_export_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-tiff-metadata-export")

add_executable(ravo_tiff_cli_tests tiff_cli_test.cpp)
target_link_libraries(ravo_tiff_cli_tests
  PRIVATE
    GTest::gtest
    ravo_cli
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    ZLIB::ZLIB)
target_include_directories(ravo_tiff_cli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_tiff_cli_tests)
ravo_copy_windows_runtime_dlls(ravo_tiff_cli_tests)
gtest_discover_tests(ravo_tiff_cli_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-tiff-cli")

add_executable(ravo_tiff_adapter_tests tiff_adapter_test.cpp)
target_link_libraries(ravo_tiff_adapter_tests
  PRIVATE GTest::gtest ravo_adapters Qt6::Core Qt6::Gui ZLIB::ZLIB lcms2)
target_include_directories(ravo_tiff_adapter_tests PRIVATE
  $<TARGET_PROPERTY:lcms2,INTERFACE_INCLUDE_DIRECTORIES>
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_tiff_adapter_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_tiff_adapter_tests)
ravo_copy_windows_runtime_dlls(ravo_tiff_adapter_tests)
gtest_discover_tests(ravo_tiff_adapter_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-tiff-adapter")

add_executable(ravo_qoi_adapter_tests qoi_adapter_test.cpp)
target_link_libraries(ravo_qoi_adapter_tests
  PRIVATE GTest::gtest ravo_adapters Qt6::Core Qt6::Gui)
target_include_directories(ravo_qoi_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_qoi_adapter_tests)
ravo_copy_windows_runtime_dlls(ravo_qoi_adapter_tests)
gtest_discover_tests(ravo_qoi_adapter_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-qoi-adapter")

add_executable(ravo_qoi_catalog_tests qoi_catalog_test.cpp)
target_link_libraries(ravo_qoi_catalog_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql)
target_include_directories(ravo_qoi_catalog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_qoi_catalog_tests)
ravo_copy_windows_runtime_dlls(ravo_qoi_catalog_tests)
gtest_discover_tests(ravo_qoi_catalog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-qoi-catalog")

add_executable(ravo_heic_adapter_tests heic_adapter_test.cpp)
target_link_libraries(ravo_heic_adapter_tests
  PRIVATE GTest::gtest ravo_adapters Qt6::Core Qt6::Gui)
target_include_directories(ravo_heic_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_heic_adapter_tests)
ravo_copy_windows_runtime_dlls(ravo_heic_adapter_tests)
gtest_discover_tests(ravo_heic_adapter_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-heic-adapter")

add_executable(ravo_heic_catalog_tests heic_catalog_test.cpp)
target_link_libraries(ravo_heic_catalog_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql)
target_include_directories(ravo_heic_catalog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_heic_catalog_tests)
ravo_copy_windows_runtime_dlls(ravo_heic_catalog_tests)
gtest_discover_tests(ravo_heic_catalog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-heic-catalog")

add_executable(ravo_rgbe_adapter_tests rgbe_adapter_test.cpp)
target_link_libraries(ravo_rgbe_adapter_tests
  PRIVATE GTest::gtest ravo_adapters Qt6::Core)
target_include_directories(ravo_rgbe_adapter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_rgbe_adapter_tests)
ravo_copy_windows_runtime_dlls(ravo_rgbe_adapter_tests)
gtest_discover_tests(ravo_rgbe_adapter_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-rgbe-adapter")

add_executable(ravo_rgbe_catalog_tests rgbe_catalog_test.cpp)
target_link_libraries(ravo_rgbe_catalog_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql)
target_include_directories(ravo_rgbe_catalog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_rgbe_catalog_tests)
ravo_copy_windows_runtime_dlls(ravo_rgbe_catalog_tests)
gtest_discover_tests(ravo_rgbe_catalog_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-rgbe-catalog")

add_executable(ravo_original_copy_service_tests original_copy_service_test.cpp)
target_link_libraries(ravo_original_copy_service_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql)
target_include_directories(ravo_original_copy_service_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/Ravo/services/src
  ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_original_copy_service_tests)
ravo_copy_windows_runtime_dlls(ravo_original_copy_service_tests)
gtest_discover_tests(ravo_original_copy_service_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-original-copy-service")

add_executable(ravo_encoded_publication_tests encoded_publication_test.cpp)
target_link_libraries(ravo_encoded_publication_tests PRIVATE GTest::gtest_main ravo_services)
target_include_directories(ravo_encoded_publication_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/Ravo/services/src
  ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_encoded_publication_tests)
ravo_copy_windows_runtime_dlls(ravo_encoded_publication_tests)
gtest_discover_tests(ravo_encoded_publication_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-encoded-publication")

add_executable(ravo_original_copy_cli_tests original_copy_cli_test.cpp)
target_link_libraries(ravo_original_copy_cli_tests
  PRIVATE GTest::gtest ravo_cli Qt6::Core Qt6::Gui Qt6::Sql)
target_include_directories(ravo_original_copy_cli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_original_copy_cli_tests)
ravo_copy_windows_runtime_dlls(ravo_original_copy_cli_tests)
gtest_discover_tests(ravo_original_copy_cli_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-original-copy-cli")

add_executable(ravo_desktop_command_tests
  studio_qml_test_support.cpp
  studio_test_support.cpp
  studio_command_test.cpp
  studio_presenter_test.cpp
  studio_qml_contract_test.cpp
  studio_live_control_test.cpp
  studio_assistant_test.cpp
  studio_window_geometry_test.cpp
  export_option_conversion_test.cpp
  filesystem_browser_model_test.cpp)
target_link_libraries(ravo_desktop_command_tests
  PRIVATE GTest::gtest_main ravo_desktop Qt6::Core Qt6::Gui)
target_include_directories(ravo_desktop_command_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Ravo/desktop/src)
target_compile_definitions(ravo_desktop_command_tests PRIVATE
  RAVO_CLI_EXECUTABLE="$<TARGET_FILE:ravo>"
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}"
  RAVO_STUDIO_TRANSLATION_DIR="${CMAKE_BINARY_DIR}/Ravo/desktop/i18n"
  RAVO_STUDIO_DEVELOP_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/DevelopPanel.qml"
  RAVO_STUDIO_DEVELOP_COLOR_SLIDER_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/DevelopColorSlider.qml"
  RAVO_STUDIO_COLOR_GRADE_WHEEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/ColorGradeWheel.qml"
  RAVO_STUDIO_DEVELOP_HISTORY_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/DevelopHistoryPanel.qml"
  RAVO_STUDIO_DEVELOP_PRESET_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/DevelopPresetPanel.qml"
  RAVO_STUDIO_SCOPE_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/inspect/ScopePanel.qml"
  RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/gallery/LibrarySidePanel.qml"
  RAVO_STUDIO_LIBRARY_FILTER_BAR_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/gallery/LibraryFilterBar.qml"
  RAVO_STUDIO_EXPORT_OPTIONS_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/ExportOptionsDialog.qml"
  RAVO_STUDIO_BACKUP_SCHEDULE_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/BackupScheduleDialog.qml"
  RAVO_STUDIO_ACTIONS_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/StudioActions.qml"
  RAVO_STUDIO_CONTEXT_MENU_ITEM_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/StudioContextMenuItem.qml"
  RAVO_STUDIO_PRESET_RENAME_DIALOG_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/PresetRenameDialog.qml"
  RAVO_STUDIO_PRESET_SAVE_DIALOG_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/PresetSaveDialog.qml"
  RAVO_STUDIO_PHOTO_CONTEXT_MENU_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/PhotoContextMenu.qml"
  RAVO_STUDIO_SETTINGS_PAGE_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/SettingsPage.qml"
  RAVO_STUDIO_IMPORT_PAGE_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/ImportPage.qml"
  RAVO_STUDIO_ASSISTANT_PANEL_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/chrome/AssistantPanel.qml"
  RAVO_STUDIO_MAIN_QML="${CMAKE_SOURCE_DIR}/Ravo/desktop/qml/Main.qml")
add_dependencies(ravo_desktop_command_tests ravo ravo_studio_translations)
ravo_configure_target(ravo_desktop_command_tests)
ravo_copy_windows_runtime_dlls(ravo_desktop_command_tests)
gtest_discover_tests(ravo_desktop_command_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS "ravo-desktop-smoke")

ravo_add_repository_check(ravo_fixture_manifest freeze_legacy_manifest.py --check)
ravo_add_repository_check(ravo_dependency_boundary check_ravo_dependency_boundary.py)
ravo_add_repository_check(ravo_vertical_slice_plan check_vertical_slice_plan.py)
ravo_add_repository_check(ravo_fixture_classification_ledger check_fixture_classification_ledger.py)

add_test(
  NAME ravo_repo_check_utils_tests
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/test_repo_check_utils.py" -q)
add_test(
  NAME ravo_translation_unit_size_checker_tests
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/test_check_translation_unit_size.py" -q)
add_test(
  NAME ravo_qml_file_size_checker_tests
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/test_check_qml_file_size.py" -q)
add_test(
  NAME ravo_test_split_inventory_checker_tests
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/test_check_test_split_inventory.py" -q)
set_tests_properties(
  ravo_repo_check_utils_tests
  ravo_translation_unit_size_checker_tests
  ravo_qml_file_size_checker_tests
  ravo_test_split_inventory_checker_tests
  PROPERTIES LABELS "ravo-contract")

ravo_add_root_repository_check(ravo_translation_unit_size check_translation_unit_size.py)
ravo_add_root_repository_check(ravo_qml_file_size check_qml_file_size.py)
ravo_add_root_repository_check(ravo_test_split_inventory check_test_split_inventory.py)

add_custom_target(RavoCodeQuality
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/test_repo_check_utils.py" -q
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/test_check_translation_unit_size.py" -q
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/check_translation_unit_size.py"
    --repository-root "${CMAKE_SOURCE_DIR}"
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/test_check_qml_file_size.py" -q
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/check_qml_file_size.py"
    --repository-root "${CMAKE_SOURCE_DIR}"
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/test_check_test_split_inventory.py" -q
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/configs/check_test_split_inventory.py"
    --repository-root "${CMAKE_SOURCE_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  USES_TERMINAL)
