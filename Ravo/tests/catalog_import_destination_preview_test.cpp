#include <QColorSpace>
#include <QImage>
#include <fstream>
#include <chrono>
#include "catalog_test_support.h"

namespace ravo
{
TEST_F(CatalogServiceTest, DestinationPreviewCacheRevalidatesSourceAndCatalogMembership)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source.png";
    const auto destination = root / "destination";
    std::filesystem::create_directory(destination);
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(QString::fromStdString(source.string())));
    const auto original_time = std::filesystem::last_write_time(source);
    const auto original_size = std::filesystem::file_size(source);
    ImportRequest request;
    request.inputs = {source.string()};
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kCaptureMonth;
    request.destination_directory = destination.string();
    request.skip_existing = true;
    auto first = service->import().preview_import_destinations(request);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_EQ(first.value().photo_count, 1U);
    // Preview identity is deliberately provisional. A same-size/time rewrite
    // can reuse metadata, but formal import must inspect/hash the actual bytes.
    {
        std::ofstream corrupt(source, std::ios::binary | std::ios::trunc);
        corrupt << std::string(original_size, 'x');
    }
    std::filesystem::last_write_time(source, original_time);
    auto reused = service->import().preview_import_destinations(request);
    ASSERT_TRUE(reused) << reused.error().message;
    EXPECT_EQ(reused.value().photo_count, 1U);
    EXPECT_FALSE(service->import().preflight_import(request));
    std::filesystem::last_write_time(source, original_time + std::chrono::hours(24));
    EXPECT_FALSE(service->import().preview_import_destinations(request));
    ASSERT_TRUE(image.save(QString::fromStdString(source.string())));
    std::filesystem::last_write_time(source, original_time + std::chrono::hours(24 * 400));
    auto changed = service->import().preview_import_destinations(request);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_NE(changed.value().folders.back().path, first.value().folders.back().path);
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    auto imported =
        service->import().import_one(source.string(), {}, ImportPreviewPolicy::kMinimal, true);
    ASSERT_TRUE(imported) << imported.error().message;
    auto duplicate = service->import().preview_import_destinations(request);
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_EQ(duplicate.value().photo_count, 0U);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    request.cancellation = cancellation.token();
    EXPECT_FALSE(service->import().preview_import_destinations(request));
    ASSERT_TRUE(service->close());
    request.cancellation = {};
    EXPECT_FALSE(service->import().preview_import_destinations(request));
}

TEST_F(CatalogServiceTest, DestinationPreviewPerformanceProbe)
{
    const auto source = qEnvironmentVariable("RAVO_IMPORT_PREVIEW_SOURCE");
    if (source.isEmpty())
        GTEST_SKIP() << "Set RAVO_IMPORT_PREVIEW_SOURCE to an explicit read-only photo directory";
    ASSERT_TRUE(std::filesystem::is_directory(source.toStdString()));
    ASSERT_TRUE(open_service(true));
    const auto destination = root / "destination";
    std::filesystem::create_directory(destination);
    ImportRequest request;
    request.inputs = {source.toStdString()};
    request.source_root = source.toStdString();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kCaptureMonth;
    request.destination_directory = destination.string();
    const auto measure = [&](const char *name)
    {
        const auto start = std::chrono::steady_clock::now();
        std::size_t partial_count = 0;
        auto preview = service->import().preview_import_destinations(
            request,
            [&](const ImportDestinationPreview &partial)
            {
                if (partial_count++ == 0)
                    RecordProperty(
                        std::string(name) + "_first",
                        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count()));
                EXPECT_GT(partial.photo_count, 0U);
            });
        RecordProperty(name, std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - start)
                                                .count()));
        RecordProperty(std::string(name) + "_updates", std::to_string(partial_count));
        return preview;
    };
    auto cold = measure("cold_month_ms");
    ASSERT_TRUE(cold) << cold.error().message;
    auto warm = measure("warm_month_ms");
    ASSERT_TRUE(warm) << warm.error().message;
    EXPECT_EQ(warm.value().photo_count, cold.value().photo_count);
    request.organization = ImportOrganization::kCaptureDate;
    auto date = measure("warm_date_ms");
    ASSERT_TRUE(date) << date.error().message;
    EXPECT_EQ(date.value().photo_count, cold.value().photo_count);
    request.organization = ImportOrganization::kSingleFolder;
    auto single = measure("single_ms");
    ASSERT_TRUE(single) << single.error().message;
    EXPECT_EQ(single.value().photo_count, cold.value().photo_count);
    request.organization = ImportOrganization::kPreserveHierarchy;
    auto hierarchy = measure("hierarchy_ms");
    ASSERT_TRUE(hierarchy) << hierarchy.error().message;
    EXPECT_EQ(hierarchy.value().photo_count, cold.value().photo_count);
    RecordProperty("photos", std::to_string(cold.value().photo_count));
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    EXPECT_TRUE(service->library().list_assets().value().empty());
}

