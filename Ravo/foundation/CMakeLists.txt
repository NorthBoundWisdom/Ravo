add_library(ravo_foundation STATIC)

target_sources(ravo_foundation
  PRIVATE
    src/cancellation.cpp
    src/error.cpp
    src/executor.cpp
    src/json.cpp
    src/log.cpp)

target_include_directories(ravo_foundation
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_foundation PUBLIC rflog::rflog)

ravo_configure_target(ravo_foundation)
