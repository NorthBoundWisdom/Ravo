add_library(ravo_engine STATIC)

target_sources(ravo_engine
  PRIVATE
    src/capability_ops.cpp
    src/color_balance_rgb.cpp
    src/color_balance_rgb.h
    src/engine.cpp
    src/image_ops.cpp
    src/input_color.cpp
    src/input_color.h
    src/output_color.h
    src/profile_gamma.cpp
    src/profile_gamma.h
    src/primaries.cpp
    src/primaries.h
    src/raw_ca.cpp
    src/raw_ca.h
    src/raw_pipeline.cpp
    src/raw_temperature.cpp
    src/raw_temperature.h
    src/scopes.cpp)

target_include_directories(ravo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_engine
  PUBLIC ravo_recipe
  PRIVATE Qt6::Core PNG::PNG ZLIB::ZLIB lcms2 ravo_libraw)

ravo_configure_target(ravo_engine)
