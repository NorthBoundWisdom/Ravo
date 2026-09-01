#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "engine_harmony_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_harmony_test_support;

TEST(ColorHarmonizerSmoothingTest, MatchesIndependentTwoPassOracleAtFullAndDownscaledRoiScale)
{
    auto input = color_harmonizer_working_fixture();
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.5;
    params.pull_width = 0.25; // freezes the fmaxf(1, pull_width) sigma floor.
    const auto tables = frozen_harmony_tables();
    const auto run = [&](const CanonicalRoiScale scale)
    {
        input.canonical_roi_scale = scale;
        EXPECT_TRUE(input.canonical_roi_scale.valid());
        if (!input.canonical_roi_scale.valid())
        {
            return std::vector<float>{};
        }
        const float sigma = static_cast<float>(params.smoothing) *
                            std::fmax(1.5F, 8.0F * scale.value()) *
                            std::fmax(1.0F, static_cast<float>(params.pull_width));
        const auto expected =
            frozen_color_harmonizer_two_pass(input, params, scale.value(), tables);
        const auto actual = apply_color_harmonizer(input, params, CancellationToken{});
        EXPECT_TRUE(actual) << (actual ? "" : actual.error().message);
        if (!actual)
        {
            return std::vector<float>{};
        }
        EXPECT_FLOAT_EQ(sigma, scale.value() == 1.0F ? 4.0F : 2.0F);
        EXPECT_EQ(actual.value().rgb.size(), expected.size());
        for (std::size_t index = 0U; index < expected.size(); ++index)
        {
            EXPECT_NEAR(actual.value().rgb[index], expected[index], kPlatformLibmReferenceTolerance)
                << index;
        }
        EXPECT_EQ(actual.value().color_profile, input.color_profile);
        EXPECT_EQ(actual.value().exposure_analysis, input.exposure_analysis);
        EXPECT_EQ(actual.value().canonical_roi_scale.value(), scale.value());
        EXPECT_NE(actual.value().rgb.data(), input.rgb.data());
        EXPECT_NE(actual.value().color_profile.icc_bytes.data(),
                  input.color_profile.icc_bytes.data());
        return actual.value().rgb;
    };

    const auto full = run(CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U));
    const auto downscaled = run(CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 8U, 2U));
    EXPECT_NE(full, downscaled);
    EXPECT_EQ(input.rgb, color_harmonizer_working_fixture().rgb);
}

TEST(ColorHarmonizerSmoothingTest, ZeroScaleMetadataDoesNotAffectZeroButPositiveFails)
{
    auto input = color_harmonizer_working_fixture();
    const auto zero_without_scale =
        apply_color_harmonizer(input, ColorHarmonizerParams{}, CancellationToken{});
    ASSERT_TRUE(zero_without_scale) << zero_without_scale.error().message;
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U);
    const auto zero_with_scale =
        apply_color_harmonizer(input, ColorHarmonizerParams{}, CancellationToken{});
    ASSERT_TRUE(zero_with_scale) << zero_with_scale.error().message;
    EXPECT_EQ(zero_without_scale.value().rgb, zero_with_scale.value().rgb);

    input.canonical_roi_scale = {};
    ColorHarmonizerParams positive = frozen_color_harmonizer_0176_record13();
    positive.smoothing = 0.25;
    const auto rejected = apply_color_harmonizer(input, positive, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_roi_scale");
}

TEST(ColorHarmonizerSmoothingTest,
     ControlledCancellationAtMapGaussianApplyAndPublicationNeverPublishes)
{
    auto input = color_harmonizer_working_fixture();
    input.width = 128U;
    input.height = 2U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(128U, 2U, 128U, 2U);
    const auto original = input;
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.25;
    const std::array checkpoints{detail::ColorHarmonizerCheckpoint::kBeforeValidation,
                                 detail::ColorHarmonizerCheckpoint::kMapChunk,
                                 detail::ColorHarmonizerCheckpoint::kGaussian,
                                 detail::ColorHarmonizerCheckpoint::kApplyChunk,
                                 detail::ColorHarmonizerCheckpoint::kBeforePublication};
    for (const auto checkpoint : checkpoints)
    {
        CancellationSource cancellation;
        ColorHarmonizerCancellationFixture fixture{&cancellation, checkpoint};
        const auto rejected = detail::apply_color_harmonizer_controlled(
            input, params, cancellation.token(), {&fixture, cancel_color_harmonizer});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(fixture.fired);
        EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(input.rgb, original.rgb);
        EXPECT_EQ(input.color_profile, original.color_profile);
        EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
        EXPECT_EQ(input.canonical_roi_scale.value(), original.canonical_roi_scale.value());
    }
}

TEST(ColorHarmonizerSmoothingTest, RawMemoryEstimateUsesS2Point2OwnerAndSaturates)
{
    DecodedRaw raw;
    raw.width = 8U;
    raw.height = 4U;
    raw.pixels.assign(32U, 0U);
    Recipe recipe;
    const auto baseline = estimate_raw_render_memory(raw, recipe, 8U, 4U);
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.5;
    auto serialized = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(serialized) << serialized.error().message;
    recipe.operations.push_back({std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion, "smoothing", true,
                                 std::move(serialized).value(), std::nullopt});
    const auto estimated = estimate_raw_render_memory(raw, recipe, 8U, 4U);
    const std::uint64_t expected =
        8U * 4U * 3U * sizeof(float) + detail::recursive_gaussian_zero_2c_bytes(8U, 4U);
    EXPECT_EQ(estimated - baseline, expected);
    EXPECT_EQ(estimate_raw_render_memory(raw, recipe, std::numeric_limits<std::uint32_t>::max(),
                                         std::numeric_limits<std::uint32_t>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(ColorHarmonizerTest, RowCancellationAndDisabledCopyPreserveSourceOwnership)
{
    auto input = color_harmonizer_working_fixture();
    input.width = 1024U;
    input.height = 4096U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    const float first = input.rgb.front();
    const float last = input.rgb.back();
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.rule = ColorHarmonizerRule::kCustom;
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    const auto cancelled = apply_color_harmonizer(input, params, deadline.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(input.rgb.front(), first);
    EXPECT_FLOAT_EQ(input.rgb.back(), last);

    const auto parameters = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance disabled{std::string(kColorHarmonizerOperationId),
                               kColorHarmonizerOperationSchemaVersion,
                               "colorharmonizer-disabled",
                               false,
                               parameters.value(),
                               std::nullopt};
    auto small = color_harmonizer_working_fixture();
    const auto copied = apply_color_harmonizer(small, disabled, CancellationToken{});
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(copied.value().rgb, small.rgb);
    EXPECT_NE(copied.value().rgb.data(), small.rgb.data());
    EXPECT_EQ(copied.value().color_profile, small.color_profile);
    EXPECT_NE(copied.value().color_profile.icc_bytes.data(), small.color_profile.icc_bytes.data());
}

} // namespace
} // namespace ravo
