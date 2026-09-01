#include "catalog_test_support.h"

#include <system_error>
#include <utility>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/domain/types.h"

namespace ravo
{

std::filesystem::path make_catalog_test_temp_root()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    return root;
}

CatalogServiceTest::CatalogServiceTest()
    : engine([]
             {
                 auto created = EngineFacade::create_phase1();
                 return std::move(created).value();
             }())
{
}

CatalogServiceTest::~CatalogServiceTest() = default;

void CatalogServiceTest::SetUp()
{
    auto created = EngineFacade::create_phase1();
    ASSERT_TRUE(created) << created.error().message;
    engine = std::move(created).value();
    root = make_catalog_test_temp_root();
    database_path = (root / "library.sqlite").string();
}

void CatalogServiceTest::TearDown()
{
    service.reset();
    sqlite_repository = nullptr;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

Result<void> CatalogServiceTest::open_service(const bool create, const bool resume_recovery)
{
    auto repository = create ? SqliteCatalogRepository::create(database_path) :
                               SqliteCatalogRepository::open(database_path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(database_path + ".preview");
    if (!cache)
    {
        return cache.error();
    }
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    if (!recovery)
    {
        return recovery.error();
    }
    auto owned_repository = std::move(repository).value();
    sqlite_repository = owned_repository.get();
    service = std::make_unique<CatalogService>(
        engine, std::move(owned_repository), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(), std::move(recovery).value());
    if (resume_recovery)
    {
        auto resumed = service->sync_recovery(std::nullopt);
        if (!resumed)
        {
            return resumed.error();
        }
    }
    return {};
}

} // namespace ravo
