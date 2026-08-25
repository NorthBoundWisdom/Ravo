add_library(ravo_services STATIC)

target_sources(ravo_services
  PRIVATE
    src/catalog_internal.cpp
    src/catalog_service.cpp
    src/catalog_preview.cpp)

target_include_directories(ravo_services
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_services PUBLIC ravo_domain ravo_engine)

ravo_configure_target(ravo_services)
