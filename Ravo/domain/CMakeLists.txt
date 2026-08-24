add_library(ravo_domain STATIC)

target_sources(ravo_domain
  PRIVATE
    src/types.cpp
    src/uri.cpp)

target_include_directories(ravo_domain
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_domain PUBLIC ravo_foundation)

ravo_configure_target(ravo_domain)
