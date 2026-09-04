add_library(ravo_adapters STATIC)

target_sources(ravo_adapters
  PRIVATE
    src/camera_noise_profile.cpp
    src/export_metadata_encoder.cpp
    src/filesystem_recovery_store.cpp
    src/filesystem_preview_cache.cpp
    src/jpeg_encoder.cpp
    src/crs_xmp.cpp
    src/xmp_adjacent_metadata.cpp
    src/legacy_xmp.cpp
    src/legacy_xmp_balance.cpp
    src/legacy_xmp_color.cpp
    src/legacy_xmp_effects.cpp
    src/legacy_xmp_geometry.cpp
    src/png_encoder.cpp
    src/qt_raster_decoder.cpp
    src/qt_raster_common.cpp
    src/qt_raster_encoder.cpp
    src/qt_raster_jpeg.cpp
    src/qt_raster_png.cpp
    src/qt_raster_tiff.cpp
    src/qt_raster_tiff_contract.cpp
    src/radiance_rgbe_decoder.cpp
    src/sqlite_catalog.cpp
    src/sqlite_catalog_assets.cpp
    src/sqlite_catalog_internal.cpp
    src/sqlite_catalog_library.cpp
    src/sqlite_catalog_keywords.cpp
    src/sqlite_catalog_recipe.cpp
    src/sqlite_catalog_recovery.cpp
    src/tiff_encoder.cpp
    src/text_file.cpp)

# Host prefixes inject -isystem jpeglib.h (JPEG 80) ahead of any target
# include we add. Name the pinned 6b headers in the encoder TU instead.
target_compile_definitions(ravo_adapters PRIVATE
  "RAVO_PINNED_JPEGLIB_H=\"${RAVO_LIBJPEG_TURBO_INCLUDE_DIR}/jpeglib.h\""
  "RAVO_PINNED_JERROR_H=\"${RAVO_LIBJPEG_TURBO_INCLUDE_DIR}/jerror.h\"")
target_include_directories(ravo_adapters PRIVATE
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
