include_directories("${CMAKE_CURRENT_BINARY_DIR}/../")

add_executable(darktable-test-variables variables.c)
target_link_libraries(darktable-test-variables lib_darktable)
if(WIN32)
  add_custom_command(
    TARGET darktable-test-variables
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:darktable-test-variables>
            $<TARGET_FILE_DIR:darktable-test-variables>
    COMMAND_EXPAND_LISTS)
endif()
add_test(NAME darktable-test-variables COMMAND darktable-test-variables)
set_tests_properties(
  darktable-test-variables
  PROPERTIES
    LABELS "unit;core")

add_subdirectory(unittests)
