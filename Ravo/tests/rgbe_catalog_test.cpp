#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

class RgbeCatalogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto created_engine = EngineFacade::create_phase1();
        ASSERT_TRUE(created_engine) << created_engine.error().message;
        engine_ = std::move(created_engine).value();
        root_ =
            std::filesystem::temp_directory_path() / ("ravo-rgbe-catalog-" + generate_catalog_id());
        std::filesystem::create_directories(root_);

        const std::string database_path = (root_ / "library.sqlite").string();
        auto repository = SqliteCatalogRepository::create(database_path);
        ASSERT_TRUE(repository) << repository.error().message;
        auto cache = FilesystemPreviewCache::create(database_path + ".preview");
        ASSERT_TRUE(cache) << cache.error().message;
        auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
        ASSERT_TRUE(recovery) << recovery.error().message;
        service_ = std::make_unique<CatalogService>(
            engine_, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
            std::move(cache).value(), std::move(recovery).value());
    }

    void TearDown() override
    {
        service_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    EngineFacade engine_ = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
    std::filesystem::path root_;
    std::unique_ptr<CatalogService> service_;
};

TEST_F(RgbeCatalogTest, PreservesStructuredUnsupportedAndPublishesNoAsset)
{
    int index = 0;
    for (const QByteArray &magic : {QByteArray("#?RADIANCE\n", 11), QByteArray("#?RGBE\n", 7)})
    {
        const auto path = root_ / ("unsupported-" + std::to_string(index++) + ".bin");
        QFile file(QString::fromStdString(path.string()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray encoded = magic;
        encoded.append("FORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
        encoded.append(QByteArray(4, '\0'));
        ASSERT_EQ(file.write(encoded), encoded.size());
        file.close();

        const auto imported = service_->import_one(path.string(), CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        EXPECT_EQ(imported.value().status, ImportItemStatus::kUnsupported);
        EXPECT_FALSE(imported.value().asset);
        ASSERT_TRUE(imported.value().error);
        EXPECT_EQ(imported.value().error->code, ErrorCode::kUnsupported);
        const auto format = imported.value().error->context.find("format");
        EXPECT_NE(format, imported.value().error->context.end());
        if (format != imported.value().error->context.end())
        {
            EXPECT_EQ(format->second, "rgbe");
        }
        const auto reason = imported.value().error->context.find("reason");
        EXPECT_NE(reason, imported.value().error->context.end());
        if (reason != imported.value().error->context.end())
        {
            EXPECT_EQ(reason->second, "unsupported_rgbe_input");
        }

        const auto assets = service_->list_assets();
        ASSERT_TRUE(assets) << assets.error().message;
        EXPECT_TRUE(assets.value().empty());
        const auto previews = service_->list_previews();
        ASSERT_TRUE(previews) << previews.error().message;
        EXPECT_TRUE(previews.value().empty());
    }
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-rgbe-catalog-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
