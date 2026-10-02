#include <algorithm>
#include <cmath>
#include <QImage>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/adapters/text_file.h"

namespace ravo
{
TEST_F(CatalogServiceTest, SameSizeInspectPansMatchTheirCpuExportRegions)
{
    ASSERT_TRUE(open_service(true));
    const auto source_hash = file_sha256(raw_fixture_path());
    auto imported =
        service->import_one(raw_fixture_path(), {}, ImportPreviewPolicy::kMinimal, true);
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto before_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(before_recipe);
    const auto serialized_before = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(serialized_before);
    const auto revision_before = service->snapshot().value().revision;
    ExportRequest output;
    output.asset_id = asset_id;
    output.output_path = (root / "cpu-full.png").string();
    output.format = ExportFormat::kPng;
    auto exported = service->export_asset(output);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage gold =
        QImage(QString::fromStdString(output.output_path)).convertToFormat(QImage::Format_RGB888);
    ASSERT_FALSE(gold.isNull());
    std::vector<std::uint8_t> first_pixels;
    for (const auto rect :
         {PreviewNormRect{.25, .25, .20, .15}, PreviewNormRect{.45, .25, .20, .15},
          PreviewNormRect{.45, .45, .20, .15}, PreviewNormRect{.25, .25, .20, .15}})
    {
        PreviewRequest request;
        request.asset_id = asset_id;
        request.roi = rect;
        request.persist_preview_record = false;
        request.prefer_embedded_preview = false;
        request.need_cpu_pixels = true;
        auto preview = service->request_preview(request);
        ASSERT_TRUE(preview) << preview.error().message;
        const int x = static_cast<int>(std::llround(rect.x * gold.width()));
        const int y = static_cast<int>(std::llround(rect.y * gold.height()));
        const auto width = static_cast<std::uint32_t>(std::llround(rect.width * gold.width()));
        const auto height = static_cast<std::uint32_t>(std::llround(rect.height * gold.height()));
        ASSERT_EQ(preview.value().width, width);
        ASSERT_EQ(preview.value().height, height);
        ASSERT_EQ(preview.value().rgb.size(), std::size_t(width) * height * 3);
        int max_delta = 0;
        for (std::uint32_t row = 0; row < height; ++row)
        {
            const auto *expected = gold.constScanLine(y + static_cast<int>(row)) + x * 3;
            for (std::size_t col = 0; col < std::size_t(width) * 3; ++col)
                max_delta =
                    std::max(max_delta,
                             std::abs(int(preview.value().rgb[std::size_t(row) * width * 3 + col]) -
                                      int(expected[col])));
        }
        EXPECT_LE(max_delta, 1) << "ROI " << rect.x << ',' << rect.y;
        if (rect.x == .25 && rect.y == .25)
        {
            if (first_pixels.empty())
                first_pixels = preview.value().rgb;
            else
                EXPECT_EQ(first_pixels, preview.value().rgb);
        }
        else
            EXPECT_NE(first_pixels, preview.value().rgb);
    }
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
    EXPECT_EQ(service->snapshot().value().revision, revision_before);
    auto after_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(after_recipe);
    EXPECT_EQ(serialize_recipe(after_recipe.value()).value(), serialized_before.value());
}
} // namespace ravo
