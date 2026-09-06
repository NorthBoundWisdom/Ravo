#include <QColorSpace>
#include <QImage>
#include <fstream>
#include "catalog_test_support.h"

namespace ravo
{
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
        const auto revision = service->snapshot().value().revision;
        auto preview = service->preview_import_destinations(request);
        ASSERT_TRUE(preview) << preview.error().message;
        EXPECT_EQ(preview.value().schema, "ravo-import-destination-preview/v1");
        EXPECT_EQ(preview.value().photo_count, 2U);
        EXPECT_EQ(preview.value().catalog_revision, revision);
        EXPECT_EQ(service->snapshot().value().revision, revision);
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
        auto imported = service->execute_import(request);
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
    request.expected_catalog_revision = service->snapshot().value().revision + 1;
    auto stale = service->preview_import_destinations(request);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    request.expected_catalog_revision.reset();
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    request.cancellation = cancellation.token();
    auto cancelled = service->preview_import_destinations(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    request.cancellation = {};
    std::filesystem::copy_file(source, destination / "source.png");
    auto conflict = service->preview_import_destinations(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    const auto corrupt = root / "corrupt.png";
    {
        std::ofstream file(corrupt);
        file << "not a photo";
    }
    request.inputs = {corrupt.string()};
    EXPECT_FALSE(service->preview_import_destinations(request));
    EXPECT_FALSE(std::filesystem::exists(destination / "corrupt.png"));
    EXPECT_TRUE(service->list_assets().value().empty());
}
} // namespace ravo
