#include <QColorSpace>
#include <QImage>

#include "ravo/recipe/develop.h"
#include "catalog_test_support.h"

namespace ravo
{
namespace
{

TEST_F(CatalogServiceTest, RasterDehazeInvalidatesCachePersistsReopensAndExportsExactPixels)
{
    QImage image(1281, 961, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(50 + 5 * (x % 33), 60 + 6 * (y % 25), 120));
    const auto source = (root / "declared.png").string();
    ASSERT_TRUE(image.save(QString::fromStdString(source), "PNG"));
    const auto source_hash = file_sha256(source);
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(source, {});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 960U;
    preview.prefer_embedded_preview = false;
    PreviewRequest interactive = preview;
    interactive.persist_preview_record = false;
    auto warm = service->preview().request_preview(interactive);
    ASSERT_TRUE(warm) << warm.error().message;
    auto before = service->preview().request_preview(preview);
    ASSERT_TRUE(before) << before.error().message;
    const QImage baseline(QString::fromStdString(before.value().cache_path));
    ASSERT_FALSE(baseline.isNull());
    auto recipe = service->develop().load_recipe(asset_id);
    ASSERT_TRUE(recipe);
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop);
    develop.value().dehaze = 0.4;
    develop.value().dehaze_distance = 0.2;
    develop.value().dehaze_adaptive = false;
    ASSERT_TRUE(service->develop().save_develop(asset_id, develop.value()));
    warm = service->preview().request_preview(interactive);
    ASSERT_TRUE(warm) << warm.error().message;
    auto changed = service->preview().request_preview(preview);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_NE(changed.value().cache_key, before.value().cache_key);
    const QImage expected(QString::fromStdString(changed.value().cache_path));
    ASSERT_FALSE(expected.isNull());
    EXPECT_NE(expected, baseline);

    // The same retained decoded source must recompute when strength changes.
    develop.value().dehaze = -0.4;
    ASSERT_TRUE(service->develop().save_develop(asset_id, develop.value()));
    auto negative = service->preview().request_preview(preview);
    ASSERT_TRUE(negative) << negative.error().message;
    EXPECT_NE(negative.value().cache_key, changed.value().cache_key);
    EXPECT_NE(QImage(QString::fromStdString(negative.value().cache_path)), expected);
    develop.value().dehaze = 0.4;
    ASSERT_TRUE(service->develop().save_develop(asset_id, develop.value()));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->develop().load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe);
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored);
    EXPECT_DOUBLE_EQ(restored.value().dehaze, 0.4);
    EXPECT_DOUBLE_EQ(restored.value().dehaze_distance, 0.2);
    EXPECT_FALSE(restored.value().dehaze_adaptive);
    auto reopened = service->preview().request_preview(preview);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened.value().cache_key, changed.value().cache_key);
    EXPECT_EQ(QImage(QString::fromStdString(reopened.value().cache_path)), expected);
    ExportRequest request;
    request.asset_id = asset_id;
    request.output_path = (root / "dehazed.png").string();
    request.format = ExportFormat::kPng;
    request.max_edge = 960U;
    auto exported = service->exports().export_asset(request);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(QImage(QString::fromStdString(request.output_path)), expected);
    EXPECT_EQ(file_sha256(source), source_hash);
}

} // namespace
} // namespace ravo
