#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/engine/iq_consistency.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

namespace ravo
{
namespace
{

[[nodiscard]] LinearWorkingBuffer make_working(const std::uint32_t width,
                                               const std::uint32_t height)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input;
    input.width = width;
    input.height = height;
    input.color_profile = profile;
    input.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    input.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
        input.rgb[index] = static_cast<float>((index % 17U) + 3U) / 32.0F;
    return input;
}

[[nodiscard]] OperationInstance default_sigmoid_operation()
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-1",
            true,
            {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
             {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
             {"middle_grey_contrast", ParameterValue{kSigmoidContrastDefault}},
             {"contrast_skewness", ParameterValue{kSigmoidSkewDefault}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{kSigmoidHuePreservationDefault}}},
            std::nullopt};
}

[[nodiscard]] Recipe make_sigmoid_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    return recipe;
}

[[nodiscard]] const std::vector<std::uint8_t> *
export_rgb8(const RenderedExportImage &image) noexcept
{
    return std::get_if<std::vector<std::uint8_t>>(&image.samples);
}

} // namespace

TEST(IqConsistencyTest, PersistPreviewStaysOnCpuGoldAndIsBitExactOnRerender)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 6);
    const auto recipe = make_sigmoid_recipe();

    const auto first = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(first.value().gpu_backend, "persist_preview"));
    EXPECT_TRUE(is_cpu_gold_backend(first.value().gpu_backend));
    ASSERT_FALSE(first.value().rgb.empty());

    const auto second = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(second.value().gpu_backend, "persist_preview_reopen"));
    EXPECT_EQ(first.value().width, second.value().width);
    EXPECT_EQ(first.value().height, second.value().height);
    EXPECT_TRUE(rgb8_buffers_equal(first.value().rgb, second.value().rgb));
}

TEST(IqConsistencyTest, ExportStaysOnCpuGoldAndMatchesAcrossCalls)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 6);
    const auto recipe = make_sigmoid_recipe();

    const auto first = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    const auto *first_rgb = export_rgb8(first.value());
    ASSERT_NE(first_rgb, nullptr);
    ASSERT_FALSE(first_rgb->empty());

    const auto second = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    const auto *second_rgb = export_rgb8(second.value());
    ASSERT_NE(second_rgb, nullptr);
    EXPECT_EQ(first.value().width, second.value().width);
    EXPECT_EQ(first.value().height, second.value().height);
    EXPECT_EQ(*first_rgb, *second_rgb);
}

TEST(IqConsistencyTest, InteractiveGpuMatchesCpuWithinDocumentedToleranceWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(4, 4);
    const auto recipe = make_sigmoid_recipe();

    const auto cpu = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(cpu.value().gpu_backend, "cpu_reference"));

    InteractivePreviewRenderCache cache;
    const auto interactive = engine.value().render_interactive_linear_working(
        working, recipe, cache, CancellationToken{}, std::nullopt, true);
    ASSERT_TRUE(interactive) << interactive.error().message;
    ASSERT_EQ(interactive.value().rgb.size(), cpu.value().rgb.size());

    if (!interactive.value().gpu_backend.empty())
    {
        EXPECT_EQ(interactive.value().gpu_backend, engine.value().gpu_backend());
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
        {
            const int delta = static_cast<int>(interactive.value().rgb[index]) -
                              static_cast<int>(cpu.value().rgb[index]);
            if (std::abs(delta) > 1)
                ++mismatches;
        }
        EXPECT_LE(mismatches, cpu.value().rgb.size() / 8U);
    }
    else
    {
        EXPECT_TRUE(rgb8_buffers_equal(interactive.value().rgb, cpu.value().rgb));
    }
}

TEST(IqConsistencyTest, RequireCpuGoldRejectsNamedGpuBackend)
{
    auto ok = require_cpu_gold_backend({}, "persist");
    ASSERT_TRUE(ok);
    auto bad = require_cpu_gold_backend("metal", "persist");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().context.at("reason"), "iq_cpu_gold_backend_required");
}

} // namespace ravo
