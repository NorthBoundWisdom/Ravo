#include <gtest/gtest.h>
#include "image_ops.h"
#include "mask_evaluator.h"
#include "ravo/engine/mask_geometry.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/recipe/develop_mask.h"

namespace ravo
{
TEST(LocalAdjustmentEngineTest, FusedLightControlsHonorBypassedChildren)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "scope", 1));
    auto local = local_adjustment_develop(params, "scope");
    ASSERT_TRUE(local);
    local.value().highlights = -0.4;
    local.value().shadows = 0.7;
    ASSERT_TRUE(set_local_adjustment_develop(params, "scope", local.value()));
    auto group = params.local_adjustments[0].operation;
    ASSERT_EQ(group.children.back().id, "ravo.core.shadows");
    group.children.back().bypass = true;
    WorkingImage input;
    input.width = input.height = 16;
    input.rgb.resize(16 * 16 * 3, 0.1F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = kInputProfileLinearRec709;
    Recipe recipe;
    recipe.masks = params.masks;
    auto actual = apply_local_adjustment(input, recipe, group, {});
    ASSERT_TRUE(actual);
    group.children.pop_back();
    auto expected = apply_local_adjustment(input, recipe, group, {});
    ASSERT_TRUE(expected);
    EXPECT_EQ(actual.value().rgb, expected.value().rgb);
}

TEST(LocalAdjustmentEngineTest, DrawnGradientEndpointsMatchTheActualAlpha)
{
    for (const auto &points : {std::vector<LocalMaskPoint>{{0.205, 0.505}, {0.805, 0.505}},
                               std::vector<LocalMaskPoint>{{0.505, 0.205}, {0.505, 0.805}}})
    {
        DevelopParams params;
        ASSERT_TRUE(create_local_adjustment(params, "gradient", 2));
        auto local = local_adjustment_develop(params, "gradient");
        ASSERT_TRUE(local);
        ASSERT_TRUE(author_local_mask_gesture(local.value(), points, "draw", 1));
        ASSERT_TRUE(set_local_adjustment_develop(params, "gradient", local.value()));
        auto recipe = recipe_from_develop({"asset", "photo.png", std::nullopt}, params);
        ASSERT_TRUE(recipe);
        WorkingImage input;
        input.width = input.height = 100;
        input.rgb.resize(100 * 100 * 3, 0.25F);
        input.color_profile.model = ColorModel::kRgb;
        input.color_profile.identifier = kInputProfileLinearRec709;
        AlphaPlane alpha;
        ASSERT_TRUE(apply_local_adjustment(input, recipe.value(),
                                           params.local_adjustments[0].operation, {}, &alpha));
        const auto index = [](const auto &point)
        {
            return static_cast<std::size_t>(point.y * 100) * 100 +
                   static_cast<std::size_t>(point.x * 100);
        };
        EXPECT_NEAR(alpha.alpha[index(points.front())], 1.0, 1e-6);
        EXPECT_NEAR(alpha.alpha[index(points.back())], 0.0, 1e-6);
        EXPECT_NEAR(alpha.alpha[50 * 100 + 50], 0.5, 1e-6);
    }
}

TEST(LocalAdjustmentEngineTest, ContinuousBrushAdmitsMoreThanThirtyTwoPoints)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "brush", 8));
    auto local = local_adjustment_develop(params, "brush");
    ASSERT_TRUE(local);
    std::vector<LocalMaskPoint> points;
    for (int index = 0; index < 65; ++index)
        points.push_back({0.1 + 0.8 * index / 64, 0.5});
    ASSERT_TRUE(author_local_mask_gesture(local.value(), points, "draw", 1));
    ASSERT_TRUE(set_local_adjustment_develop(params, "brush", local.value()));
    auto recipe = recipe_from_develop({"asset", "photo.png", std::nullopt}, params);
    ASSERT_TRUE(recipe);
    WorkingImage input;
    input.width = input.height = 64;
    input.rgb.resize(64 * 64 * 3, 0.25F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = kInputProfileLinearRec709;
    AlphaPlane alpha;
    ASSERT_TRUE(apply_local_adjustment(input, recipe.value(), params.local_adjustments[0].operation,
                                       {}, &alpha));
    EXPECT_GT(alpha.alpha[32 * 64 + 32], 0.5);
    EXPECT_EQ(alpha.alpha[0], 0);
}

