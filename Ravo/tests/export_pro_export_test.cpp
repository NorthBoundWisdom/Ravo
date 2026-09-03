#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/domain/types.h"
#include "ravo/recipe/recipe.h"

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

TEST_F(CatalogServiceTest, DeliveryWatermarkEqualityPrivacyAndNoRecipeMutation)
{
    ASSERT_TRUE(open_service(true));

    const auto path = (root / "wm-src.jpg").string();
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 95));
    auto imported = service->import_one(path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset.has_value());
    const auto asset_id = imported.value().asset->id;

    auto before_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    const auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    ExportRequest off;
    off.asset_id = asset_id;
    off.output_path = (root / "wm-off.png").string();
    off.format = ExportFormat::kPng;
    off.metadata_mode = ExportMetadataMode::kNoLocation;
    auto off_result = service->export_asset(off);
    ASSERT_TRUE(off_result) << off_result.error().message;

    ExportRequest on = off;
    on.output_path = (root / "wm-on.png").string();
    on.watermark.enabled = true;
    on.watermark.text = "RAVO";
    on.watermark.opacity = 1.0;
    on.watermark.scale_percent = 20.0;
    on.watermark.alignment = "bottom_right";
    auto on_result = service->export_asset(on);
    ASSERT_TRUE(on_result) << on_result.error().message;
    EXPECT_EQ(on_result.value().width, off_result.value().width);
    EXPECT_EQ(on_result.value().height, off_result.value().height);

    QImage off_image(QString::fromStdString(off.output_path));
    QImage on_image(QString::fromStdString(on.output_path));
    ASSERT_FALSE(off_image.isNull());
    ASSERT_FALSE(on_image.isNull());
    ASSERT_EQ(off_image.size(), on_image.size());
    bool differ = false;
    for (int y = 0; y < off_image.height() && !differ; ++y)
    {
        for (int x = 0; x < off_image.width(); ++x)
        {
            if (off_image.pixel(x, y) != on_image.pixel(x, y))
            {
                differ = true;
                break;
            }
        }
    }
    EXPECT_TRUE(differ);

    ExportRequest again = on;
    again.output_path = (root / "wm-on-again.png").string();
    auto again_result = service->export_asset(again);
    ASSERT_TRUE(again_result) << again_result.error().message;
    QImage again_image(QString::fromStdString(again.output_path));
    ASSERT_FALSE(again_image.isNull());
    ASSERT_EQ(on_image.size(), again_image.size());
    for (int y = 0; y < on_image.height(); ++y)
    {
        for (int x = 0; x < on_image.width(); ++x)
            EXPECT_EQ(on_image.pixel(x, y), again_image.pixel(x, y)) << x << "," << y;
    }

    auto after_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    const auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(before_json.value(), after_json.value());

    ExportRequest none_meta = on;
    none_meta.output_path = (root / "wm-none-meta.png").string();
    none_meta.metadata_mode = ExportMetadataMode::kNone;
    auto none_result = service->export_asset(none_meta);
    ASSERT_TRUE(none_result) << none_result.error().message;
    QImage none_image(QString::fromStdString(none_meta.output_path));
    ASSERT_FALSE(none_image.isNull());
    for (int y = 0; y < on_image.height(); ++y)
    {
        for (int x = 0; x < on_image.width(); ++x)
            EXPECT_EQ(on_image.pixel(x, y), none_image.pixel(x, y)) << x << "," << y;
    }

    ExportRequest original;
    original.asset_id = asset_id;
    original.output_path = (root / "wm-original.jpg").string();
    original.format = ExportFormat::kOriginalCopy;
    original.watermark.enabled = true;
    auto rejected = service->export_asset(original);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "original_copy_resize_not_applicable");
}

TEST_F(CatalogServiceTest, DeliveryColourAndFrameEqualityAndNoRecipeMutation)
{
    ASSERT_TRUE(open_service(true));

    const auto path = (root / "cf-src.jpg").string();
    QImage image(80, 60, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 90, 150));
    ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 95));
    auto imported = service->import_one(path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset.has_value());
    const auto asset_id = imported.value().asset->id;

    auto before_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    const auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    ExportRequest baseline;
    baseline.asset_id = asset_id;
    baseline.output_path = (root / "cf-base.png").string();
    baseline.format = ExportFormat::kPng;
    auto base_out = service->export_asset(baseline);
    ASSERT_TRUE(base_out) << base_out.error().message;

    ExportRequest framed = baseline;
    framed.output_path = (root / "cf-frame.png").string();
    framed.frame.enabled = true;
    framed.frame.size = 0.2;
    framed.frame.border_color = {1.0, 1.0, 1.0};
    auto frame_out = service->export_asset(framed);
    ASSERT_TRUE(frame_out) << frame_out.error().message;
    EXPECT_GT(frame_out.value().width, base_out.value().width);
    EXPECT_GT(frame_out.value().height, base_out.value().height);

    ExportRequest framed_again = framed;
    framed_again.output_path = (root / "cf-frame-again.png").string();
    auto again = service->export_asset(framed_again);
    ASSERT_TRUE(again) << again.error().message;
    QImage first(QString::fromStdString(framed.output_path));
    QImage second(QString::fromStdString(framed_again.output_path));
    ASSERT_FALSE(first.isNull());
    ASSERT_FALSE(second.isNull());
    ASSERT_EQ(first.size(), second.size());
    for (int y = 0; y < first.height(); ++y)
    {
        for (int x = 0; x < first.width(); ++x)
            EXPECT_EQ(first.pixel(x, y), second.pixel(x, y)) << x << "," << y;
    }

    ExportRequest colored = baseline;
    colored.output_path = (root / "cf-color.png").string();
    colored.output_color.enabled = true;
    colored.output_color.output_profile = "adobe_rgb";
    colored.output_color.rendering_intent = "relative_colorimetric";
    auto color_out = service->export_asset(colored);
    ASSERT_TRUE(color_out) << color_out.error().message;
    EXPECT_EQ(color_out.value().width, base_out.value().width);
    EXPECT_EQ(color_out.value().height, base_out.value().height);

    ExportRequest ordered = baseline;
    ordered.output_path = (root / "cf-ordered.png").string();
    ordered.frame.enabled = true;
    ordered.frame.size = 0.15;
    ordered.watermark.enabled = true;
    ordered.watermark.text = "RAVO";
    ordered.watermark.opacity = 1.0;
    ordered.watermark.scale_percent = 18.0;
    ordered.watermark.alignment = "bottom_right";
    auto ordered_out = service->export_asset(ordered);
    ASSERT_TRUE(ordered_out) << ordered_out.error().message;
    EXPECT_GT(ordered_out.value().width, base_out.value().width);

    auto after_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    const auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(before_json.value(), after_json.value());

    ExportRequest original;
    original.asset_id = asset_id;
    original.output_path = (root / "cf-original.jpg").string();
    original.format = ExportFormat::kOriginalCopy;
    original.frame.enabled = true;
    auto rejected_frame = service->export_asset(original);
    ASSERT_FALSE(rejected_frame);
    EXPECT_EQ(rejected_frame.error().context.at("reason"), "original_copy_resize_not_applicable");

    original.frame.enabled = false;
    original.output_color.enabled = true;
    auto rejected_color = service->export_asset(original);
    ASSERT_FALSE(rejected_color);
    EXPECT_EQ(rejected_color.error().context.at("reason"), "original_copy_resize_not_applicable");
}

} // namespace
} // namespace ravo
