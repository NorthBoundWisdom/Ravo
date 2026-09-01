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

TEST(ColorHarmonizerTest, Real0176StatesMatchIndependentSourceOrderReferences)
{
    const auto tables = frozen_harmony_tables();
    const WorkingImage input = color_harmonizer_working_fixture();
    const WorkingImage original = input;
    const std::array cases{ColorHarmonizerParams{}, frozen_color_harmonizer_0176_record13()};
    const std::array<std::array<std::array<std::uint32_t, 3>, 4>, 2> goldens{{
        {{{1022738880U, 1043878391U, 1060655590U},
          {1063843274U, 1054280255U, 1032805390U},
          {3039821824U, 1056964611U, 1071225240U},
          {0U, 0U, 0U}}},
        {{{3202216542U, 1051481554U, 1057828450U},
          {1066933552U, 1051086807U, 1044202570U},
          {3212784549U, 1062758668U, 1067310102U},
          {0U, 0U, 0U}}},
    }};
    for (std::size_t case_index = 0U; case_index < cases.size(); ++case_index)
    {
        auto actual = apply_color_harmonizer(input, cases[case_index], CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        ASSERT_EQ(actual.value().rgb.size(), input.rgb.size());
        for (std::size_t pixel = 0U; pixel < input.rgb.size() / 3U; ++pixel)
        {
            const std::size_t index = pixel * 3U;
            const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                          input.rgb[index + 2U]};
            const auto oracle = frozen_color_harmonizer_rgb(
                cases[case_index], source, input.color_profile.matrix_to_xyz_d50, tables);
            const FrozenD50Triplet produced{actual.value().rgb[index],
                                            actual.value().rgb[index + 1U],
                                            actual.value().rgb[index + 2U]};
            EXPECT_EQ(d50_triplet_bits(produced), d50_triplet_bits(oracle));
            for (std::size_t channel = 0U; channel < produced.size(); ++channel)
            {
                if (goldens[case_index][pixel][channel] == 0U)
                {
                    EXPECT_EQ(std::bit_cast<std::uint32_t>(produced[channel]),
                              goldens[case_index][pixel][channel]);
                }
                else
                {
                    EXPECT_NEAR(produced[channel],
                                std::bit_cast<float>(goldens[case_index][pixel][channel]),
                                kPlatformLibmReferenceTolerance);
                }
            }
        }
        EXPECT_EQ(actual.value().color_profile, input.color_profile);
        EXPECT_EQ(actual.value().exposure_analysis, input.exposure_analysis);
        EXPECT_NE(actual.value().rgb.data(), input.rgb.data());
        EXPECT_NE(actual.value().color_profile.icc_bytes.data(),
                  input.color_profile.icc_bytes.data());
        actual.value().rgb.front() = 99.0F;
        actual.value().color_profile.icc_bytes.front() = 99U;
        EXPECT_EQ(input.rgb, original.rgb);
        EXPECT_EQ(input.color_profile, original.color_profile);
    }
    const FrozenD50Triplet clipped_input{-0.25F, 0.5F, 1.7F};
    const auto clipped = frozen_color_harmonizer_rgb(cases.back(), clipped_input,
                                                     input.color_profile.matrix_to_xyz_d50, tables);
    const auto no_clip = frozen_color_harmonizer_rgb(
        cases.back(), clipped_input, input.color_profile.matrix_to_xyz_d50, tables, true);
    EXPECT_NE(d50_triplet_bits(no_clip), d50_triplet_bits(clipped))
        << "the extended fixture must detect omission of frozen fmaxf clipping";
    const FrozenD50Triplet neutral_input{0.03F, 0.18F, 0.72F};
    const auto cubic_neutral = frozen_color_harmonizer_rgb(
        cases.back(), neutral_input, input.color_profile.matrix_to_xyz_d50, tables);
    const auto linear_neutral = frozen_color_harmonizer_rgb(
        cases.back(), neutral_input, input.color_profile.matrix_to_xyz_d50, tables, false, true);
    EXPECT_NE(d50_triplet_bits(linear_neutral), d50_triplet_bits(cubic_neutral))
        << "the fixture must detect changing the frozen cubic neutral protection";
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorHarmonizerTest, EveryPredefinedRuleAndCustomNodeCountUseCanonicalDispatch)
{
    const auto tables = frozen_harmony_tables();
    const auto production_tables = harmony_geometry::build_harmony_hue_tables();
    auto input = color_harmonizer_working_fixture();
    input.width = 1U;
    input.rgb.resize(3U);
    for (std::size_t rule_index = 0U; rule_index <= 9U; ++rule_index)
    {
        ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
        params.rule = static_cast<ColorHarmonizerRule>(rule_index);
        if (params.rule == ColorHarmonizerRule::kCustom)
        {
            params.custom_hue = {0.03, 0.29, 0.61, 0.87};
        }
        const std::array node_counts = params.rule == ColorHarmonizerRule::kCustom ?
                                           std::array<std::int64_t, 3>{2, 3, 4} :
                                           std::array<std::int64_t, 3>{4, 4, 4};
        const std::size_t count = params.rule == ColorHarmonizerRule::kCustom ? 3U : 1U;
        for (std::size_t node_case = 0U; node_case < count; ++node_case)
        {
            params.num_custom_nodes = node_counts[node_case];
            const auto oracle_nodes = frozen_color_harmonizer_nodes(params, tables);
            std::array<float, 4> production_nodes{};
            std::size_t production_count = 0U;
            if (params.rule == ColorHarmonizerRule::kCustom)
            {
                production_count = static_cast<std::size_t>(params.num_custom_nodes);
                for (std::size_t index = 0U; index < production_count; ++index)
                {
                    production_nodes[index] = static_cast<float>(params.custom_hue[index]);
                }
            }
            else
            {
                const auto nodes = harmony_geometry::predefined_harmony_nodes(
                    static_cast<harmony_geometry::StandardRule>(params.rule),
                    static_cast<float>(params.anchor_hue), production_tables);
                ASSERT_TRUE(nodes) << nodes.error().message;
                production_nodes = nodes.value().hues;
                production_count = nodes.value().count;
            }
            EXPECT_EQ(production_count, oracle_nodes.count);
            for (std::size_t index = 0U; index < production_count; ++index)
            {
                EXPECT_EQ(std::bit_cast<std::uint32_t>(production_nodes[index]),
                          std::bit_cast<std::uint32_t>(oracle_nodes.hues[index]));
            }
            const FrozenD50Triplet clipped{std::fmax(input.rgb[0], 0.0F),
                                           std::fmax(input.rgb[1], 0.0F),
                                           std::fmax(input.rgb[2], 0.0F)};
            const auto xyz =
                frozen_color_harmonizer_matrix(input.color_profile.matrix_to_xyz_d50, clipped);
            const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
            const auto production_jch = dt_ucs::xyz_d50_to_jch(xyz, white_lightness);
            const auto oracle_jch = frozen_dt_ucs_xyz_d50_to_jch(xyz, white_lightness);
            EXPECT_EQ(d50_triplet_bits(production_jch), d50_triplet_bits(oracle_jch));
            constexpr float pi = 3.14159265358979323846F;
            constexpr float two_pi = 6.28318530717958647693F;
            const float hue = (oracle_jch[2] + pi) / two_pi;
            const auto production_attraction = harmony_geometry::harmony_attraction(
                hue, std::span<const float>(production_nodes.data(), production_count),
                static_cast<float>(params.pull_width));
            ASSERT_TRUE(production_attraction) << production_attraction.error().message;
            const auto oracle_attraction = frozen_harmony_attraction(
                hue, std::span<const float>(oracle_nodes.hues.data(), oracle_nodes.count),
                static_cast<float>(params.pull_width));
            EXPECT_EQ(production_attraction.value().winning_index, oracle_attraction.winning_index);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(production_attraction.value().weight),
                      std::bit_cast<std::uint32_t>(oracle_attraction.weight));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(production_attraction.value().shift),
                      std::bit_cast<std::uint32_t>(oracle_attraction.shift));
            const float neutral = static_cast<float>(params.neutral_protection);
            const float neutral_squared = neutral * neutral;
            const float neutral_cubed = neutral_squared * neutral;
            const float cutoff = neutral_cubed * 0.03F;
            const float denominator = (production_jch[1] + cutoff) + 1.0e-5F;
            const float chroma_weight = production_jch[1] / denominator;
            const float saturation_delta =
                (static_cast<float>(
                     params.node_saturation[production_attraction.value().winning_index]) -
                 1.0F) *
                production_attraction.value().weight;
            auto corrected_jch = production_jch;
            float corrected_hue =
                std::fmod(hue + production_attraction.value().shift *
                                    static_cast<float>(params.pull_strength) * chroma_weight,
                          1.0F);
            if (corrected_hue < 0.0F)
            {
                corrected_hue += 1.0F;
            }
            corrected_jch[2] = corrected_hue * two_pi - pi;
            corrected_jch[1] =
                std::fmax(production_jch[1] * (1.0F + saturation_delta * chroma_weight), 0.0F);
            const auto production_xyz = dt_ucs::jch_to_xyz_d50(corrected_jch, white_lightness);
            const auto oracle_xyz = frozen_dt_ucs_jch_to_xyz_d50(corrected_jch, white_lightness);
            EXPECT_EQ(d50_triplet_bits(production_xyz), d50_triplet_bits(oracle_xyz));
            const auto direct = apply_color_harmonizer(input, params, CancellationToken{});
            ASSERT_TRUE(direct) << direct.error().message;
            const auto oracle =
                frozen_color_harmonizer_rgb(params, {input.rgb[0], input.rgb[1], input.rgb[2]},
                                            input.color_profile.matrix_to_xyz_d50, tables);
            EXPECT_EQ(d50_triplet_bits(
                          {direct.value().rgb[0], direct.value().rgb[1], direct.value().rgb[2]}),
                      d50_triplet_bits(oracle));
            const auto parameters = color_harmonizer_to_parameters(params);
            ASSERT_TRUE(parameters) << parameters.error().message;
            Recipe recipe;
            recipe.operations.push_back(
                {std::string(kColorHarmonizerOperationId), kColorHarmonizerOperationSchemaVersion,
                 "colorharmonizer-dispatch", true, parameters.value(), std::nullopt});
            const auto dispatched = apply_recipe_ops(input, recipe, CancellationToken{});
            ASSERT_TRUE(dispatched) << dispatched.error().message;
            EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
        }
    }
}

