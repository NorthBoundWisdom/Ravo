#include <cstdint>
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

void append_u32_be(QByteArray &bytes, const std::uint32_t value)
{
    bytes.append(static_cast<char>((value >> 24U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 16U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>(value & 0xFFU));
}

[[nodiscard]] QByteArray minimal_qoi()
{
    QByteArray bytes("qoif", 4);
    append_u32_be(bytes, 1U);
    append_u32_be(bytes, 1U);
    bytes.append('\3');
    bytes.append('\0');
    bytes.append(static_cast<char>(0xFEU));
    bytes.append('\x12');
    bytes.append('\x34');
    bytes.append('\x56');
    bytes.append(QByteArray(7, '\0'));
    bytes.append('\1');
    return bytes;
}

class QoiCatalogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto created_engine = EngineFacade::create_phase1();
        ASSERT_TRUE(created_engine) << created_engine.error().message;
        engine_ = std::move(created_engine).value();
        root_ =
            std::filesystem::temp_directory_path() / ("ravo-qoi-catalog-" + generate_catalog_id());
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

TEST_F(QoiCatalogTest, PreservesStructuredUnsupportedAndPublishesNoAsset)
{
    const auto path = root_ / "unsupported.qoi";
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray encoded = minimal_qoi();
    ASSERT_EQ(file.write(encoded), encoded.size());
    file.close();

    const auto imported = service_->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().status, ImportItemStatus::kUnsupported);
    EXPECT_FALSE(imported.value().asset);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kUnsupported);
    ASSERT_TRUE(imported.value().error->context.contains("format"));
    EXPECT_EQ(imported.value().error->context.at("format"), "qoi");
    ASSERT_TRUE(imported.value().error->context.contains("reason"));
    EXPECT_EQ(imported.value().error->context.at("reason"), "unsupported_qoi_input");

    const auto assets = service_->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
    const auto previews = service_->list_previews();
    ASSERT_TRUE(previews) << previews.error().message;
    EXPECT_TRUE(previews.value().empty());
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-qoi-catalog-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
