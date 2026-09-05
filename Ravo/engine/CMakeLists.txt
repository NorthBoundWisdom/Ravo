add_library(ravo_engine STATIC)

target_sources(ravo_engine
  PRIVATE
    src/bayer_demosaic.cpp
    src/bayer_demosaic.h
    src/capability_ops.cpp
    src/capability_raw.cpp
    src/capability_lens_color.cpp
    src/capability_color_equalizer.cpp
    src/capability_tone_equalizer.cpp
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
    src/dng_opcodes.cpp
    src/dng_opcodes.h
    src/harmony_geometry.cpp
    src/harmony_geometry.h
    src/hsl.h
    src/engine.cpp
    src/gpu_adapter.h
    src/gpu_adapter_rhi_types.h
    src/guided_filter.cpp
    src/guided_filter.h
    src/image_ops.cpp
    src/image_ops_effects.cpp
    src/image_ops_tone.cpp
    src/image_recipe_ops.cpp
    src/image_geometry_ops.cpp
    src/input_color.cpp
    src/input_color.h
    src/lut3d.cpp
    src/lut3d.h
    src/mask_evaluator.cpp
    src/mask_evaluator.h
    src/mask_path.cpp
    src/mask_path.h
    src/monochrome.cpp
    src/monochrome.h
    src/noise_calibration.cpp
    src/iq_quality_evaluation.cpp
    src/output_color.h
    src/output_dither.cpp
    src/output_dither.h
    src/parallel_rows.h
    src/perspective_transform.cpp
    src/perspective_transform.h
    src/preview_roi.cpp
    src/preview_roi.h
    src/profile_gamma.cpp
    src/profile_gamma.h
    src/primaries.cpp
    src/primaries.h
    src/raw_ca.cpp
    src/raw_ca.h
    src/raw_denoise.cpp
    src/raw_denoise.h
    src/raw_pipeline.cpp
    src/rapidraw_tone_controls.cpp
    src/rapidraw_tone_controls.h
    src/raw_metadata.cpp
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
    src/texture.cpp
    src/texture.h
    src/watermark.cpp
    src/watermark.h
    src/velvia.cpp
    src/velvia.h
    src/xtrans_demosaic.cpp
    src/xtrans_demosaic.h)

target_include_directories(ravo_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    BEFORE
    "${RAVO_EXIV2_SOURCE_ROOT}/include"
    "${CMAKE_BINARY_DIR}")

set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.11 REQUIRED COMPONENTS GuiPrivate ShaderTools)
target_link_libraries(ravo_engine
  PUBLIC ravo_recipe Qt6::Gui
  PRIVATE Exiv2::exiv2lib Qt6::Core Qt6::GuiPrivate PNG::PNG ZLIB::ZLIB lcms2 ravo_libraw)

if(NOT Python3_Interpreter_FOUND)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
get_filename_component(_ravo_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
set(_ravo_qsb_hints "${_ravo_qt_prefix}/bin" "${_ravo_qt_prefix}/libexec")
if(DEFINED ENV{QT_ROOT_DIR} AND NOT "$ENV{QT_ROOT_DIR}" STREQUAL "")
  list(APPEND _ravo_qsb_hints "$ENV{QT_ROOT_DIR}/bin" "$ENV{QT_ROOT_DIR}/libexec")
endif()
find_program(RAVO_QSB_EXECUTABLE NAMES qsb qsb.exe HINTS ${_ravo_qsb_hints} REQUIRED)
set(_ravo_gpu_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
function(ravo_embed_gpu_shader shader_name)
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader_name}.comp")
  set(_qsb "${_ravo_gpu_shader_dir}/${shader_name}.comp.qsb")
  set(_cpp "${_ravo_gpu_shader_dir}/${shader_name}_qsb.cpp")
  add_custom_command(
    OUTPUT "${_qsb}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ravo_gpu_shader_dir}"
    COMMAND "${RAVO_QSB_EXECUTABLE}"
      --glsl "310es,430"
      --hlsl 50
      --msl 12
      -o "${_qsb}"
      "${_src}"
    DEPENDS "${_src}"
    VERBATIM)
  add_custom_command(
    OUTPUT "${_cpp}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configs/embed_binary.py"
      --input "${_qsb}"
      --output "${_cpp}"
      --symbol ravo_gpu_${shader_name}_qsb
    DEPENDS "${_qsb}" "${CMAKE_SOURCE_DIR}/configs/embed_binary.py"
    VERBATIM)
  target_sources(ravo_engine PRIVATE "${_cpp}")
endfunction()
ravo_embed_gpu_shader(affine_rgb)
ravo_embed_gpu_shader(sigmoid_rgb)
ravo_embed_gpu_shader(rapidraw_basic_tone)
ravo_embed_gpu_shader(rapidraw_tone_blur)
ravo_embed_gpu_shader(rapidraw_tone_controls)
ravo_embed_gpu_shader(light_controls)
ravo_embed_gpu_shader(contrast_rgb)
ravo_embed_gpu_shader(gamma_rgb)
ravo_embed_gpu_shader(vibrance_saturation_rgb)
ravo_embed_gpu_shader(velvia_rgb)
ravo_embed_gpu_shader(split_toning_rgb)
ravo_embed_gpu_shader(color_contrast_rgb)
ravo_embed_gpu_shader(sharpen_lab)
ravo_embed_gpu_shader(rcd_demosaic)
ravo_embed_gpu_shader(copy_rgb)
ravo_embed_gpu_shader(pack_rgba8)
target_sources(ravo_engine PRIVATE
  src/gpu_adapter_rhi.cpp
  src/gpu_preview.cpp)
if(APPLE)
  target_sources(ravo_engine PRIVATE src/gpu_display_metal.mm src/gpu_display_metal.h)
  target_link_libraries(ravo_engine PRIVATE "-framework Metal" "-framework IOSurface"
                                            "-framework CoreFoundation")
endif()

ravo_configure_target(ravo_engine)
