add_library(ravo_cli STATIC)

target_sources(ravo_cli
  PRIVATE
    src/application.cpp)

target_include_directories(ravo_cli
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_definitions(ravo_cli PRIVATE RAVO_VERSION="${PROJECT_VERSION}")
target_link_libraries(ravo_cli PUBLIC ravo_adapters ravo_engine ravo_services Qt6::Core)

ravo_configure_target(ravo_cli)

add_executable(ravo src/main.cpp)
target_link_libraries(ravo PRIVATE ravo_cli)
ravo_configure_target(ravo)
ravo_deploy_qt_core(ravo)