TEST_F(CatalogServiceTest, DestinationPreviewMatchesAllOrganizationsWithoutPublishing)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source";
    std::filesystem::create_directories(source / "nested");
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(QString::fromStdString((source / "a.png").string())));
    image.fill(Qt::blue);
    ASSERT_TRUE(image.save(QString::fromStdString((source / "nested" / "b.png").string())));
    const auto original_hash = file_sha256((source / "a.png").string());
    int index = 0;
    for (const auto organization :
         {ImportOrganization::kSingleFolder, ImportOrganization::kPreserveHierarchy,
          ImportOrganization::kCaptureDate, ImportOrganization::kCaptureMonth})
    {
        const auto destination = root / ("destination-" + std::to_string(index));
        const auto second = root / ("second-" + std::to_string(index++));
        std::filesystem::create_directory(destination);
        std::filesystem::create_directory(second);
        ImportRequest request;
        request.inputs = {source.string()};
        request.source_root = source.string();
        request.mode = ImportTransferMode::kCopy;
        request.organization = organization;
        request.destination_directory = destination.string();
        request.second_copy_directory = second.string();
        request.filename_template = "{stem}-{sequence}{ext}";
        request.defer_previews = true;
        const auto revision = service->library().snapshot().value().revision;
        auto preview = service->import().preview_import_destinations(request);
        ASSERT_TRUE(preview) << preview.error().message;
        EXPECT_EQ(preview.value().schema, "ravo-import-destination-preview/v2");
        EXPECT_EQ(preview.value().photo_count, 2U);
        EXPECT_EQ(preview.value().catalog_revision, revision);
        EXPECT_EQ(service->library().snapshot().value().revision, revision);
        EXPECT_TRUE(std::filesystem::is_empty(destination));
        EXPECT_TRUE(std::filesystem::is_empty(second));
        EXPECT_EQ(file_sha256((source / "a.png").string()), original_hash);
        std::size_t roots = 0;
        for (const auto &folder : preview.value().folders)
        {
            if (folder.depth == 0)
            {
                ++roots;
                EXPECT_FALSE(folder.will_create);
                EXPECT_EQ(folder.photo_count, 2U);
            }
            else
                EXPECT_TRUE(folder.will_create);
        }
        EXPECT_EQ(roots, 2U);
        const std::size_t expected_per_root = organization == ImportOrganization::kSingleFolder ?
                                                  1 :
                                              organization == ImportOrganization::kCaptureDate ? 4 :
                                                                                                 3;
        EXPECT_EQ(preview.value().folders.size(), expected_per_root * 2);
        auto imported = service->import().execute_import(request);
        ASSERT_TRUE(imported) << imported.error().message;
        EXPECT_EQ(imported.value().imported, 2U);
        for (const auto &folder : preview.value().folders)
            EXPECT_TRUE(std::filesystem::is_directory(folder.path));
        EXPECT_EQ(file_sha256((source / "a.png").string()), original_hash);
    }
}

TEST_F(CatalogServiceTest, DestinationPreviewRejectsStaleCancelledConflictingAndCorruptInputs)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source.png";
    const auto destination = root / "destination";
    std::filesystem::create_directory(destination);
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(QString::fromStdString(source.string())));
    ImportRequest request;
    request.inputs = {source.string()};
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.expected_catalog_revision = service->library().snapshot().value().revision + 1;
    auto stale = service->import().preview_import_destinations(request);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    request.expected_catalog_revision.reset();
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    request.cancellation = cancellation.token();
    auto cancelled = service->import().preview_import_destinations(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    request.cancellation = {};
    std::filesystem::copy_file(source, destination / "source.png");
    auto conflict = service->import().preview_import_destinations(request);
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict.value().photo_count, 1U);
    auto preflight = service->import().preflight_import(request);
    ASSERT_FALSE(preflight);
    EXPECT_EQ(preflight.error().code, ErrorCode::kConflict);
    request.organization = ImportOrganization::kPreserveHierarchy;
    {
        std::ofstream blocker(destination / root.filename());
        blocker << "folder is a file";
    }
    auto blocked_folder = service->import().preview_import_destinations(request);
    ASSERT_FALSE(blocked_folder);
    EXPECT_EQ(blocked_folder.error().code, ErrorCode::kConflict);
    request.organization = ImportOrganization::kSingleFolder;
    const auto corrupt = root / "corrupt.png";
    {
        std::ofstream file(corrupt);
        file << "not a photo";
    }
    request.inputs = {corrupt.string()};
    // A path-only projection is provisional; validating image bytes belongs to
    // formal preflight, while a date-based projection must read valid metadata.
    auto provisional = service->import().preview_import_destinations(request);
    ASSERT_TRUE(provisional) << provisional.error().message;
    EXPECT_EQ(provisional.value().photo_count, 1U);
    EXPECT_FALSE(service->import().preflight_import(request));
    request.organization = ImportOrganization::kCaptureMonth;
    EXPECT_FALSE(service->import().preview_import_destinations(request));
    request.organization = ImportOrganization::kSingleFolder;
    request.filename_template = "{date}-{stem}{ext}";
    EXPECT_FALSE(service->import().preview_import_destinations(request));
    EXPECT_FALSE(std::filesystem::exists(destination / "corrupt.png"));
    EXPECT_TRUE(service->library().list_assets().value().empty());
}

