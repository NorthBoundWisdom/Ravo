#include <gtest/gtest.h>

#include "ravo/engine/engine.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/dehaze.h"

#include "dehaze.h"
#include "engine_test_support.h"
#include "image_ops.h"

namespace ravo
{
namespace
{

TEST(RasterDehazeTest, DeclaredProfileIsLinearizedBeforeDehazeAndExportAppliesItOnce)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto raster = engine_test_support::gradient_raster();
    raster.source_width = 160U;
    raster.source_height = 160U;
    for (const auto working_profile : {kInputProfileLinearRec709, kInputProfileLinearRec2020})
    {
        InputColorParams input;
        input.working_profile = working_profile;
        Recipe baseline;
        baseline.asset = {"raster", "memory:raster", std::nullopt};
        baseline.operations.push_back(
            {"ravo.color.input", 1, "input", true, input_color_to_parameters(input), std::nullopt});
        baseline.operations.push_back({"ravo.color.output", 1, "output", true,
                                       output_color_to_parameters(OutputColorParams{}),
                                       std::nullopt});
        auto linear = engine.value().linear_working_from_raster(raster, baseline, {});
        ASSERT_TRUE(linear) << linear.error().message;
        EXPECT_NEAR(linear.value().canonical_roi_scale.value(), 0.1F, 1e-6F);
        const DehazeParams params{0.4, 0.2, true};
        auto expected = apply_dehaze(linear.value(), params, {});
        ASSERT_TRUE(expected) << expected.error().message;
        EXPECT_NE(expected.value().rgb, linear.value().rgb);

        Recipe recipe = baseline;
        auto parameters = dehaze_to_parameters(params);
        ASSERT_TRUE(parameters);
        recipe.operations.push_back({std::string(kDehazeOperationId), kDehazeOperationSchemaVersion,
                                     "dehaze", true, parameters.value(), std::nullopt});
        auto actual = engine.value().linear_working_from_raster(raster, recipe, {});
        ASSERT_TRUE(actual) << actual.error().message;
        EXPECT_EQ(actual.value().rgb, expected.value().rgb);
        EXPECT_EQ(actual.value().color_profile.identifier, working_profile);

        // A subsequent light adjustment must remain active, while source-stage
        // dehaze must not run a second time in ordinary recipe dispatch.
        baseline.operations.push_back({"ravo.core.exposure",
                                       1,
                                       "exposure",
                                       true,
                                       {{"exposure_ev", ParameterValue{0.25}}},
                                       std::nullopt});
        recipe.operations.push_back(baseline.operations.back());
        auto expected_export = engine.value().render_linear_working_export(
            expected.value(), baseline, RenderSampleKind::kRgb8, {});
        ASSERT_TRUE(expected_export) << expected_export.error().message;
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        auto exported =
            engine.value().render_to_export_image(request, RenderSampleKind::kRgb8, &raster);
        ASSERT_TRUE(exported) << exported.error().message;
        EXPECT_EQ(exported.value().samples, expected_export.value().samples);
    }
}

TEST(RasterDehazeTest, MissingProfileNonlinearDispatchAndCancellationRemainExplicit)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto raster = engine_test_support::gradient_raster();
    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    engine_test_support::declare_input(recipe);
    auto parameters = dehaze_to_parameters({0.4, 0.2, true});
    ASSERT_TRUE(parameters);
    recipe.operations.push_back({std::string(kDehazeOperationId), kDehazeOperationSchemaVersion,
                                 "dehaze", true, parameters.value(), std::nullopt});
    const auto original = raster.srgb;
    auto encoded = working_from_encoded_rgb8(raster);
    ASSERT_TRUE(encoded);
    auto nonlinear = apply_dehaze(encoded.value(), recipe.operations.back(), {});
    ASSERT_FALSE(nonlinear);
    EXPECT_EQ(nonlinear.error().context.at("reason"), "unsupported_dehaze_working_space");

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("raster-dehaze"));
    auto rejected = engine.value().linear_working_from_raster(raster, recipe, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    raster.color_profile = {};
    rejected = engine.value().linear_working_from_raster(raster, recipe, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(raster.srgb, original);
}

} // namespace
} // namespace ravo
