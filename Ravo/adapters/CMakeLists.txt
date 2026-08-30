add_library(ravo_adapters STATIC)

target_sources(ravo_adapters
  PRIVATE
    src/camera_noise_profile.cpp
    src/export_metadata_encoder.cpp
    src/filesystem_preview_cache.cpp
    src/jpeg_encoder.cpp
    src/crs_xmp.cpp
    src/legacy_xmp.cpp
    src/png_encoder.cpp
    src/qt_raster_decoder.cpp
    src/radiance_rgbe_decoder.cpp
    src/sqlite_catalog.cpp
    src/tiff_encoder.cpp
    src/text_file.cpp)

target_include_directories(ravo_adapters SYSTEM BEFORE PRIVATE
  $<TARGET_PROPERTY:ravo_libjpeg_turbo,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:ravo_libtiff,INTERFACE_INCLUDE_DIRECTORIES>)
target_include_directories(ravo_adapters
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_adapters
  PUBLIC ravo_foundation ravo_recipe ravo_domain
  PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    PNG::PNG
    ZLIB::ZLIB
    ravo_libjpeg_turbo
    ravo_libtiff)

ravo_configure_target(ravo_adapters)
