add_library(ravo_engine STATIC)

target_sources(ravo_engine
  PRIVATE
    src/capability_ops.cpp
    src/engine.cpp
    src/image_ops.cpp
    src/raw_pipeline.cpp
    src/scopes.cpp)

target_include_directories(ravo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_engine
  PUBLIC ravo_recipe
  PRIVATE Qt6::Core PNG::PNG ravo_libraw)

ravo_configure_target(ravo_engine)
