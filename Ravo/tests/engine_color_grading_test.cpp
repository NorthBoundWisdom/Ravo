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
#include "engine_color_test_support.h"
#include "engine_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_test_support;
using namespace engine_color_test_support;

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

TEST(ColorContrastTest, LabAffineBranchesMatchFrozenSourceBitGoldens)
{
    struct Case
    {
        ColorContrastParams params;
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> golden;
    };
    const std::array cases{
        Case{{2.5999999046325684, 0.0, 2.5, 0.0, true},
             {50.0F, -60.0F, 70.0F},
             {0x42480000U, 0xc31c0000U, 0x432f0000U}},
        Case{{1.25, -12.5, 0.5, 7.25, true},
             {37.75F, -8.125F, 2.25F},
             {0x42170000U, 0xc1b54000U, 0x41060000U}},
        Case{{5.0, 128.0, 0.0, -128.0, false},
             {-20.0F, 20.0F, -20.0F},
             {0xc1a00000U, 0x43000000U, 0xc3000000U}},
    };
    for (const auto &[params, input, golden] : cases)
    {
        const auto oracle = frozen_color_contrast_lab(params, input);
        EXPECT_EQ(d50_triplet_bits(oracle), golden);
        const auto actual = apply_color_contrast_lab(params, input, CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        EXPECT_EQ(d50_triplet_bits(actual.value()), golden);
        EXPECT_EQ(actual.value()[0], input[0]);
    }

    auto perturbed = cases.front().params;
    std::swap(perturbed.a_steepness, perturbed.b_steepness);
    EXPECT_NE(d50_triplet_bits(frozen_color_contrast_lab(perturbed, cases.front().input)),
              cases.front().golden)
        << "the independent oracle must detect an a*/b* coefficient swap";

    auto invalid_params = cases.front().params;
    invalid_params.a_offset = std::numeric_limits<double>::quiet_NaN();
    const auto invalid =
        apply_color_contrast_lab(invalid_params, cases.front().input, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_colorcontrast_parameters");
    const auto nonfinite = apply_color_contrast_lab(
        cases.front().params, {50.0F, std::numeric_limits<float>::infinity(), 0.0F},
        CancellationToken{});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_colorcontrast_lab_input");
}

TEST(ColorContrastTest, ExplicitCanonicalDefaultRetainsTheFrozenD50LabRoundTrip)
{
    const WorkingImage input = legacy_color_balance_working_fixture();
    const WorkingImage original = input;
    const ColorContrastParams defaults;
    const auto actual = apply_color_contrast(input, defaults, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    ASSERT_EQ(actual.value().rgb.size(), input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                      input.rgb[index + 2U]};
        const auto expected = frozen_color_contrast_rgb(defaults, source);
        EXPECT_EQ(d50_triplet_bits({actual.value().rgb[index], actual.value().rgb[index + 1U],
                                    actual.value().rgb[index + 2U]}),
                  d50_triplet_bits(expected));
    }
    EXPECT_NE(actual.value().rgb, input.rgb)
        << "only an absent or upgraded v1-zero operation may skip the frozen Lab round-trip";
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorContrastTest, WorkingDispatchOwnershipFailuresAndCancellationAreAtomic)
{
    const WorkingImage input = legacy_color_balance_working_fixture();
    const WorkingImage original = input;
    const ColorContrastParams params{2.5999999046325684, 0.0, 2.5, 0.0, true};
    auto direct = apply_color_contrast(input, params, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    ASSERT_EQ(direct.value().rgb.size(), input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const FrozenD50Triplet source{input.rgb[index], input.rgb[index + 1U],
                                      input.rgb[index + 2U]};
        const auto expected = frozen_color_contrast_rgb(params, source);
        EXPECT_EQ(d50_triplet_bits({direct.value().rgb[index], direct.value().rgb[index + 1U],
                                    direct.value().rgb[index + 2U]}),
                  d50_triplet_bits(expected));
    }
    EXPECT_EQ(direct.value().color_profile, input.color_profile);
    EXPECT_EQ(direct.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(direct.value().rgb.data(), input.rgb.data());
    EXPECT_NE(direct.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    const auto parameters = color_contrast_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe canonical;
    canonical.operations.push_back({std::string(kColorContrastOperationId),
                                    kColorContrastOperationSchemaVersion, "colorcontrast-v2", true,
                                    parameters.value(), std::nullopt});
    const auto dispatched = apply_recipe_ops(input, canonical, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
    direct.value().rgb.front() = 42.0F;
    direct.value().color_profile.icc_bytes.front() = 99U;
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);

    Recipe compatible;
    compatible.operations.push_back({std::string(kColorContrastOperationId),
                                     1,
                                     "colorcontrast-v1",
                                     true,
                                     {{"amount", ParameterValue{0.25}}},
                                     std::nullopt});
    const auto v1 = apply_recipe_ops(input, compatible, CancellationToken{});
    const auto v1_direct = apply_color_contrast(
        input, ColorContrastParams{1.25, 0.0, 1.25, 0.0, true}, CancellationToken{});
    ASSERT_TRUE(v1) << v1.error().message;
    ASSERT_TRUE(v1_direct) << v1_direct.error().message;
    EXPECT_EQ(v1.value().rgb, v1_direct.value().rgb);
    compatible.operations.front().parameters["amount"] = ParameterValue{0.0};
    const auto v1_zero = apply_recipe_ops(input, compatible, CancellationToken{});
    ASSERT_TRUE(v1_zero) << v1_zero.error().message;
    EXPECT_EQ(v1_zero.value().rgb, input.rgb);

    OperationInstance masked{std::string(kColorContrastOperationId),
                             kColorContrastOperationSchemaVersion,
                             "colorcontrast-mask",
                             true,
                             parameters.value(),
                             "mask-1"};
    auto rejected = apply_color_contrast(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorcontrast_mask_graph_unavailable");
    auto future = masked;
    future.mask_id.reset();
    future.schema_version = kColorContrastOperationSchemaVersion + 1;
    rejected = apply_color_contrast(input, future, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    auto wrong_operation = masked;
    wrong_operation.mask_id.reset();
    wrong_operation.id = "ravo.color.colorcorrection";
    rejected = apply_color_contrast(input, wrong_operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    auto invalid_dimensions = input;
    invalid_dimensions.width = 0U;
    rejected = apply_color_contrast(invalid_dimensions, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcontrast_dimensions");
    auto invalid_buffer = input;
    invalid_buffer.rgb.pop_back();
    rejected = apply_color_contrast(invalid_buffer, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorcontrast_buffer");
    auto invalid_profile = input;
    invalid_profile.color_profile.identifier = "srgb";
    rejected = apply_color_contrast(invalid_profile, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcontrast_working_space");
    auto invalid_model = input;
    invalid_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_contrast(invalid_model, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorcontrast_working_space");
    auto invalid_sample = input;
    invalid_sample.rgb[2] = std::numeric_limits<float>::quiet_NaN();
    const auto invalid_source = invalid_sample.rgb;
    rejected = apply_color_contrast(invalid_sample, params, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorcontrast_input");
    for (std::size_t index = 0U; index < invalid_source.size(); ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(invalid_sample.rgb[index]),
                  std::bit_cast<std::uint32_t>(invalid_source[index]));
    }
    auto overflowing = params;
    overflowing.a_offset = static_cast<double>(std::numeric_limits<float>::max());
    rejected = apply_color_contrast(input, overflowing, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorcontrast_output");
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("colorcontrast-pre"));
    rejected = apply_color_contrast(input, params, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);

    WorkingImage rows;
    rows.width = 1024U;
    rows.height = 4096U;
    rows.rgb.assign(static_cast<std::size_t>(rows.width) * rows.height * 3U, 0.25F);
    rows.color_profile.model = ColorModel::kRgb;
    rows.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    rejected = apply_color_contrast(rows, params, deadline.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(rows.rgb.front(), 0.25F);
    EXPECT_FLOAT_EQ(rows.rgb.back(), 0.25F);
}

TEST(ColorCheckerTest, RgbAndD50LabBridgeMatchesTheFrozenScalarReference)
{
    const auto reference = [](const std::array<float, 3> &rgb)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 216.0F / 24389.0F;
        constexpr float kappa = 24389.0F / 27.0F;
        const std::array<float, 3> xyz{
            0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
        std::array<float, 3> f{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float normalized = xyz[channel] / d50[channel];
            f[channel] = normalized > epsilon ? std::cbrt(normalized) :
                                                (kappa * normalized + 16.0F) / 116.0F;
        }
        const std::array<float, 3> lab{116.0F * f[1] - 16.0F, 500.0F * (f[0] - f[1]),
                                       200.0F * (f[1] - f[2])};
        const float fy = (lab[0] + 16.0F) / 116.0F;
        const std::array<float, 3> inverse_f{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
        std::array<float, 3> roundtrip_xyz{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float value = inverse_f[channel] > 0.20689655172413796F ?
                                    inverse_f[channel] * inverse_f[channel] * inverse_f[channel] :
                                    (116.0F * inverse_f[channel] - 16.0F) / kappa;
            roundtrip_xyz[channel] = d50[channel] * value;
        }
        return std::array<float, 3>{3.1338561F * roundtrip_xyz[0] - 1.6168667F * roundtrip_xyz[1] -
                                        0.4906146F * roundtrip_xyz[2],
                                    -0.9787684F * roundtrip_xyz[0] + 1.9161415F * roundtrip_xyz[1] +
                                        0.0334540F * roundtrip_xyz[2],
                                    0.0719453F * roundtrip_xyz[0] - 0.2289914F * roundtrip_xyz[1] +
                                        1.4052427F * roundtrip_xyz[2]};
    };
    WorkingImage input;
    input.width = 1U;
    input.height = 1U;
    input.rgb = {0.25F, 0.5F, 0.75F};
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto expected = reference({input.rgb[0], input.rgb[1], input.rgb[2]});
    ColorCheckerParams no_patches{{}};

    const auto actual = apply_color_checker(input, no_patches, CancellationToken{});

    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value().rgb[channel], expected[channel], 1.0e-6F) << channel;
        EXPECT_NEAR(actual.value().rgb[channel], input.rgb[channel], 2.0e-6F) << channel;
    }
}

TEST(ColorCheckerTest, DeadlineCancellationDuringRowsNeverMutatesTheSource)
{
    WorkingImage input;
    input.width = 1024U;
    input.height = 4096U;
    input.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.25F);
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    const auto first = input.rgb.front();
    const auto last = input.rgb.back();
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});

    const auto cancelled = apply_color_checker(input, ColorCheckerParams{}, deadline.token());

    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FLOAT_EQ(input.rgb.front(), first);
    EXPECT_FLOAT_EQ(input.rgb.back(), last);
}

TEST(LegacyColorBalanceTest, ModeSpecificNearOneContrastThresholdIsFrozen)
{
    const auto input = legacy_color_balance_working_fixture();
    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        SCOPED_TRACE(mode);
        ColorBalanceParams baseline;
        baseline.mode = std::string(mode);
        auto without_contrast = apply_color_balance(input, baseline, CancellationToken{});
        ASSERT_TRUE(without_contrast) << without_contrast.error().message;

        auto inside = baseline;
        inside.contrast = 1.0 + 0.5e-6;
        auto inside_result = apply_color_balance(input, inside, CancellationToken{});
        ASSERT_TRUE(inside_result) << inside_result.error().message;
        EXPECT_EQ(inside_result.value().rgb, without_contrast.value().rgb);

        auto outside = baseline;
        outside.contrast = 1.0 + 2.0e-6;
        auto outside_result = apply_color_balance(input, outside, CancellationToken{});
        ASSERT_TRUE(outside_result) << outside_result.error().message;
        EXPECT_NE(outside_result.value().rgb, without_contrast.value().rgb);
    }
}

TEST(LegacyColorBalanceTest, BoundaryFailuresCancellationAndMasksNeverPublishPartialPixels)
{
    const auto input = legacy_color_balance_working_fixture();
    const auto original = input;

    WorkingImage zero = input;
    zero.width = 0U;
    auto zero_result = apply_color_balance(zero, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(zero_result.error().code, ErrorCode::kValidation);

    WorkingImage wrong_size = input;
    wrong_size.width = 3U;
    auto size_result = apply_color_balance(wrong_size, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(size_result);
    EXPECT_EQ(size_result.error().code, ErrorCode::kValidation);

    WorkingImage lab = input;
    lab.color_profile.model = ColorModel::kLab;
    auto model_result = apply_color_balance(lab, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(model_result);
    EXPECT_EQ(model_result.error().code, ErrorCode::kUnsupported);

    WorkingImage wrong_profile = input;
    wrong_profile.color_profile.identifier = "linear_rec2020";
    auto profile_result =
        apply_color_balance(wrong_profile, ColorBalanceParams{}, CancellationToken{});
    ASSERT_FALSE(profile_result);
    EXPECT_EQ(profile_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(profile_result.error().context.at("reason"),
              "unsupported_colorbalance_working_space");

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        for (const float sample :
             {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()})
        {
            SCOPED_TRACE(mode);
            WorkingImage invalid = input;
            invalid.rgb[2] = sample;
            ColorBalanceParams params;
            params.mode = std::string(mode);
            auto result = apply_color_balance(invalid, params, CancellationToken{});
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::kValidation);
            EXPECT_EQ(result.error().context.at("reason"), "nonfinite_colorbalance_input");
            EXPECT_EQ(invalid.rgb[0], input.rgb[0]);
            EXPECT_EQ(invalid.rgb[1], input.rgb[1]);
        }
    }

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        ColorBalanceParams invalid_denominator;
        invalid_denominator.mode = std::string(mode);
        invalid_denominator.contrast = 0.0;
        auto denominator = apply_color_balance(input, invalid_denominator, CancellationToken{});
        ASSERT_FALSE(denominator);
        EXPECT_EQ(denominator.error().code, ErrorCode::kValidation);
        EXPECT_EQ(denominator.error().context.at("parameter"), "contrast");
    }

    ColorBalanceParams invalid_power;
    invalid_power.mode = std::string(kColorBalanceModeLiftGammaGain);
    invalid_power.gamma = {0.0, 0.0, 0.0, 0.0};
    WorkingImage superwhite = input;
    superwhite.rgb.assign(superwhite.rgb.size(), 2.0F);
    auto power = apply_color_balance(superwhite, invalid_power, CancellationToken{});
    ASSERT_FALSE(power);
    EXPECT_EQ(power.error().code, ErrorCode::kValidation);
    EXPECT_EQ(power.error().context.at("reason"), "nonfinite_colorbalance_curve");

    ColorBalanceParams sop_power;
    WorkingImage negative = input;
    negative.rgb.assign(negative.rgb.size(), -2.0F);
    auto clipped_power = apply_color_balance(negative, sop_power, CancellationToken{});
    ASSERT_TRUE(clipped_power) << clipped_power.error().message;
    EXPECT_TRUE(std::all_of(clipped_power.value().rgb.begin(), clipped_power.value().rgb.end(),
                            [](const float sample) { return std::isfinite(sample); }));

    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        CancellationSource cancelled;
        ASSERT_TRUE(cancelled.cancel("legacy-colorbalance-pre-cancel"));
        ColorBalanceParams params;
        params.mode = std::string(mode);
        auto pre_cancelled = apply_color_balance(input, params, cancelled.token());
        ASSERT_FALSE(pre_cancelled);
        EXPECT_EQ(pre_cancelled.error().code, ErrorCode::kCancelled);
    }

    WorkingImage large;
    large.width = 1024U;
    large.height = 2048U;
    large.color_profile = input.color_profile;
    large.rgb.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    for (const std::string_view mode :
         {kColorBalanceModeSlopeOffsetPower, kColorBalanceModeLiftGammaGain})
    {
        const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                                std::chrono::milliseconds{1});
        ColorBalanceParams params;
        params.mode = std::string(mode);
        auto row_cancelled = apply_color_balance(large, params, deadline.token());
        ASSERT_FALSE(row_cancelled);
        EXPECT_EQ(row_cancelled.error().code, ErrorCode::kCancelled);
    }
    EXPECT_FLOAT_EQ(large.rgb.front(), 0.5F);
    EXPECT_FLOAT_EQ(large.rgb.back(), 0.5F);

    auto masked = legacy_color_balance_operation(ColorBalanceParams{});
    masked.mask_id = "mask-1";
    auto mask_result = apply_color_balance(input, masked, CancellationToken{});
    ASSERT_FALSE(mask_result);
    EXPECT_EQ(mask_result.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(mask_result.error().context.at("reason"), "colorbalance_mask_graph_unavailable");
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
}

TEST(ColorBalanceRgbTest, FilmlightTransformsAndOpacityMasksMatchTheFrozenMath)
{
    const auto white_ych = color_balance_rgb_working_to_ych(std::array<float, 3>{1.0F, 1.0F, 1.0F});
    EXPECT_NEAR(white_ych[0], 1.0578599F, 2.0e-5F);
    EXPECT_LT(white_ych[1], 5.0e-5F);
    EXPECT_NEAR(std::hypot(white_ych[2], white_ych[3]), 1.0F, 2.0e-5F);
    const auto grading = color_balance_rgb_ych_to_grading_rgb(white_ych);
    for (const float sample : grading)
    {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_GT(sample, 0.0F);
    }

    const ColorBalanceRgbParams params;
    const auto center =
        color_balance_rgb_opacity_masks(static_cast<float>(params.mask_grey_fulcrum), params);
    EXPECT_NEAR(center.opacity[0], 0.5F, 1.0e-6F);
    EXPECT_NEAR(center.opacity[1], 0.5F, 1.0e-6F);
    EXPECT_NEAR(center.opacity[2], 0.5F, 1.0e-6F);
    for (std::size_t index = 0; index < center.opacity.size(); ++index)
    {
        EXPECT_NEAR(center.opacity[index] + center.complement[index], 1.0F, 1.0e-6F);
    }
    const auto dark = color_balance_rgb_opacity_masks(0.001F, params);
    const auto bright = color_balance_rgb_opacity_masks(1.0F, params);
    EXPECT_GT(dark.opacity[0], dark.opacity[2]);
    EXPECT_GT(bright.opacity[2], bright.opacity[0]);
    EXPECT_GT(center.opacity[1], dark.opacity[1]);
    EXPECT_GT(center.opacity[1], bright.opacity[1]);

    bool exercised_negative_lms_clip = false;
    for (int degrees = -180; degrees < 180; degrees += 15)
    {
        auto clipped = color_balance_rgb_jzazbz_negative_lms_clip(
            0.2F, 2.0F, static_cast<float>(degrees) * std::numbers::pi_v<float> / 180.0F);
        ASSERT_TRUE(clipped) << clipped.error().message;
        if (clipped.value().clipped)
        {
            exercised_negative_lms_clip = true;
            EXPECT_LT(clipped.value().chroma, 2.0F);
        }
    }
    EXPECT_TRUE(exercised_negative_lms_clip);
}

TEST(ColorBalanceRgbTest, EveryGradingStageChangesSyntheticPixels)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const RasterBuffer source = gradient_raster();
    Recipe identity_recipe;
    identity_recipe.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(identity_recipe);
    RenderRequest identity_request;
    identity_request.asset = identity_recipe.asset;
    identity_request.recipe = identity_recipe;
    auto identity = engine.value().render_to_image(identity_request, &source);
    ASSERT_TRUE(identity) << identity.error().message;

    const auto exercise = [&](ColorBalanceRgbParams params)
    { return render_op(engine.value(), source, color_balance_rgb_operation(params)); };

    ColorBalanceRgbParams offset;
    offset.global_y = 0.15;
    offset.global_chroma = 0.08;
    offset.global_hue = 35.0;
    auto offset_result = exercise(offset);
    ASSERT_TRUE(offset_result) << offset_result.error().message;
    EXPECT_NE(offset_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams shadows;
    shadows.shadows_y = 0.35;
    shadows.shadows_chroma = 0.1;
    shadows.shadows_hue = 220.0;
    auto shadows_result = exercise(shadows);
    ASSERT_TRUE(shadows_result) << shadows_result.error().message;
    EXPECT_NE(shadows_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams midtones;
    midtones.midtones_y = 0.25;
    midtones.midtones_chroma = 0.08;
    midtones.midtones_hue = 300.0;
    auto midtones_result = exercise(midtones);
    ASSERT_TRUE(midtones_result) << midtones_result.error().message;
    EXPECT_NE(midtones_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams highlights;
    highlights.highlights_y = -0.3;
    highlights.highlights_chroma = 0.08;
    highlights.highlights_hue = 70.0;
    auto highlights_result = exercise(highlights);
    ASSERT_TRUE(highlights_result) << highlights_result.error().message;
    EXPECT_NE(highlights_result.value().rgb, identity.value().rgb);

    ColorBalanceRgbParams perceptual;
    perceptual.chroma_global = 0.2;
    perceptual.saturation_global = 0.25;
    perceptual.brilliance_global = 0.15;
    perceptual.vibrance = 0.2;
    perceptual.hue_rotation = 20.0;
    perceptual.contrast = 0.15;
    auto dt_ucs = exercise(perceptual);
    ASSERT_TRUE(dt_ucs) << dt_ucs.error().message;
    EXPECT_NE(dt_ucs.value().rgb, identity.value().rgb);

    perceptual.saturation_formula = std::string(kColorBalanceRgbFormulaJzAzBz2021);
    auto jzazbz = exercise(perceptual);
    ASSERT_TRUE(jzazbz) << jzazbz.error().message;
    EXPECT_NE(jzazbz.value().rgb, identity.value().rgb);
    EXPECT_NE(jzazbz.value().rgb, dt_ucs.value().rgb);
}

TEST(ColorBalanceRgbTest, CancellationAndNonFiniteInputNeverPublishPartialPixels)
{
    ColorBalanceRgbParams params;
    params.global_y = 0.2;
    const auto operation = color_balance_rgb_operation(params);
    WorkingImage image{2, 1, {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {}, {}, {}, {}};
    const auto original = image.rgb;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("color_balance_cancel"));
    auto cancelled = apply_color_balance_rgb(image, operation, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(image.rgb, original);

    image.rgb[4] = std::numeric_limits<float>::infinity();
    const auto invalid_original = image.rgb;
    auto invalid = apply_color_balance_rgb(image, operation, CancellationToken{});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(image.rgb, invalid_original);
}

TEST(ColorBalanceRgbTest, ParallelRowsAreBitExactAndReportTheLowestInvalidSample)
{
    ColorBalanceRgbParams params;
    params.global_y = 0.2;
    params.chroma_global = 0.12;
    params.hue_rotation = 18.0;
    const auto operation = color_balance_rgb_operation(params);
    WorkingImage source;
    source.width = 64;
    source.height = 64;
    source.rgb.resize(static_cast<std::size_t>(source.width) * source.height * 3U);
    for (std::size_t index = 0; index < source.rgb.size(); ++index)
    {
        source.rgb[index] = static_cast<float>((index * 17U) % 251U) / 250.0F;
    }
    WorkingImage first = source;
    WorkingImage second = source;
    auto first_result = apply_color_balance_rgb(first, operation, CancellationToken{});
    auto second_result = apply_color_balance_rgb(second, operation, CancellationToken{});
    ASSERT_TRUE(first_result) << first_result.error().message;
    ASSERT_TRUE(second_result) << second_result.error().message;
    EXPECT_EQ(first.rgb, second.rgb);

    WorkingImage invalid = source;
    const std::size_t later = (48U * invalid.width + 4U) * 3U + 2U;
    const std::size_t earlier = (2U * invalid.width + 9U) * 3U + 1U;
    invalid.rgb[later] = std::numeric_limits<float>::infinity();
    invalid.rgb[earlier] = std::numeric_limits<float>::quiet_NaN();
    const auto original = invalid.rgb;
    auto rejected = apply_color_balance_rgb(invalid, operation, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("sample_index"), std::to_string(earlier - earlier % 3U));
    ASSERT_EQ(invalid.rgb.size(), original.size());
    for (std::size_t index = 0; index < invalid.rgb.size(); ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(invalid.rgb[index]),
                  std::bit_cast<std::uint32_t>(original[index]));
    }
}

} // namespace
} // namespace ravo
