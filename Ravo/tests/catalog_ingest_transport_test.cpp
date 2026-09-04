#include <filesystem>
#include <fstream>
#include <optional>
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

TEST(IngestTransportUri, RejectsUnpackagedPtpUsbWithPlatformState)
{
    auto parsed = parse_ingest_source_uri("ravo-ingest:ptp-usb:04a9:31f2:serial1");
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(reason_of(parsed.error()), "native_ingest_adapter_not_packaged");
    EXPECT_FALSE(parsed.error().context.at("platform").empty());
    EXPECT_EQ(parsed.error().context.at("adapter_packaged"), "false");
}

TEST(IngestTransportUri, RejectsUnpackagedMtpWithPlatformState)
{
    auto parsed = parse_ingest_source_uri("ravo-ingest:mtp:18d1:4ee1:serial2");
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(reason_of(parsed.error()), "native_ingest_adapter_not_packaged");
}

TEST(NativeIngestProbe, ReportsUnsupportedUntilPackaged)
{
    const auto support = probe_native_ingest_support();
    EXPECT_EQ(support.schema, kNativeIngestContractVersion);
    EXPECT_FALSE(support.adapter_packaged);
    EXPECT_FALSE(native_ingest_adapter_is_packaged());
    EXPECT_EQ(support.ptp_usb, NativeIngestSupportState::kUnsupported);
    EXPECT_EQ(support.mtp, NativeIngestSupportState::kUnsupported);
    EXPECT_EQ(support.reason, "native_ingest_adapter_not_packaged");
    EXPECT_FALSE(support.platform.empty());
    EXPECT_FALSE(support.ptp_planned_stack.empty());
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

TEST_F(CatalogServiceTest, NativePtpUsbIngestFailsClosedWithMachineState)
{
    ASSERT_TRUE(open_service(true));
    IngestRequest request;
    request.transport = "ptp-usb";
    request.session_vendor_id = "04a9";
    request.session_product_id = "31f2";
    request.session_serial = "abc";
    request.source_root = (root / "unused").string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = (root / "dest").string();
    std::filesystem::create_directories(root / "dest");
    auto batch = service->execute_ingest_detailed(request);
    ASSERT_FALSE(batch);
    EXPECT_EQ(reason_of(batch.error()), "native_ingest_adapter_not_packaged");
    EXPECT_EQ(batch.error().context.at("adapter_packaged"), "false");
    EXPECT_FALSE(batch.error().context.at("platform").empty());
}

TEST_F(CatalogServiceTest, PtpStubIngestCopiesThroughPlannerAndRejectsMove)
{
    ASSERT_TRUE(open_service(true));
    const auto fixture = root / "ptp-fixture" / "DCIM" / "100RAVO";
    const auto destination = root / "ptp-dest";
    const auto second = root / "ptp-second";
    std::filesystem::create_directories(fixture);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second);
    ASSERT_TRUE(write_jpeg(fixture / "IMG_0001.jpg", QColor(12, 34, 56)));
    ASSERT_TRUE(write_jpeg(fixture / "IMG_0002.jpg", QColor(65, 43, 21)));

    auto snapshot = open_ptp_stub_ingest((root / "ptp-fixture").string(), CancellationToken{});
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().source.transport, IngestTransportKind::kPtpStub);
    ASSERT_EQ(snapshot.value().objects.size(), 2U);
    EXPECT_EQ(snapshot.value().objects.front().relative_path.rfind("storage/0001/", 0), 0U);
    EXPECT_TRUE(snapshot.value().source.session.has_value());

    IngestRequest move_request;
    move_request.transport = "ptp-stub";
    move_request.source_root = (root / "ptp-fixture").string();
    move_request.mode = ImportTransferMode::kMove;
    move_request.destination_directory = destination.string();
    auto moved = service->execute_ingest(move_request);
    ASSERT_FALSE(moved);
    EXPECT_EQ(reason_of(moved.error()), "ingest_move_unsupported");

    IngestRequest request;
    request.transport = "ptp-stub";
    request.source_root = (root / "ptp-fixture").string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.second_copy_directory = second.string();
    request.filename_template = "cam-{sequence}-{stem}{ext}";
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    auto detailed = service->execute_ingest_detailed(request);
    ASSERT_TRUE(detailed) << detailed.error().message;
    EXPECT_EQ(detailed.value().transport, "ptp-stub");
    EXPECT_EQ(detailed.value().import.imported, 2U);
    EXPECT_EQ(detailed.value().import.verified_second_copies, 2U);
    EXPECT_TRUE(detailed.value().resume_checkpoint_cleared);
    EXPECT_TRUE(std::filesystem::exists(fixture / "IMG_0001.jpg"));
    EXPECT_TRUE(std::filesystem::exists(fixture / "IMG_0002.jpg"));
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 2U);
}