TEST(ColorHarmonizerTest, InvalidStatesAndUnknownScaleFailAtomicallyBeforePublication)
{
    WorkingImage input = color_harmonizer_working_fixture();
    const WorkingImage original = input;
    ColorHarmonizerParams params = frozen_color_harmonizer_0176_record13();
    params.smoothing = 0.01;
    auto rejected = apply_color_harmonizer(input, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_roi_scale");
    EXPECT_EQ(input.rgb, original.rgb);
    input.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(4U, 1U, 4U, 1U);
    ASSERT_TRUE(input.canonical_roi_scale.valid());
    const auto smoothed = apply_color_harmonizer(input, params, CancellationToken{});
    ASSERT_TRUE(smoothed) << smoothed.error().message;
    EXPECT_EQ(smoothed.value().canonical_roi_scale.value(), input.canonical_roi_scale.value());

    params.smoothing = 0.0;
    auto parameters = color_harmonizer_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    OperationInstance operation{std::string(kColorHarmonizerOperationId),
                                kColorHarmonizerOperationSchemaVersion,
                                "colorharmonizer-mask",
                                true,
                                parameters.value(),
                                "mask-1"};
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorharmonizer_mask_graph_unavailable");
    operation.mask_id.reset();
    operation.schema_version += 1;
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    operation.schema_version = kColorHarmonizerOperationSchemaVersion;
    operation.id = "ravo.color.colorcontrast";
    rejected = apply_color_harmonizer(input, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    const auto expect_reason = [&](WorkingImage invalid, const std::string_view reason)
    {
        const auto result = apply_color_harmonizer(invalid, params, CancellationToken{});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().context.at("reason"), reason);
    };
    auto invalid = input;
    invalid.width = 0U;
    expect_reason(invalid, "invalid_colorharmonizer_dimensions");
    invalid = input;
    invalid.rgb.pop_back();
    expect_reason(invalid, "invalid_colorharmonizer_buffer");
    invalid = input;
    invalid.width = std::numeric_limits<std::uint32_t>::max();
    invalid.height = std::numeric_limits<std::uint32_t>::max();
    invalid.rgb.clear();
    expect_reason(invalid, "invalid_colorharmonizer_buffer");
    invalid = input;
    invalid.color_profile.kind = ColorProfileKind::kMissing;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.model = ColorModel::kLab;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.has_matrix = false;
    expect_reason(invalid, "unsupported_colorharmonizer_working_space");
    invalid = input;
    invalid.color_profile.matrix_to_xyz_d50[0] = std::numeric_limits<float>::quiet_NaN();
    expect_reason(invalid, "invalid_colorharmonizer_profile_matrix");
    invalid = input;
    invalid.color_profile.matrix_to_xyz_d50.fill(0.0F);
    expect_reason(invalid, "invalid_colorharmonizer_profile_matrix");
    for (const float sample :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        invalid = input;
        invalid.rgb[1] = sample;
        expect_reason(invalid, "nonfinite_colorharmonizer_input");
    }
    invalid = input;
    invalid.rgb = {std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max(),
                   0.91F,
                   0.42F,
                   0.07F,
                   -0.25F,
                   0.5F,
                   1.7F,
                   0.0F,
                   0.0F,
                   0.0F};
    rejected = apply_color_harmonizer(invalid, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorharmonizer_geometry");

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("colorharmonizer-pre"));
    rejected = apply_color_harmonizer(input, params, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

} // namespace
} // namespace ravo
