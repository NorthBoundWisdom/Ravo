#include <QImage>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "catalog_service_test_support.h"
#include "ravo/recipe/develop.h"
#include "ravo/engine/crop_preview.h"

namespace ravo
{
TEST_F(CatalogServiceTest, CroppedPreviewUsesFinalPhotoDensityAndReopensExactCache)
{
    ASSERT_TRUE(open_service(true));
    const auto path = raw_fixture_path();
    const auto source_hash = file_sha256(path);
    auto imported = service->import().import_one(path, {}, ImportPreviewPolicy::kMinimal, true);
    ASSERT_TRUE(imported);
    const auto id = imported.value().asset->id;
    auto recipe = service->develop().load_recipe(id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    params.value().crop_x = .525;
    params.value().crop_y = .426;
    params.value().crop_width = .415;
    params.value().crop_height = .401;
    params.value().straighten_degrees = -.02578125;
    ASSERT_TRUE(service->develop().save_develop(id, params.value()));
    const auto saved_recipe = serialize_recipe(service->develop().load_recipe(id).value()).value();
    PreviewRequest request;
    request.asset_id = id;
    request.max_edge = 512;
    request.persist_preview_record = false;
    auto live = service->preview().request_preview(request, params.value());
    ASSERT_TRUE(live) << live.error().message;
    EXPECT_EQ(std::max(live.value().width, live.value().height), 512U);
    const auto prepared = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    ASSERT_TRUE(prepared[1]);
    EXPECT_GT(*prepared[1], request.max_edge);
    auto changed_params = params.value();
    changed_params.exposure_ev = .5;
    auto changed = service->preview().request_preview(request, changed_params);
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(changed.value().width, live.value().width);
    EXPECT_EQ(changed.value().height, live.value().height);
    EXPECT_EQ(testing::CatalogServiceTestControl::linear_working_max_edges(*service), prepared);
    EXPECT_NE(changed.value().rgb, live.value().rgb);
    request.need_cpu_pixels = false;
    auto native = service->preview().request_preview(request, changed_params);
    ASSERT_TRUE(native) << native.error().message;
#if defined(__APPLE__)
    EXPECT_TRUE(native.value().rgb.empty());
    EXPECT_NE(native.value().gpu_display_native_surface, 0U);
    EXPECT_EQ(native.value().gpu_display_width, changed.value().width);
    EXPECT_EQ(native.value().gpu_display_height, changed.value().height);
#endif
    request.need_cpu_pixels = true;
    auto dimensions = engine.inspect(path, {});
    ASSERT_TRUE(dimensions);
    auto modified = recipe_from_develop(recipe.value().asset, changed_params);
    ASSERT_TRUE(modified);
    auto planned = plan_preview_source_size(modified.value(), dimensions.value().width,
                                            dimensions.value().height, request.max_edge);
    ASSERT_TRUE(planned);
    RenderRequest gold_request;
    gold_request.asset = recipe.value().asset;
    gold_request.recipe = modified.value();
    gold_request.output_width = planned.value().width;
    gold_request.output_height = planned.value().height;
    auto gold = engine.render_to_image(gold_request);
    ASSERT_TRUE(gold) << gold.error().message;
    ASSERT_EQ(gold.value().width, changed.value().width);
    ASSERT_EQ(gold.value().height, changed.value().height);
    ASSERT_EQ(gold.value().rgb.size(), changed.value().rgb.size());
    int max_delta = 0;
    for (std::size_t i = 0; i < gold.value().rgb.size(); ++i)
        max_delta =
            std::max(max_delta, std::abs(int(gold.value().rgb[i]) - int(changed.value().rgb[i])));
    EXPECT_LE(max_delta, 1);
    request.persist_preview_record = true;
    auto settled = service->preview().request_preview(request);
    ASSERT_TRUE(settled) << settled.error().message;
    EXPECT_EQ(settled.value().width, live.value().width);
    QImage cached(QString::fromStdString(settled.value().cache_path));
    EXPECT_EQ(cached.width(), int(settled.value().width));
    EXPECT_EQ(cached.height(), int(settled.value().height));
    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));
    auto reopened = service->preview().request_preview(request);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened.value().cache_path, settled.value().cache_path);
    EXPECT_EQ(reopened.value().width, settled.value().width);
    EXPECT_EQ(reopened.value().height, settled.value().height);
    request.max_edge = kDefaultPreviewMaxEdge;
    auto display_settled = service->preview().request_preview(request);
    ASSERT_TRUE(display_settled);
    request.persist_preview_record = false;
    request.prefer_cached_settled_preview = true;
    auto selection = service->preview().request_preview(request, params.value());
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection.value().cache_path, display_settled.value().cache_path);
    EXPECT_EQ(selection.value().width, display_settled.value().width);
    EXPECT_EQ(selection.value().height, display_settled.value().height);
    EXPECT_EQ(serialize_recipe(service->develop().load_recipe(id).value()).value(), saved_recipe);
    EXPECT_EQ(file_sha256(path), source_hash);
}

