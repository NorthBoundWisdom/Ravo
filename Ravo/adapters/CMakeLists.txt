add_library(ravo_adapters STATIC)

target_sources(ravo_adapters
  PRIVATE
    src/filesystem_preview_cache.cpp
    src/legacy_xmp.cpp
    src/qt_raster_decoder.cpp
    src/radiance_rgbe_decoder.cpp
    src/sqlite_catalog.cpp
    src/text_file.cpp)

target_include_directories(ravo_adapters
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_adapters
  PUBLIC ravo_foundation ravo_recipe ravo_domain
  PRIVATE Qt6::Core Qt6::Gui Qt6::Sql ZLIB::ZLIB)

ravo_configure_target(ravo_adapters)
