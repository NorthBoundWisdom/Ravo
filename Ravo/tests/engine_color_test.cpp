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

TEST(LegacyColorBalanceTest, SopAndLggModesPreserveFrozenMathAndOwnedPublication)
{
    const auto input = legacy_color_balance_working_fixture();
    const auto original = input;

    ColorBalanceParams sop;
    sop.lift = {0.96, 1.03, 0.98, 1.06};
    sop.gamma = {1.08, 0.91, 1.05, 0.97};
    sop.gain = {1.04, 1.12, 0.95, 1.08};
    sop.input_saturation = 0.84;
    sop.contrast = 1.16;
    sop.grey_fulcrum_percent = 18.0;
    sop.output_saturation = 1.09;
    auto sop_result = apply_color_balance(input, sop, CancellationToken{});
    ASSERT_TRUE(sop_result) << sop_result.error().message;
    ASSERT_EQ(sop_result.value().rgb.size(), input.rgb.size());
    const std::array<float, 6> expected_sop{0.10232526F, 0.15027370F, 0.66838688F,
                                            0.85773712F, 0.34731370F, 0.18906617F};
    const auto reference_sop = frozen_legacy_color_balance_reference(input, sop);
    for (std::size_t index = 0U; index < expected_sop.size(); ++index)
    {
        EXPECT_NEAR(reference_sop[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], expected_sop[index], 2.0e-5F) << index;
        EXPECT_NEAR(sop_result.value().rgb[index], reference_sop[index], 2.0e-5F) << index;
    }
    auto channel_order_perturbation = sop;
    std::swap(channel_order_perturbation.lift[1], channel_order_perturbation.lift[3]);
    const auto perturbed_reference =
        frozen_legacy_color_balance_reference(input, channel_order_perturbation);
    bool perturbation_detected = false;
    for (std::size_t index = 0U; index < reference_sop.size(); ++index)
    {
        perturbation_detected |=
            std::abs(reference_sop[index] - perturbed_reference[index]) > 1.0e-3F;
    }
    EXPECT_TRUE(perturbation_detected)
        << "the independent oracle must detect a frozen RGB channel-order perturbation";

    ColorBalanceParams lgg = sop;
    lgg.mode = std::string(kColorBalanceModeLiftGammaGain);
    auto lgg_result = apply_color_balance(input, lgg, CancellationToken{});
    ASSERT_TRUE(lgg_result) << lgg_result.error().message;
    const std::array<float, 6> expected_lgg{0.12932241F, 0.17394857F, 0.73170942F,
                                            1.06095791F, 0.34121433F, 0.19952966F};
    const auto reference_lgg = frozen_legacy_color_balance_reference(input, lgg);
    for (std::size_t index = 0U; index < expected_lgg.size(); ++index)
    {
        EXPECT_NEAR(reference_lgg[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], expected_lgg[index], 2.0e-5F) << index;
        EXPECT_NEAR(lgg_result.value().rgb[index], reference_lgg[index], 2.0e-5F) << index;
    }
    EXPECT_NE(lgg_result.value().rgb, sop_result.value().rgb);

    auto defaults = apply_color_balance(input, ColorBalanceParams{}, CancellationToken{});
    ASSERT_TRUE(defaults) << defaults.error().message;
    const auto reference_defaults =
        frozen_legacy_color_balance_reference(input, ColorBalanceParams{});
    // The frozen operation performs its Lab/ProPhoto conversion boundary even at defaults.
    EXPECT_NE(defaults.value().rgb, input.rgb);
    ASSERT_EQ(defaults.value().rgb.size(), reference_defaults.size());
    for (std::size_t index = 0U; index < reference_defaults.size(); ++index)
    {
        EXPECT_NEAR(defaults.value().rgb[index], reference_defaults[index], 2.0e-5F) << index;
    }
    EXPECT_EQ(sop_result.value().width, input.width);
    EXPECT_EQ(sop_result.value().height, input.height);
    EXPECT_EQ(sop_result.value().color_profile, input.color_profile);
    EXPECT_EQ(sop_result.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(sop_result.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(sop_result.value().color_profile.icc_bytes.empty());
    EXPECT_NE(sop_result.value().color_profile.icc_bytes.data(),
              input.color_profile.icc_bytes.data());
    sop_result.value().rgb[0] = 42.0F;
    sop_result.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(ColorCheckerTest, ThinPlateKernelUsesTheFrozenFastLogApproximation)
{
    struct KernelCase
    {
        std::array<float, 3> point;
        float squared_distance;
        std::uint32_t expected_bits;
    };
    const std::array<KernelCase, 4> cases{{
        {{1.0F, 0.0F, 0.0F}, 1.0F, 0xb5ddce9eU},
        {{1.0F, 1.0F, 0.0F}, 2.0F, 0x3fb171fcU},
        {{3.0F, 1.0F, 0.0F}, 10.0F, 0x41b8340aU},
        {{100.0F, 0.0F, 0.0F}, 10000.0F, 0x47b3e369U},
    }};
    for (const auto &[point, squared_distance, expected_bits] : cases)
    {
        const float actual = color_checker_thin_plate_kernel(point, {});
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual), expected_bits) << squared_distance;
    }
}

TEST(ColorCheckerTest, TwoPatchGaussianOrientationMatchesTheFrozenScalarOracle)
{
    ColorCheckerParams params{{
        {{{1.0, 2.0, 4.0}}, {{3.0, 8.0, 20.0}}},
        {{{3.0, 5.0, 9.0}}, {{7.0, 20.0, 45.0}}},
    }};
    const std::array<float, 3> input{2.0F, 3.0F, 6.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const std::array<float, 3> golden{5.0F, 12.0F, 30.0F};
    EXPECT_EQ(oracle, golden);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), golden);
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, ThreePatchOtherChannelSumRoundsInFloatBeforePromotion)
{
    ColorCheckerParams params{{
        {{{-52.407073974609375, 16777224.0, -16777207.0}},
         {{-5.947298526763916, 67.29228973388672, -4.729358196258545}}},
        {{{-5.189292907714844, 16777200.0, -16777184.0}},
         {{27.813627243041992, -69.87671661376953, 26.972131729125977}}},
        {{{-6.1535325050354, 16777212.0, -16777192.0}},
         {{73.60906219482422, 4.636241912841797, 48.250370025634766}}},
    }};
    const std::array<float, 3> input{12.5F, 16777220.0F, -16777216.0F};
    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto promoted = frozen_color_checker_lab_reference(params, input, false, true);
    EXPECT_NE(oracle, promoted)
        << "the independent oracle must detect promotion before the frozen float addition";
    EXPECT_FLOAT_EQ(oracle[1], 48.0F);
    EXPECT_FLOAT_EQ(promoted[1], 40.0F);

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), oracle);
}

