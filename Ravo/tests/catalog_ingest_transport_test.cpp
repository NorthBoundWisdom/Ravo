#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/ingest_transport.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "JPEG", 92);
}

[[nodiscard]] std::string reason_of(const TaskError &error)
{
    const auto it = error.context.find("reason");
    return it == error.context.end() ? std::string{} : it->second;
}

} // namespace

TEST(IngestTransportUri, FormatsAndParsesFilesystemCard)
{
    const auto uri = format_filesystem_card_ingest_uri("/Volumes/CARD");
    EXPECT_EQ(uri, "ravo-ingest:filesystem-card:/Volumes/CARD");
    auto parsed = parse_ingest_source_uri(uri);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().transport, IngestTransportKind::kFilesystemCard);
    EXPECT_EQ(parsed.value().root, "/Volumes/CARD");
    EXPECT_EQ(parsed.value().uri, uri);
}

TEST(IngestTransportUri, RejectsUnimplementedPtpUsb)
{
    auto parsed = parse_ingest_source_uri("ravo-ingest:ptp-usb:device/abc");
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(reason_of(parsed.error()), "ingest_transport_ptp_usb_unimplemented");
}

TEST_F(CatalogServiceTest, FilesystemCardIngestPrefersDcimAndPreservesSource)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "CARD";
    const auto dcim = card / "DCIM" / "100RAVO";
    const auto destination = root / "primary";
    const auto second = root / "second";
    std::filesystem::create_directories(dcim);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second);
    const auto photo_a = dcim / "IMG_0001.jpg";
    const auto photo_b = dcim / "IMG_0002.jpg";
    ASSERT_TRUE(write_jpeg(photo_a, QColor(10, 20, 30)));
    ASSERT_TRUE(write_jpeg(photo_b, QColor(200, 100, 50)));
    const auto hash_a = sha256_file_hex(photo_a.string());
    const auto hash_b = sha256_file_hex(photo_b.string());
    ASSERT_TRUE(hash_a) << hash_a.error().message;
    ASSERT_TRUE(hash_b) << hash_b.error().message;

    auto snapshot = open_filesystem_card_ingest(card.string(), CancellationToken{});
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_TRUE(snapshot.value().dcim_discovered);
    EXPECT_EQ(snapshot.value().media_paths.size(), 2U);
    EXPECT_EQ(snapshot.value().source.uri,
              format_filesystem_card_ingest_uri(normalize_local_input(card.string()).value().path));

    IngestRequest request;
    request.source_root = card.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kSingleFolder;
    request.destination_directory = destination.string();
    request.second_copy_directory = second.string();
    request.filename_template = "job-{sequence}-{stem}{ext}";
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    auto batch = service->execute_ingest(request);
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_EQ(batch.value().imported, 2U);
    EXPECT_EQ(batch.value().verified_second_copies, 2U);
    EXPECT_EQ(batch.value().failed, 0U);

    EXPECT_TRUE(std::filesystem::exists(destination / "job-0001-IMG_0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(destination / "job-0002-IMG_0002.jpg"));
    EXPECT_TRUE(std::filesystem::exists(second / "job-0001-IMG_0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(second / "job-0002-IMG_0002.jpg"));
    ASSERT_EQ(sha256_file_hex(photo_a.string()).value(), hash_a.value());
    ASSERT_EQ(sha256_file_hex(photo_b.string()).value(), hash_b.value());
    EXPECT_TRUE(std::filesystem::exists(photo_a));
    EXPECT_TRUE(std::filesystem::exists(photo_b));

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_EQ(assets.value().size(), 2U);
}

TEST_F(CatalogServiceTest, FilesystemCardIngestRejectsMove)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "move-card" / "DCIM";
    std::filesystem::create_directories(card);
    ASSERT_TRUE(write_jpeg(card / "a.jpg", QColor(1, 2, 3)));
    IngestRequest request;
    request.source_root = (root / "move-card").string();
    request.mode = ImportTransferMode::kMove;
    request.destination_directory = (root / "dest").string();
    std::filesystem::create_directories(root / "dest");
    auto batch = service->execute_ingest(request);
    ASSERT_FALSE(batch);
    EXPECT_EQ(reason_of(batch.error()), "ingest_move_unsupported");
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, FilesystemCardIngestReportsDisconnectPartialAndPreservesCatalog)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "live-card";
    const auto dcim = card / "DCIM" / "100RAVO";
    const auto destination = root / "dest-disconnect";
    std::filesystem::create_directories(dcim);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(dcim / "one.jpg", QColor(11, 22, 33)));
    ASSERT_TRUE(write_jpeg(dcim / "two.jpg", QColor(44, 55, 66)));
    ASSERT_TRUE(write_jpeg(dcim / "three.jpg", QColor(77, 88, 99)));
    const auto source_one = dcim / "one.jpg";
    const auto source_hash = sha256_file_hex(source_one.string());
    ASSERT_TRUE(source_hash) << source_hash.error().message;

    IngestRequest request;
    request.source_root = card.string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    std::size_t imported_progress = 0;
    auto batch =
        service->execute_ingest(request,
                                [&](std::size_t, std::size_t, const ImportItemResult *item)
                                {
                                    if (item && item->status == ImportItemStatus::kImported)
                                    {
                                        ++imported_progress;
                                        if (imported_progress == 1U)
                                        {
                                            std::error_code error;
                                            std::filesystem::remove_all(card, error);
                                            EXPECT_FALSE(error) << error.message();
                                        }
                                    }
                                });
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_EQ(batch.value().items.size(), 3U);
    EXPECT_EQ(batch.value().imported, 1U);
    EXPECT_EQ(batch.value().failed, 2U);
    ASSERT_EQ(sha256_file_hex((destination / "one.jpg").string()).value(), source_hash.value());

    std::size_t disconnected = 0;
    for (const auto &item : batch.value().items)
    {
        if (item.status == ImportItemStatus::kFailed && item.error &&
            reason_of(*item.error) == "ingest_source_disconnected")
            ++disconnected;
    }
    EXPECT_EQ(disconnected, 2U);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_EQ(assets.value().size(), 1U);
}

