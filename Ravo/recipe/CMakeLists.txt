add_library(ravo_recipe STATIC)

target_sources(ravo_recipe
  PRIVATE
    src/color_checker.cpp
    src/canvas_frame.cpp
    src/color_harmonizer.cpp
    src/color_reconstruction.cpp
    src/color_zones.cpp
    src/color_contrast.cpp
    src/color_correction.cpp
    src/color_input.cpp
    src/color_output.cpp
    src/mask.cpp
    src/monochrome.cpp
    src/develop_mask.cpp
    src/profile_gamma.cpp
    src/primaries.cpp
    src/operation.cpp
    src/output_dither.cpp
    src/recipe.cpp
    src/retouch.cpp
    src/develop.cpp
    src/dehaze.cpp
    src/sharpen.cpp
    src/split_toning.cpp
    src/style.cpp
    src/watermark.cpp
    src/velvia.cpp)

target_include_directories(ravo_recipe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_recipe PUBLIC ravo_foundation)

ravo_configure_target(ravo_recipe)