TEST(LocalAdjustmentEngineTest, CompleteChainUsesOneAlphaAndLeavesOutsidePixelsExact)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "local-one", 3));
    auto local = local_adjustment_develop(params, "local-one");
    ASSERT_TRUE(local);
    local.value().exposure_ev = 1;
    local.value().contrast = 0.3;
    ASSERT_TRUE(apply_develop_mask_field_strict(local.value(), "localMaskFeather", 0.2));
    ASSERT_TRUE(set_local_adjustment_develop(params, "local-one", local.value()));
    auto recipe = recipe_from_develop({"asset", "photo.png", std::nullopt}, params);
    ASSERT_TRUE(recipe);
    const auto &group = params.local_adjustments.front().operation;
    WorkingImage input;
    input.width = 32;
    input.height = 24;
    input.rgb.resize(32 * 24 * 3, 0.15F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = kInputProfileLinearRec709;
    Recipe chain;
    chain.operations = group.children;
    auto full = apply_recipe_ops(input, chain, {}, true);
    ASSERT_TRUE(full);
    AlphaPlane alpha;
    auto actual = apply_local_adjustment(input, recipe.value(), group, {}, &alpha);
    ASSERT_TRUE(actual);
    bool outside = false, feather = false;
    for (std::size_t pixel = 0; pixel < alpha.alpha.size(); ++pixel)
    {
        const float a = alpha.alpha[pixel];
        outside = outside || a == 0;
        feather = feather || (a > 0 && a < 1);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const auto index = pixel * 3 + channel;
            const float expected =
                a == 0 ? input.rgb[index] :
                a == 1 ? full.value().rgb[index] :
                         input.rgb[index] + a * (full.value().rgb[index] - input.rgb[index]);
            EXPECT_FLOAT_EQ(actual.value().rgb[index], expected);
        }
    }
    EXPECT_TRUE(outside);
    EXPECT_TRUE(feather);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    EXPECT_FALSE(apply_local_adjustment(input, recipe.value(), group, cancellation.token()));
}

TEST(LocalAdjustmentEngineTest, GeometryUsesPixelCentersAndReversesRotationCropCanvasPerspective)
{
    DevelopParams params;
    params.rotate_quarters = 1;
    params.crop_x = 0.1;
    params.crop_y = 0.05;
    params.crop_width = 0.8;
    params.crop_height = 0.8;
    auto mapping = prepare_mask_geometry(params, 400, 200);
    ASSERT_TRUE(mapping);
    auto displayed = map_mask_point(mapping.value(), {0.2, 0.3}, false);
    ASSERT_TRUE(displayed);
    EXPECT_NEAR(displayed.value().x, 0.75, 1e-12);
    EXPECT_NEAR(displayed.value().y, 0.1875, 1e-12);
    params.canvas_enabled = true;
    params.canvas.percent_left = 12;
    params.canvas.percent_top = 8;
    params.perspective_vertical = 0.1;
    params.straighten_degrees = 7;
    mapping = prepare_mask_geometry(params, 400, 200);
    ASSERT_TRUE(mapping);
    for (const MaskPoint original : {MaskPoint{0.2, 0.3}, MaskPoint{0.5, 0.5}, MaskPoint{0.7, 0.6}})
    {
        displayed = map_mask_point(mapping.value(), original, false);
        ASSERT_TRUE(displayed);
        auto restored = map_mask_point(mapping.value(), displayed.value(), true);
        ASSERT_TRUE(restored);
        EXPECT_NEAR(restored.value().x, original.x, 1e-12);
        EXPECT_NEAR(restored.value().y, original.y, 1e-12);
    }
    EXPECT_FALSE(map_mask_point(mapping.value(), {-100, -100}, true));
}

TEST(LocalAdjustmentEngineTest, PromotedLegacyMaskKeepsPixelsAndGlobalExposureCanBeAdded)
{
    DevelopParams params;
    params.exposure_ev = 0.7;
    params.highlights = -0.3;
    ASSERT_TRUE(apply_develop_mask_field_strict(params, "exposureMaskKind", 4));
    auto before = recipe_from_develop({"asset", "photo.png", std::nullopt}, params);
    ASSERT_TRUE(before);
    ASSERT_TRUE(promote_legacy_local_adjustments(params));
    auto after = recipe_from_develop(before.value().asset, params);
    ASSERT_TRUE(after);
    WorkingImage input;
    input.width = input.height = 16;
    input.rgb.resize(16 * 16 * 3, 0.25F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = kInputProfileLinearRec709;
    auto old_pixels = apply_recipe_ops(input, before.value(), {});
    auto new_pixels = apply_recipe_ops(input, after.value(), {});
    ASSERT_TRUE(old_pixels);
    ASSERT_TRUE(new_pixels);
    EXPECT_EQ(new_pixels.value().rgb, old_pixels.value().rgb);
    params.exposure_ev = 0.2;
    after = recipe_from_develop(before.value().asset, params);
    ASSERT_TRUE(after);
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry);
    ASSERT_TRUE(validate_recipe(after.value(), registry.value()));
}
} // namespace ravo
