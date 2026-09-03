#include <filesystem>
#include <string>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/domain/types.h"

namespace ravo
{
namespace
{

TEST_F(CatalogServiceTest, BoxFitExportAndRestartableJobRetainDelivered)
{
    ASSERT_TRUE(open_service(true));

    const auto import_image = [&](const std::string &name, const QColor color)
    {
        const auto path = (root / name).string();
        QImage image(64, 32, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(color);
        EXPECT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        EXPECT_TRUE(imported) << imported.error().message;
        EXPECT_TRUE(imported.value().asset.has_value());
        return imported.value().asset->id;
    };
    const auto first_id = import_image("box-a.jpg", QColor(200, 20, 30));
    const auto second_id = import_image("box-b.jpg", QColor(20, 200, 30));

    ExportRequest sized;
    sized.asset_id = first_id;
    sized.output_path = (root / "box.png").string();
    sized.format = ExportFormat::kPng;
    sized.max_width = 40;
    sized.max_height = 30;
    auto exported = service->export_asset(sized);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported.value().width, 40U);
    EXPECT_EQ(exported.value().height, 20U);

    ExportRequest sharpened = sized;
    sharpened.output_path = (root / "sharpen.png").string();
    sharpened.output_sharpen.enabled = true;
    sharpened.output_sharpen.amount = 0.8;
    sharpened.output_sharpen.radius = 0.6;
    sharpened.output_sharpen.threshold = 0.0;
    auto sharpened_out = service->export_asset(sharpened);
    ASSERT_TRUE(sharpened_out) << sharpened_out.error().message;
    EXPECT_EQ(sharpened_out.value().width, 40U);
    EXPECT_EQ(sharpened_out.value().height, 20U);

    ExportRequest original;
    original.asset_id = first_id;
    original.output_path = (root / "original-copy.jpg").string();
    original.format = ExportFormat::kOriginalCopy;
    original.max_width = 40;
    auto rejected = service->export_asset(original);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "original_copy_resize_not_applicable");

    ExportBatchRequest batch;
    batch.asset_ids = {first_id, second_id};
    batch.output_directory = (root / "job-out").string();
    std::filesystem::create_directory(batch.output_directory);
    batch.filename_template = "{stem}-{sequence}{ext}";
    batch.options.format = ExportFormat::kPng;
    batch.options.max_edge = 48;
    auto job = service->create_export_job(batch, "job-1");
    ASSERT_TRUE(job) << job.error().message;
    ASSERT_EQ(job.value().items.size(), 2U);

    ExportRequest first;
    static_cast<ExportOptions &>(first) = batch.options;
    first.asset_id = job.value().items[0].asset_id;
    first.output_path = job.value().items[0].output_path;
    auto first_out = service->export_asset(first);
    ASSERT_TRUE(first_out) << first_out.error().message;
    job.value().items[0].status = ExportJobItemStatus::kDelivered;

    auto resumed = service->resume_export_job(job.value());
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_EQ(resumed.value().items[0].status, ExportJobItemStatus::kDelivered);
    EXPECT_EQ(resumed.value().items[1].status, ExportJobItemStatus::kDelivered);
    EXPECT_TRUE(std::filesystem::exists(resumed.value().items[0].output_path));
    EXPECT_TRUE(std::filesystem::exists(resumed.value().items[1].output_path));

    auto again = service->resume_export_job(resumed.value());
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again.value().items[0].status, ExportJobItemStatus::kDelivered);
    EXPECT_EQ(again.value().items[1].status, ExportJobItemStatus::kDelivered);
}

} // namespace
} // namespace ravo