TEST_F(CatalogServiceTest, FilesystemCardIngestCancelReportsRemainingItems)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "cancel-card" / "DCIM";
    const auto destination = root / "cancel-dest";
    std::filesystem::create_directories(card);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(card / "a.jpg", QColor(9, 9, 9)));
    ASSERT_TRUE(write_jpeg(card / "b.jpg", QColor(8, 8, 8)));
    ASSERT_TRUE(write_jpeg(card / "c.jpg", QColor(7, 7, 7)));

    CancellationSource cancellation;
    std::size_t progress_count = 0;
    IngestRequest request;
    request.source_root = (root / "cancel-card").string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    request.cancellation = cancellation.token();
    auto batch = service->execute_ingest(
        request,
        [&](std::size_t, std::size_t, const ImportItemResult *item)
        {
            ++progress_count;
            if (progress_count == 1U && item && item->status == ImportItemStatus::kImported)
                EXPECT_TRUE(cancellation.cancel("ingest-test-cancel"));
        });
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_EQ(batch.value().items.size(), 3U);
    EXPECT_GE(batch.value().imported, 1U);
    EXPECT_GE(batch.value().failed, 1U);
    bool saw_cancelled = false;
    for (const auto &item : batch.value().items)
    {
        if (item.status == ImportItemStatus::kFailed && item.error &&
            item.error->code == ErrorCode::kCancelled)
            saw_cancelled = true;
    }
    EXPECT_TRUE(saw_cancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), batch.value().imported);
}

TEST_F(CatalogServiceTest, FilesystemCardIngestUnresolvedConflictPublishesNoCatalogRow)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "conflict-card" / "DCIM";
    const auto destination = root / "conflict-dest";
    std::filesystem::create_directories(card);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(card / "same.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(destination / "same.jpg", QColor(4, 5, 6)));

    IngestRequest request;
    request.source_root = (root / "conflict-card").string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    auto batch = service->execute_ingest(request);
    ASSERT_FALSE(batch);
    EXPECT_EQ(reason_of(batch.error()), "import_destination_conflict");
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
    EXPECT_TRUE(std::filesystem::exists(card / "same.jpg"));
}

} // namespace ravo
