add_library(ravo_engine STATIC)

target_sources(ravo_engine
  PRIVATE
    src/capability_ops.cpp
    src/color_balance_rgb.cpp
    src/color_balance_rgb.h
    src/color_checker.cpp
    src/color_checker.h
    src/color_contrast.cpp
    src/color_contrast.h
    src/color_correction.cpp
    src/color_correction.h
    src/d50_lab.h
    src/dt_ucs.cpp
    src/dt_ucs.h
    src/harmony_geometry.cpp
    src/harmony_geometry.h
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

set_property(SOURCE src/harmony_geometry.cpp APPEND PROPERTY COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-ffp-contract=off>"
  "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>")

target_include_directories(ravo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_engine
  PUBLIC ravo_recipe
  PRIVATE Exiv2::exiv2lib Qt6::Core PNG::PNG ZLIB::ZLIB lcms2 ravo_libraw)

ravo_configure_target(ravo_engine)
