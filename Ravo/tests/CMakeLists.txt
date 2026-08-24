find_package(GTest 1.17 CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(GoogleTest)

function(ravo_add_phase0_check name script)
  add_test(
    NAME ${name}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Ravo/tools/${script}"
      --repository-root "${CMAKE_SOURCE_DIR}" ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS "ravo-contract")
endfunction()

add_executable(ravo_unit_tests
  domain_test.cpp
  foundation_test.cpp
  recipe_test.cpp)
target_link_libraries(ravo_unit_tests PRIVATE GTest::gtest_main ravo_domain ravo_recipe)
target_include_directories(ravo_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ravo_configure_target(ravo_unit_tests)
gtest_discover_tests(ravo_unit_tests PROPERTIES LABELS "ravo-unit")

add_executable(ravo_contract_tests
  cli_test.cpp
  engine_test.cpp)
target_link_libraries(ravo_contract_tests PRIVATE GTest::gtest_main ravo_cli)
target_include_directories(ravo_contract_tests PRIVATE
  $<TARGET_PROPERTY:PNG::PNG,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(ravo_contract_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_contract_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_contract_tests)
ravo_deploy_qt_core(ravo_contract_tests)
gtest_discover_tests(ravo_contract_tests
  PROPERTIES
    LABELS "ravo-contract"
    ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;$<ENV:PATH>")

add_executable(ravo_catalog_tests catalog_test.cpp)
target_link_libraries(ravo_catalog_tests
  PRIVATE
    GTest::gtest
    ravo_services
    ravo_adapters
    Qt6::Core
    Qt6::Gui
    Qt6::Sql)
target_include_directories(ravo_catalog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ravo_catalog_tests PRIVATE
  RAVO_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
ravo_configure_target(ravo_catalog_tests)
ravo_deploy_qt_core(ravo_catalog_tests)
gtest_discover_tests(ravo_catalog_tests
  PROPERTIES
    LABELS "ravo-catalog"
    ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;$<ENV:PATH>")

ravo_add_phase0_check(ravo_fixture_manifest freeze_legacy_manifest.py --check)
ravo_add_phase0_check(ravo_capability_inventory check_capability_inventory.py)
ravo_add_phase0_check(ravo_dependency_boundary check_ravo_dependency_boundary.py)
ravo_add_phase0_check(ravo_vertical_slice_plan check_vertical_slice_plan.py)
ravo_add_phase0_check(ravo_fixture_classification_ledger check_fixture_classification_ledger.py)
ravo_add_phase0_check(ravo_freeze_reference check_freeze_reference.py)