TEST_F(CatalogServiceTest, PtpStubIngestResumesIncompleteBatchAfterReconnect)
{
    ASSERT_TRUE(open_service(true));
    const auto fixture = root / "resume-fixture" / "DCIM";
    const auto destination = root / "resume-dest";
    std::filesystem::create_directories(fixture);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(fixture / "a.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(fixture / "b.jpg", QColor(4, 5, 6)));
    ASSERT_TRUE(write_jpeg(fixture / "c.jpg", QColor(7, 8, 9)));

    IngestRequest request;
    request.transport = "ptp-stub";
    request.source_root = (root / "resume-fixture").string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;

    std::size_t imported_progress = 0;
    std::optional<std::string> batch_id;
    auto first = service->execute_ingest_detailed(
        request,
        [&](std::size_t, std::size_t, const ImportItemResult *item)
        {
            if (item && item->status == ImportItemStatus::kImported)
            {
                ++imported_progress;
                if (imported_progress == 1U)
                {
                    std::error_code error;
                    std::filesystem::remove_all(root / "resume-fixture", error);
                    EXPECT_FALSE(error) << error.message();
                }
            }
        });
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first.value().resume_batch_id);
    batch_id = first.value().resume_batch_id;
    EXPECT_EQ(first.value().import.imported, 1U);
    EXPECT_GE(first.value().import.failed, 1U);
    EXPECT_FALSE(first.value().resume_checkpoint_cleared);

    auto catalog = service->snapshot();
    ASSERT_TRUE(catalog);
    auto checkpoint = load_ingest_resume_checkpoint(catalog.value().database_path, *batch_id);
    ASSERT_TRUE(checkpoint) << checkpoint.error().message;
    EXPECT_EQ(checkpoint.value().completed_relative_paths.size(), 1U);

    // Reconnect: restore fixture bytes for remaining objects.
    std::filesystem::create_directories(fixture);
    ASSERT_TRUE(write_jpeg(fixture / "a.jpg", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(fixture / "b.jpg", QColor(4, 5, 6)));
    ASSERT_TRUE(write_jpeg(fixture / "c.jpg", QColor(7, 8, 9)));

    IngestRequest resume = request;
    resume.resume_batch_id = batch_id;
    auto second = service->execute_ingest_detailed(resume);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second.value().import.skipped, 1U);
    EXPECT_EQ(second.value().import.imported, 2U);
    EXPECT_EQ(second.value().import.failed, 0U);
    EXPECT_TRUE(second.value().resume_checkpoint_cleared);
    for (const auto &item : second.value().import.items)
    {
        if (item.status == ImportItemStatus::kSkipped)
            EXPECT_EQ(reason_of(*item.error), "ingest_resume_already_completed");
    }
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 3U);
    auto cleared = load_ingest_resume_checkpoint(catalog.value().database_path, *batch_id);
    EXPECT_FALSE(cleared);
    EXPECT_EQ(cleared.error().code, ErrorCode::kNotFound);
}

TEST_F(CatalogServiceTest, FilesystemCardIngestIdempotentRepeatedImportReportsDuplicates)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "idem-card" / "DCIM";
    const auto destination = root / "idem-dest";
    std::filesystem::create_directories(card);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(card / "IMG_0001.JPG", QColor(10, 20, 30)));
    ASSERT_TRUE(write_jpeg(card / "IMG_0002.JPG", QColor(40, 50, 60)));

    IngestRequest request;
    request.source_root = (root / "idem-card").string();
    request.transport = "filesystem-card";
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;

    auto first = service->execute_ingest_detailed(request);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first.value().import.imported, 2U);
    EXPECT_EQ(first.value().import.duplicates, 0U);
    EXPECT_TRUE(first.value().resume_checkpoint_cleared);

    auto second = service->execute_ingest_detailed(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second.value().import.imported, 0U);
    EXPECT_EQ(second.value().import.duplicates, 2U);
    EXPECT_EQ(second.value().transport, "filesystem-card");
    for (const auto &item : second.value().import.items)
        EXPECT_EQ(item.status, ImportItemStatus::kDuplicate);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 2U);
}