TEST_F(CatalogServiceTest, DisplaySizeLiveEditKeepsResolutionAndPreparedRawSource)
{
    ASSERT_TRUE(open_service(true));
    auto imported =
        service->import().import_one(raw_fixture_path(), {}, ImportPreviewPolicy::kMinimal, true);
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto id = imported.value().asset->id;
    auto recipe = service->develop().load_recipe(id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    PreviewRequest request;
    request.asset_id = id;
    request.max_edge = kDefaultPreviewMaxEdge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = false;
    auto initial = service->preview().request_preview(request, params.value());
    ASSERT_TRUE(initial) << initial.error().message;
    const auto revision = service->library().snapshot().value().revision;
    const auto sources_before =
        testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    params.value().exposure_ev = .5;
    auto changed = service->preview().request_preview(request, params.value());
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(changed.value().width, initial.value().width);
    EXPECT_EQ(changed.value().height, initial.value().height);
    EXPECT_EQ(std::max(changed.value().width, changed.value().height), kDefaultPreviewMaxEdge);
    EXPECT_NE(changed.value().rgb, initial.value().rgb);
    EXPECT_EQ(testing::CatalogServiceTestControl::linear_working_max_edges(*service),
              sources_before);
    EXPECT_EQ(sources_before[1], kDefaultPreviewMaxEdge);
    EXPECT_FALSE(sources_before[0].has_value());
    EXPECT_EQ(service->library().snapshot().value().revision, revision);
    EXPECT_EQ(serialize_recipe(service->develop().load_recipe(id).value()).value(),
              serialize_recipe(recipe.value()).value());
    request.need_cpu_pixels = false;
    auto native = service->preview().request_preview(request, params.value());
    ASSERT_TRUE(native) << native.error().message;
#if defined(__APPLE__)
    EXPECT_FALSE(native.value().gpu_backend.empty());
    EXPECT_TRUE(native.value().rgb.empty());
    EXPECT_NE(native.value().gpu_display_native_surface, 0U);
    EXPECT_EQ(native.value().gpu_display_width, changed.value().width);
    EXPECT_EQ(native.value().gpu_display_height, changed.value().height);
#endif
    auto modified = recipe_from_develop(recipe.value().asset, params.value());
    ASSERT_TRUE(modified);
    RenderRequest gold_request;
    gold_request.asset = recipe.value().asset;
    gold_request.recipe = modified.value();
    gold_request.output_width = initial.value().width;
    gold_request.output_height = initial.value().height;
    auto gold = engine.render_to_image(gold_request);
    ASSERT_TRUE(gold) << gold.error().message;
    ASSERT_EQ(gold.value().rgb.size(), changed.value().rgb.size());
    int max_delta = 0;
    for (std::size_t i = 0; i < gold.value().rgb.size(); ++i)
        max_delta =
            std::max(max_delta, std::abs(int(gold.value().rgb[i]) - int(changed.value().rgb[i])));
    EXPECT_LE(max_delta, 1);
}
} // namespace ravo