TEST(ColorCheckerTest, Real0098PayloadMatchesIndependentRbfOracleAndFixedLabGolden)
{
    ColorCheckerParams params;
    params.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    params.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    params.patches[22].target_lab = {45.439998626708984, -0.41999998688697815, 59.32999801635742};
    const std::array<float, 3> input{50.0F, 0.0F, 0.0F};
    // Fixed source-order results for the verbatim 0098 v2 payload. The
    // independent scalar oracle below protects the frozen kernel and solver
    // order without calling a production Ravo helper.
    const std::array<float, 3> golden{std::bit_cast<float>(0x4249cbc8U),
                                      std::bit_cast<float>(0x3eb8d900U),
                                      std::bit_cast<float>(0x404095c0U)};

    const auto oracle = frozen_color_checker_lab_reference(params, input);
    const auto libm_perturbation = frozen_color_checker_lab_reference(params, input, true);
    bool oracle_detects_libm = false;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(oracle[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        oracle_detects_libm |= std::abs(oracle[channel] - libm_perturbation[channel]) > 1.0e-5F;
    }
    EXPECT_TRUE(oracle_detects_libm)
        << "the independent oracle must distinguish the frozen fastlog from libm";

    auto actual = apply_color_checker_lab(params, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < golden.size(); ++channel)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(golden[channel]))
            << channel;
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.value()[channel]),
                  std::bit_cast<std::uint32_t>(oracle[channel]))
            << channel;
    }
}