TEST_F(CatalogServiceTest, DestinationPreviewPublishesPartialCountsAndCancelsWithoutWrites)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source";
    const auto destination = root / "destination";
    const auto second = root / "second";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(destination);
    std::filesystem::create_directory(second);
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::green);
    for (const auto *name : {"a.png", "b.png", "c.png"})
        ASSERT_TRUE(image.save(QString::fromStdString((source / name).string())));
    const auto hash = file_sha256((source / "a.png").string());
    const auto revision = service->library().snapshot().value().revision;
    ImportRequest request;
    request.inputs = {source.string()};
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kCaptureMonth;
    request.destination_directory = destination.string();
    request.second_copy_directory = second.string();
    std::vector<std::size_t> counts;
    const auto observe = [&](const ImportDestinationPreview &partial)
    {
        EXPECT_EQ(partial.catalog_revision, revision);
        EXPECT_EQ(partial.folders.size(), 6U);
        EXPECT_GT(partial.photo_count, counts.empty() ? 0U : counts.back());
        for (const auto &folder : partial.folders)
            EXPECT_EQ(folder.photo_count, partial.photo_count);
        counts.push_back(partial.photo_count);
    };
    auto complete = service->import().preview_import_destinations(request, observe);
    ASSERT_TRUE(complete) << complete.error().message;
    ASSERT_FALSE(counts.empty());
    EXPECT_EQ(counts.front(), 1U);
    EXPECT_EQ(complete.value().photo_count, 3U);
    EXPECT_LE(counts.back(), complete.value().photo_count);
    CancellationSource cancellation;
    request.cancellation = cancellation.token();
    counts.clear();
    auto cancelled =
        service->import().preview_import_destinations(request,
                                                      [&](const ImportDestinationPreview &partial)
                                                      {
                                                          observe(partial);
                                                          EXPECT_TRUE(cancellation.cancel());
                                                      });
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(counts, (std::vector<std::size_t>{1U}));
    request.cancellation = {};
    auto stale = service->import().preview_import_destinations(
        request,
        [&](const ImportDestinationPreview &)
        {
            ASSERT_TRUE(service->import().import_one((source / "a.png").string(), {},
                                                     ImportPreviewPolicy::kMinimal, true));
        });
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    EXPECT_EQ(stale.error().context.at("reason"), "import_scan_stale");
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    EXPECT_TRUE(std::filesystem::is_empty(second));
    EXPECT_EQ(file_sha256((source / "a.png").string()), hash);
}

TEST_F(CatalogServiceTest, DestinationPreviewRawCaptureDateMatchesFormalImport)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source.cr2";
    std::filesystem::copy_file(raw_fixture_path(), source);
    const auto hash = file_sha256(source.string());
    for (const auto organization :
         {ImportOrganization::kCaptureDate, ImportOrganization::kCaptureMonth})
    {
        const auto destination =
            root / (organization == ImportOrganization::kCaptureDate ? "date" : "month");
        std::filesystem::create_directory(destination);
        ImportRequest request;
        request.inputs = {source.string()};
        request.mode = ImportTransferMode::kCopy;
        request.organization = organization;
        request.destination_directory = destination.string();
        request.filename_template = "{date}-{stem}{ext}";
        request.defer_previews = true;
        auto preview = service->import().preview_import_destinations(request);
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_EQ(preview.value().photo_count, 1U);
        EXPECT_TRUE(std::filesystem::is_empty(destination));
        auto imported = service->import().execute_import(request);
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().imported, 1U);
        for (const auto &folder : preview.value().folders)
            EXPECT_TRUE(std::filesystem::is_directory(folder.path));
    }
    EXPECT_EQ(file_sha256(source.string()), hash);
}

TEST_F(CatalogServiceTest, DestinationPreviewDefersContentIdentityChecksToImport)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source.png";
    const auto destination = root / "destination";
    std::filesystem::create_directory(destination);
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(QString::fromStdString(source.string())));
    ImportRequest request;
    request.inputs = {source.string()};
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.skip_existing = true;
    request.expected_content_hashes = {{source.string(), std::string(64, '0')}};
    auto preview = service->import().preview_import_destinations(request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().photo_count, 1U);
    auto preflight = service->import().preflight_import(request);
    ASSERT_FALSE(preflight);
    EXPECT_EQ(preflight.error().context.at("reason"), "import_content_source_changed");
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    EXPECT_TRUE(service->library().list_assets().value().empty());
}
} // namespace ravo
