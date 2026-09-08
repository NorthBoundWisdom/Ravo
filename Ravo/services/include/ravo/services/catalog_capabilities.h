#pragma once

// Focused CatalogService capability owners (same ravo_services target).
// CatalogService remains the composition root and compatibility facade.
// Capability objects are narrow faces: call sites should depend on the
// capability they need. Method bodies still live on CatalogService until a
// later extraction; faces forward without growing a second authority.

#include "ravo/services/library_service.h"
#include "ravo/services/develop_service.h"
#include "ravo/services/metadata_service.h"
#include "ravo/services/import_service.h"
#include "ravo/services/ingest_service.h"
#include "ravo/services/recovery_service.h"

namespace ravo
{

class CatalogService;

// Thin aggregate for composition roots that want role accessors together.
struct CatalogServices
{
    CatalogService &catalog;
    LibraryService &library;
    DevelopService &develop;
    MetadataService &metadata;
    ImportService &import;
    IngestService &ingest;
    RecoveryService &recovery;
};

} // namespace ravo