TEST(ColorCheckerTest, ZeroOneFourAndRbfPatchModesMatchTheIndependentOracle)
{
    const std::array<float, 3> input{0.25F, 0.5F, 0.75F};

    ColorCheckerParams zero{{}};
    auto actual = apply_color_checker_lab(zero, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams one{{{{{2.0, 4.0, 5.0}}, {{6.0, 2.0, 10.0}}}}};
    actual = apply_color_checker_lab(one, {1.0F, 8.0F, 2.5F}, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), (std::array<float, 3>{3.0F, 4.0F, 5.0F}));

    ColorCheckerParams four{{
        {{{0.0, 0.0, 0.0}}, {{1.0, -2.0, 0.5}}},
        {{{1.0, 0.0, 0.0}}, {{3.0, -1.5, -0.5}}},
        {{{0.0, 1.0, 0.0}}, {{4.0, -1.0, 2.5}}},
        {{{0.0, 0.0, 1.0}}, {{5.0, -0.5, 4.5}}},
    }};
    const auto four_oracle = frozen_color_checker_lab_reference(four, input);
    EXPECT_EQ(four_oracle, (std::array<float, 3>{6.0F, -0.25F, 4.25F}));
    actual = apply_color_checker_lab(four, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), four_oracle);

    ColorCheckerParams five = four;
    five.patches.push_back({{{1.0, 1.0, 1.0}}, {{12.0, 3.0, -7.0}}});
    const auto five_oracle = frozen_color_checker_lab_reference(five, input);
    actual = apply_color_checker_lab(five, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], five_oracle[channel], 2.0e-5F) << channel;
    }
    EXPECT_NE(actual.value(), input);

    auto expanded = color_checker_params_for_preset("expanded_color_checker");
    ASSERT_TRUE(expanded) << expanded.error().message;
    ASSERT_EQ(expanded.value().patches.size(), kColorCheckerMaxPatchCount);
    const std::array<float, 3> expanded_input{52.0F, 18.0F, -21.0F};
    const auto expanded_oracle =
        frozen_color_checker_lab_reference(expanded.value(), expanded_input);
    actual = apply_color_checker_lab(expanded.value(), expanded_input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_NEAR(actual.value()[channel], expanded_oracle[channel], 2.0e-5F) << channel;
    }
}

TEST(ColorCheckerTest, SingularFallbackPreservesFrozenSequentialAndSharedMatrixSemantics)
{
    ColorCheckerParams two{{
        {{{1.0, 2.0, 3.0}}, {{2.0, 7.0, 11.0}}},
        {{{3.0, 2.0, 6.0}}, {{10.0, 9.0, 17.0}}},
    }};
    const std::array<float, 3> input{2.0F, 5.0F, 7.0F};
    const auto two_oracle = frozen_color_checker_lab_reference(two, input);
    EXPECT_FLOAT_EQ(two_oracle[0], 6.0F);
    EXPECT_FLOAT_EQ(two_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(two_oracle[2], input[2]);
    auto actual = apply_color_checker_lab(two, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), two_oracle);

    ColorCheckerParams three{{
        {{{1.0, 1.0, 5.0}}, {{2.0, -1.0, 12.0}}},
        {{{2.0, 3.0, 5.0}}, {{7.0, 4.0, 18.0}}},
        {{{4.0, 2.0, 5.0}}, {{11.0, 9.0, 24.0}}},
    }};
    const auto three_oracle = frozen_color_checker_lab_reference(three, input);
    EXPECT_NE(three_oracle[0], input[0]);
    EXPECT_NE(three_oracle[1], input[1]);
    EXPECT_FLOAT_EQ(three_oracle[2], input[2]);
    actual = apply_color_checker_lab(three, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), three_oracle);

    ColorCheckerParams four_singular{{
        {{{1.0, 2.0, 3.0}}, {{8.0, 9.0, 10.0}}},
        {{{1.0, 2.0, 3.0}}, {{11.0, 12.0, 13.0}}},
        {{{2.0, 3.0, 4.0}}, {{14.0, 15.0, 16.0}}},
        {{{3.0, 4.0, 5.0}}, {{17.0, 18.0, 19.0}}},
    }};
    actual = apply_color_checker_lab(four_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);

    ColorCheckerParams rbf_singular = four_singular;
    rbf_singular.patches.push_back({{{4.0, 5.0, 6.0}}, {{20.0, 21.0, 22.0}}});
    actual = apply_color_checker_lab(rbf_singular, input, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(actual.value(), input);
}

