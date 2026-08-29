add_library(ravo_engine STATIC)

target_sources(ravo_engine
  PRIVATE
    src/capability_ops.cpp
    src/canvas_frame.cpp
    src/canvas_frame.h
    src/color_balance_rgb.cpp
    src/color_balance_rgb.h
    src/color_checker.cpp
    src/color_checker.h
    src/color_harmonizer.cpp
    src/color_harmonizer.h
    src/color_reconstruction.cpp
    src/color_reconstruction.h
    src/color_zones.cpp
    src/color_zones.h
    src/color_contrast.cpp
    src/color_contrast.h
    src/color_correction.cpp
    src/color_correction.h
    src/d50_lab.h
    src/dehaze.cpp
    src/dehaze.h
    src/dt_ucs.cpp
    src/dt_ucs.h
    src/harmony_geometry.cpp
    src/harmony_geometry.h
    src/hsl.h
    src/engine.cpp
    src/image_ops.cpp
    src/input_color.cpp
    src/input_color.h
    src/mask_evaluator.cpp
    src/mask_evaluator.h
    src/mask_path.cpp
    src/mask_path.h
    src/monochrome.cpp
    src/monochrome.h
    src/output_color.h
    src/output_dither.cpp
    src/output_dither.h
    src/parallel_rows.h
    src/profile_gamma.cpp
    src/profile_gamma.h
    src/primaries.cpp
    src/primaries.h
    src/raw_ca.cpp
    src/raw_ca.h
    src/raw_denoise.cpp
    src/raw_denoise.h
    src/raw_pipeline.cpp
    src/recursive_gaussian.cpp
    src/recursive_gaussian.h
    src/retouch.cpp
    src/retouch.h
    src/raw_temperature.cpp
    src/raw_temperature.h
    src/scopes.cpp
    src/sharpen.cpp
    src/sharpen.h
    src/split_toning.cpp
    src/split_toning.h
    src/watermark.cpp
    src/watermark.h
    src/velvia.cpp
    src/velvia.h)

target_include_directories(ravo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_engine
  PUBLIC ravo_recipe
  PRIVATE Exiv2::exiv2lib Qt6::Core PNG::PNG ZLIB::ZLIB lcms2 ravo_libraw)

ravo_configure_target(ravo_engine)
