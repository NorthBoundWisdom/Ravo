add_library(ravo_cli STATIC)

target_sources(ravo_cli
  PRIVATE
    src/application.cpp
    src/application_catalog.cpp
    src/application_catalog_args.cpp
    src/application_catalog_support.cpp
    src/application_catalog_xmp.cpp
    src/application_catalog_editor.cpp
    src/application_catalog_ai_suggestion.cpp
    src/application_catalog_convert.cpp
    src/application_catalog_cull.cpp
    src/application_catalog_facets.cpp
    src/application_studio.cpp)

target_include_directories(ravo_cli
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_definitions(ravo_cli PRIVATE RAVO_VERSION="${PROJECT_VERSION}")
target_link_libraries(ravo_cli
  PUBLIC ravo_adapters ravo_engine ravo_services ravo_control Qt6::Core
  PRIVATE Qt6::Network)

ravo_configure_target(ravo_cli)

add_executable(ravo src/main.cpp)
target_link_libraries(ravo PRIVATE ravo_cli)
ravo_configure_target(ravo)
ravo_copy_windows_runtime_dlls(ravo)