TEST_F(CatalogServiceTest, PtpStubIngestResumesAfterCancelMidBatch)
{
    ASSERT_TRUE(open_service(true));
    const auto fixture = root / "cancel-resume-fixture" / "DCIM";
    const auto destination = root / "cancel-resume-dest";
    std::filesystem::create_directories(fixture);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(fixture / "A.JPG", QColor(1, 2, 3)));
    ASSERT_TRUE(write_jpeg(fixture / "B.JPG", QColor(4, 5, 6)));
    ASSERT_TRUE(write_jpeg(fixture / "C.JPG", QColor(7, 8, 9)));

    IngestRequest request;
    request.source_root = (root / "cancel-resume-fixture").string();
    request.transport = "ptp-stub";
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;

    CancellationSource cancellation;
    request.cancellation = cancellation.token();
    std::optional<std::string> batch_id;
    auto first = service->execute_ingest_detailed(
        request,
        [&](std::size_t, std::size_t, const ImportItemResult *item)
        {
            if (item != nullptr && item->status == ImportItemStatus::kImported)
                static_cast<void>(cancellation.cancel("ingest-cancel-resume"));
        });
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first.value().resume_batch_id);
    batch_id = first.value().resume_batch_id;
    EXPECT_GE(first.value().import.imported, 1U);
    EXPECT_FALSE(first.value().resume_checkpoint_cleared);

    auto catalog = service->snapshot();
    ASSERT_TRUE(catalog);
    auto checkpoint = load_ingest_resume_checkpoint(catalog.value().database_path, *batch_id);
    ASSERT_TRUE(checkpoint) << checkpoint.error().message;
    EXPECT_FALSE(checkpoint.value().completed_relative_paths.empty());

    IngestRequest resume = request;
    resume.cancellation = CancellationToken{};
    resume.resume_batch_id = batch_id;
    auto second = service->execute_ingest_detailed(resume);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().resume_checkpoint_cleared);
    EXPECT_GE(second.value().import.skipped, 1U);
    EXPECT_GE(second.value().import.imported, 1U);
    for (const auto &item : second.value().import.items)
    {
        if (item.status == ImportItemStatus::kSkipped)
            EXPECT_EQ(reason_of(*item.error), "ingest_resume_already_completed");
    }
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 3U);
}

TEST_F(CatalogServiceTest, IngestRejectsUnknownTransportName)
{
    ASSERT_TRUE(open_service(true));
    IngestRequest request;
    request.source_root = root.string();
    request.transport = "not-a-transport";
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = (root / "out").string();
    auto batch = service->execute_ingest_detailed(request);
    ASSERT_FALSE(batch);
    EXPECT_EQ(reason_of(batch.error()), "ingest_transport_unknown");
}

TEST_F(CatalogServiceTest, FilesystemCardIngestHonorsSelectedPathsFilter)
{
    ASSERT_TRUE(open_service(true));
    const auto card = root / "select-card" / "DCIM";
    const auto destination = root / "select-dest";
    std::filesystem::create_directories(card);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_jpeg(card / "KEEP.JPG", QColor(11, 22, 33)));
    ASSERT_TRUE(write_jpeg(card / "SKIP.JPG", QColor(44, 55, 66)));

    IngestRequest request;
    request.source_root = (root / "select-card").string();
    request.transport = "filesystem-card";
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    request.defer_previews = true;
    request.selected_paths = {(card / "KEEP.JPG").string()};

    auto batch = service->execute_ingest_detailed(request);
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_EQ(batch.value().import.imported, 1U);
    EXPECT_EQ(batch.value().import.items.size(), 1U);
    EXPECT_NE(batch.value().import.items.front().input_path.find("KEEP.JPG"), std::string::npos);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 1U);
}

} // namespace ravo
