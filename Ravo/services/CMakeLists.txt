add_library(ravo_services STATIC)

target_sources(ravo_services
  PRIVATE
    src/atomic_publication_internal.cpp
    src/artifact_publication.cpp
    src/catalog_backup.cpp
    src/catalog_backup_schedule.cpp
    src/catalog_folder.cpp
    src/catalog_keywords.cpp
    src/catalog_ai_proposal.cpp
    src/catalog_xmp_interchange.cpp
    src/catalog_external_editor.cpp
    src/catalog_foreign_catalog.cpp
    src/catalog_internal.cpp
    src/catalog_import.cpp
    src/catalog_ingest_transport.cpp
    src/encoded_publication.cpp
    src/export_jobs.cpp
    src/export_output_sharpen.cpp
    src/export_delivery_watermark.cpp
    src/export_delivery_frame.cpp
    src/export_delivery_color.cpp
    src/original_copy.cpp
    src/catalog_service.cpp
    src/catalog_develop_batch.cpp
    src/catalog_service_internal.cpp
    src/catalog_transfer.cpp
    src/catalog_preview.cpp
    src/catalog_preview_roi.cpp
    src/catalog_restore.cpp)

target_include_directories(ravo_services
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_services PUBLIC ravo_domain ravo_engine ravo_adapters)

ravo_configure_target(ravo_services)
