add_library(ravo_control STATIC)

target_sources(ravo_control
  PRIVATE
    src/live_control.cpp
    include/ravo/control/live_control.h)

target_include_directories(ravo_control
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_control
  PUBLIC ravo_foundation
  PRIVATE Qt6::Core Qt6::Network)

ravo_configure_target(ravo_control)