TEST(ColorCheckerTest, WorkingPublicationIsOwnedImmutableAndRejectsEveryInvalidBoundary)
{
    WorkingImage input = legacy_color_balance_working_fixture();
    input.rgb.front() = -0.1F;
    input.rgb.back() = 1.2F;
    const WorkingImage original = input;

    auto output = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output.value().width, input.width);
    EXPECT_EQ(output.value().height, input.height);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(output.value().exposure_analysis, input.exposure_analysis);
    EXPECT_NE(output.value().rgb.data(), input.rgb.data());
    ASSERT_FALSE(input.color_profile.icc_bytes.empty());
    EXPECT_NE(output.value().color_profile.icc_bytes.data(), input.color_profile.icc_bytes.data());
    output.value().rgb[0] = 42.0F;
    output.value().color_profile.icc_bytes[0] = 99U;
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto operation_parameters = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(operation_parameters) << operation_parameters.error().message;
    OperationInstance operation{std::string(kColorCheckerOperationId),
                                kColorCheckerOperationSchemaVersion,
                                "colorchecker-dispatch",
                                true,
                                operation_parameters.value(),
                                std::nullopt};
    auto dispatched = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(dispatched) << dispatched.error().message;
    auto direct = apply_color_checker(input, ColorCheckerParams{}, CancellationToken{});
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(dispatched.value().rgb, direct.value().rgb);
    operation.enabled = false;
    auto disabled = apply_color_checker(input, operation, CancellationToken{});
    ASSERT_TRUE(disabled) << disabled.error().message;
    EXPECT_EQ(disabled.value().rgb, input.rgb);
    EXPECT_NE(disabled.value().rgb.data(), input.rgb.data());

    WorkingImage zero = input;
    zero.width = 0U;
    auto rejected = apply_color_checker(zero, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_dimensions");
    WorkingImage wrong_size = input;
    wrong_size.rgb.pop_back();
    rejected = apply_color_checker(wrong_size, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_colorchecker_buffer");
    WorkingImage wrong_model = input;
    wrong_model.color_profile.model = ColorModel::kLab;
    rejected = apply_color_checker(wrong_model, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    WorkingImage wrong_profile = input;
    wrong_profile.color_profile.identifier = "srgb";
    rejected = apply_color_checker(wrong_profile, ColorCheckerParams{}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_colorchecker_working_space");
    for (const float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()})
    {
        WorkingImage nonfinite = input;
        nonfinite.rgb[1] = invalid;
        const auto source = nonfinite.rgb;
        rejected = apply_color_checker(nonfinite, ColorCheckerParams{}, CancellationToken{});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_colorchecker_input");
        ASSERT_EQ(nonfinite.rgb.size(), source.size());
        for (std::size_t index = 0U; index < source.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(nonfinite.rgb[index]),
                      std::bit_cast<std::uint32_t>(source[index]));
        }
    }

    ColorCheckerParams invalid_params;
    invalid_params.patches[0].target_lab[1] = std::numeric_limits<double>::infinity();
    auto invalid_fit =
        apply_color_checker_lab(invalid_params, {50.0F, 0.0F, 0.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    ColorCheckerParams zero_denominator{{{{{1.0, 0.0, 2.0}}, {{2.0, 1.0, 4.0}}}}};
    invalid_fit =
        apply_color_checker_lab(zero_denominator, {1.0F, 2.0F, 3.0F}, CancellationToken{});
    ASSERT_FALSE(invalid_fit);
    EXPECT_EQ(invalid_fit.error().context.at("reason"), "invalid_colorchecker_denominator");

    CancellationSource pre_cancelled;
    ASSERT_TRUE(pre_cancelled.cancel("colorchecker-pre"));
    rejected = apply_color_checker(input, ColorCheckerParams{}, pre_cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);

    auto canonical = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(canonical) << canonical.error().message;
    OperationInstance masked{std::string(kColorCheckerOperationId),
                             kColorCheckerOperationSchemaVersion,
                             "colorchecker-mask",
                             true,
                             std::move(canonical).value(),
                             "mask-1"};
    rejected = apply_color_checker(input, masked, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "colorchecker_mask_graph_unavailable");
    EXPECT_EQ(input.width, original.width);
    EXPECT_EQ(input.height, original.height);
    EXPECT_EQ(input.rgb, original.rgb);
    EXPECT_EQ(input.color_profile, original.color_profile);
    EXPECT_EQ(input.exposure_analysis, original.exposure_analysis);
}

TEST(D50LabBridgeTest, MatricesAndD50WhiteBlackMatchFrozenBitGoldens)
{
    struct MatrixCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> forward;
        std::array<std::uint32_t, 3> inverse;
    };
    const std::array cases{
        MatrixCase{{1.0F, 0.0F, 0.0F},
                   {0x3edf452fU, 0x3e63d838U, 0x3c6443e2U},
                   {0x40489119U, 0xbf7a9091U, 0x3d93580fU}},
        MatrixCase{{0.0F, 1.0F, 0.0F},
                   {0x3ec5273aU, 0x3f37855bU, 0x3dc6deb9U},
                   {0xbfcef57dU, 0x3ff54420U, 0xbe6a7cb9U}},
        MatrixCase{{0.0F, 0.0F, 1.0F},
                   {0x3e1283abU, 0x3d78496dU, 0x3f36d410U},
                   {0xbefb31d6U, 0x3d090710U, 0x3fb3defeU}},
    };
    for (const auto &[input, forward, inverse] : cases)
    {
        expect_frozen_d50_bits(d50_lab::linear_rec709_to_xyz(input),
                               frozen_linear_rec709_to_xyz_d50(input), forward);
        expect_frozen_d50_bits(d50_lab::xyz_to_linear_rec709(input),
                               frozen_xyz_d50_to_linear_rec709(input), inverse);
    }

    constexpr FrozenD50Triplet black_xyz{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet d50_white{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet black_lab{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet white_lab{100.0F, 0.0F, 0.0F};
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(black_xyz), frozen_xyz_d50_to_lab(black_xyz),
                           {0x00000000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_white), frozen_xyz_d50_to_lab(d50_white),
                           {0x42c80000U, 0x00000000U, 0x80000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(black_lab), frozen_lab_to_xyz_d50(black_lab),
                           {0x00000000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(white_lab), frozen_lab_to_xyz_d50(white_lab),
                           {0x3f76d5d0U, 0x3f800000U, 0x3f532ca5U});
}

TEST(D50LabBridgeTest, XyzToLabFreezesEpsilonAndReciprocalMultiplyOrder)
{
    constexpr float epsilon = 216.0F / 24389.0F;
    struct BranchCase
    {
        float y;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{epsilon * 0.99F, {0x40fd70a8U, 0xc2088d41U, 0x415a7b9bU}},
        BranchCase{epsilon, {0x41000000U, 0xc209ee59U, 0x415cb08fU}},
        BranchCase{epsilon * 1.01F, {0x41014698U, 0xc20b4e47U, 0x415ee3a5U}},
    };
    for (const auto &[y, expected] : cases)
    {
        const FrozenD50Triplet xyz{0.0F, y, 0.0F};
        expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(xyz), frozen_xyz_d50_to_lab(xyz),
                                         expected);
    }

    constexpr FrozenD50Triplet rgb{0.1938238604679151F, 0.36766030739017674F, 0.38827863670090734F};
    const auto xyz = frozen_linear_rec709_to_xyz_d50(rgb);
    const auto expected = frozen_xyz_d50_to_lab(xyz);
    expect_frozen_d50_cbrt_reference(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb)),
                                     expected, {0x42805bf3U, 0xc15d8c10U, 0xc0deecf2U});
}

TEST(D50LabBridgeTest, LabToXyzFreezesInverseThresholdAndScaleMultiplyOrder)
{
    struct BranchCase
    {
        float lightness;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        BranchCase{7.99F, {0x3c0bbc07U, 0x3c10ec37U, 0x3bef17efU}},
        BranchCase{8.0F, {0x3c0be8cdU, 0x3c111aa6U, 0x3bef648aU}},
        BranchCase{8.01F, {0x3c0c1598U, 0x3c11491bU, 0x3befb12fU}},
    };
    for (const auto &[lightness, expected] : cases)
    {
        const FrozenD50Triplet lab{lightness, 0.0F, 0.0F};
        expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), frozen_lab_to_xyz_d50(lab), expected);
    }

    constexpr FrozenD50Triplet lab{50.0F, 20.0F, -30.0F};
    const auto expected = frozen_lab_to_xyz_d50(lab);
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(lab), expected,
                           {0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf659U});

    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr float threshold = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const float fy = (lab[0] + 16.0F) / 116.0F;
    const FrozenD50Triplet divided{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
    FrozenD50Triplet divide_perturbation{};
    for (std::size_t channel = 0U; channel < divide_perturbation.size(); ++channel)
    {
        const float value = divided[channel] > threshold ?
                                divided[channel] * divided[channel] * divided[channel] :
                                (116.0F * divided[channel] - 16.0F) / kappa;
        divide_perturbation[channel] = d50[channel] * value;
    }
    EXPECT_EQ(d50_triplet_bits(divide_perturbation),
              (std::array<std::uint32_t, 3>{0x3e5ef828U, 0x3e3c9b63U, 0x3e9cf65cU}));
    EXPECT_NE(d50_triplet_bits(divide_perturbation), d50_triplet_bits(expected));
}

TEST(D50LabBridgeTest, ExtendedRoundTripsAndNonFiniteValuesPreserveFrozenClassification)
{
    constexpr FrozenD50Triplet extended_rgb{-0.25F, 0.5F, 1.75F};
    const auto expected_lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(extended_rgb));
    expect_frozen_d50_bits(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(extended_rgb)),
                           expected_lab, {0x428c3252U, 0xc19ff315U, 0xc2a7fbbeU});

    constexpr FrozenD50Triplet extended_lab{-25.0F, 120.0F, -90.0F};
    expect_frozen_d50_bits(d50_lab::lab_to_xyz(extended_lab), frozen_lab_to_xyz_d50(extended_lab),
                           {0x3b46abe5U, 0xbce2b9a4U, 0x3d2e846eU});

    struct RoundTripCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> expected;
    };
    const std::array cases{
        RoundTripCase{{0.25F, 0.5F, 0.75F}, {0x3e800006U, 0x3efffffcU, 0x3f400000U}},
        RoundTripCase{extended_rgb, {0xbe7ffffaU, 0x3efffffcU, 0x3fe00002U}},
    };
    for (const auto &[input, expected] : cases)
    {
        const auto oracle = frozen_xyz_d50_to_linear_rec709(
            frozen_lab_to_xyz_d50(frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(input))));
        const auto actual = d50_lab::xyz_to_linear_rec709(
            d50_lab::lab_to_xyz(d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(input))));
        expect_frozen_d50_cbrt_reference(actual, oracle, expected);
    }

    const std::array nonfinite{std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()};
    const auto all_nonfinite = [](const FrozenD50Triplet &value)
    {
        return std::ranges::all_of(value,
                                   [](const float sample) { return !std::isfinite(sample); });
    };
    for (const float sample : nonfinite)
    {
        const FrozenD50Triplet value{sample, sample, sample};
        EXPECT_TRUE(all_nonfinite(d50_lab::linear_rec709_to_xyz(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_linear_rec709(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::xyz_to_lab(value)));
        EXPECT_TRUE(all_nonfinite(d50_lab::lab_to_xyz(value)));
    }
}

} // namespace
} // namespace ravo
