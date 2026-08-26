#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/profile_gamma.h"

#include "input_color.h"
#include "profile_gamma.h"

namespace ravo
{
namespace
{

constexpr float kNoiseFloor = 1.0F / 65536.0F;

[[nodiscard]] ProfiledColorBuffer make_profiled(const std::vector<float> &channels,
                                                const std::uint32_t width,
                                                const std::uint32_t height,
                                                const ColorProfileState &profile = {})
{
    ProfiledColorBuffer result;
    result.width = width;
    result.height = height;
    result.channels = channels;
    result.color_profile = profile;
    return result;
}

[[nodiscard]] OperationInstance profile_gamma_operation(const ProfileGammaParams &params,
                                                        const bool enabled = true)
{
    return {std::string(kProfileGammaOperationId),
            kProfileGammaOperationSchemaVersion,
            "profilegamma-test",
            enabled,
            {{"mode", ParameterValue{params.mode}},
             {"linear", ParameterValue{params.linear}},
             {"gamma", ParameterValue{params.gamma}},
             {"dynamic_range", ParameterValue{params.dynamic_range}},
             {"grey_point", ParameterValue{params.grey_point}},
             {"shadows_range", ParameterValue{params.shadows_range}},
             {"security_factor", ParameterValue{params.security_factor}}},
            std::nullopt};
}

[[nodiscard]] OperationInstance input_color_operation(const InputColorParams &params = {})
{
    return {"ravo.color.input", 1, "input-test", true, input_color_to_parameters(params),
            std::nullopt};
}

[[nodiscard]] OperationInstance output_color_operation()
{
    return {"ravo.color.output",
            1,
            "output-test",
            true,
            output_color_to_parameters(OutputColorParams{}),
            std::nullopt};
}

[[nodiscard]] OperationInstance sigmoid_operation()
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-test",
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

[[nodiscard]] Recipe recipe_with_profile_gamma(const AssetDescriptor &asset,
                                               const ProfileGammaParams &params,
                                               const bool sigmoid = false)
{
    Recipe recipe;
    recipe.asset = asset;
    recipe.operations = {profile_gamma_operation(params), input_color_operation()};
    if (sigmoid)
    {
        recipe.operations.push_back(sigmoid_operation());
    }
    recipe.operations.push_back(output_color_operation());
    return recipe;
}

[[nodiscard]] RasterBuffer tagged_raster()
{
    RasterBuffer result;
    result.width = 2U;
    result.height = 2U;
    result.srgb = {8U, 30U, 220U, 210U, 45U, 12U, 80U, 170U, 35U, 250U, 240U, 130U};
    result.color_profile.kind = ColorProfileKind::kBuiltin;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = std::string(kInputProfileSrgb);
    return result;
}

[[nodiscard]] ProfiledColorBuffer profiled_raster(const RasterBuffer &raster)
{
    ProfiledColorBuffer result;
    result.width = raster.width;
    result.height = raster.height;
    result.color_profile = raster.color_profile;
    result.channels.resize(raster.srgb.size());
    for (std::size_t index = 0; index < result.channels.size(); ++index)
    {
        result.channels[index] = static_cast<float>(raster.srgb[index]) / 255.0F;
    }
    return result;
}

[[nodiscard]] std::array<std::uint64_t, 3> channel_sums(const RenderedImage &image)
{
    std::array<std::uint64_t, 3> result{};
    for (std::size_t index = 0; index + 2U < image.rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0; channel < result.size(); ++channel)
        {
            result[channel] += image.rgb[index + channel];
        }
    }
    return result;
}

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

TEST(ProfileGammaTest, LogarithmicModeUsesTheFrozenFloorsAndFastLogarithm)
{
    ProfileGammaParams params;
    params.mode = std::string(kProfileGammaModeLogarithmic);
    params.dynamic_range = 1.0;
    params.grey_point = 100.0;
    params.shadows_range = 0.0;
    const ColorProfileState profile{};
    const auto input =
        make_profiled({-2.0F, 0.0F, kNoiseFloor * 0.5F, kNoiseFloor, 1.0F, 2.0F}, 2U, 1U, profile);

    const auto derived = derive_profile_gamma(params, CancellationToken{});
    ASSERT_TRUE(derived) << derived.error().message;
    EXPECT_EQ(derived.value().mode, ProfileGammaRenderMode::kLogarithmic);
    EXPECT_TRUE(derived.value().table.empty());
    EXPECT_NEAR(profile_gamma_fastlog2(1.0F), 0.0F, 2.0e-3F);

    const auto output = apply_profile_gamma(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    ASSERT_EQ(output.value().channels.size(), input.channels.size());
    for (std::size_t index = 0; index < input.channels.size(); ++index)
    {
        float normalized = input.channels[index];
        if (normalized < kNoiseFloor)
        {
            normalized = kNoiseFloor;
        }
        const float expected = std::max(profile_gamma_fastlog2(normalized), kNoiseFloor);
        EXPECT_FLOAT_EQ(output.value().channels[index], expected) << "sample=" << index;
    }
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(input.channels,
              (std::vector<float>{-2.0F, 0.0F, kNoiseFloor * 0.5F, kNoiseFloor, 1.0F, 2.0F}));
}

TEST(ProfileGammaTest, DefaultLogarithmicBlackMiddleGreyAndWhiteAreFrozen)
{
    const ProfileGammaParams params;
    const float grey = static_cast<float>(params.grey_point) / 100.0F;
    const float black = grey * std::exp2(static_cast<float>(params.shadows_range));
    const float white =
        grey * std::exp2(static_cast<float>(params.shadows_range + params.dynamic_range));
    const auto input = make_profiled({black, grey, white}, 1U, 1U);

    const auto output = apply_profile_gamma(input, params, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_NEAR(output.value().channels[0], kNoiseFloor, 2.0e-3F);
    EXPECT_NEAR(output.value().channels[1], 0.5F, 2.0e-3F);
    EXPECT_NEAR(output.value().channels[2], 1.0F, 2.0e-3F);
}

TEST(ProfileGammaTest, GammaModeUsesTheFrozenLutPiecewiseBoundaryAndUnboundedFit)
{
    ProfileGammaParams power;
    power.mode = std::string(kProfileGammaModeGamma);
    power.linear = 0.0;
    power.gamma = 0.45;
    const auto power_derived = derive_profile_gamma(power, CancellationToken{});
    ASSERT_TRUE(power_derived) << power_derived.error().message;
    ASSERT_EQ(power_derived.value().table.size(), kProfileGammaLutEntries);
    EXPECT_FLOAT_EQ(power_derived.value().table[0], 0.0F);
    EXPECT_FLOAT_EQ(power_derived.value().table[32768U], std::pow(0.5F, 0.45F));
    EXPECT_FLOAT_EQ(power_derived.value().unbounded_coefficients[0], 1.0F);
    EXPECT_FLOAT_EQ(power_derived.value().unbounded_coefficients[1],
                    power_derived.value().table.back());

    const float before_one = std::nextafter(1.0F, 0.0F);
    const auto input = make_profiled({-4.0F, 0.0F, 0.5F, before_one, 1.0F, 2.0F}, 2U, 1U);
    const auto output = apply_profile_gamma(input, power, CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_FLOAT_EQ(output.value().channels[0], power_derived.value().table.front());
    EXPECT_FLOAT_EQ(output.value().channels[1], power_derived.value().table.front());
    EXPECT_FLOAT_EQ(output.value().channels[2], power_derived.value().table[32768U]);
    EXPECT_FLOAT_EQ(output.value().channels[3], power_derived.value().table.back());
    EXPECT_FLOAT_EQ(output.value().channels[4], power_derived.value().unbounded_coefficients[1]);
    EXPECT_FLOAT_EQ(output.value().channels[5],
                    power_derived.value().unbounded_coefficients[1] *
                        std::pow(2.0F * power_derived.value().unbounded_coefficients[0],
                                 power_derived.value().unbounded_coefficients[2]));

    ProfileGammaParams piecewise = power;
    piecewise.linear = 0.1;
    const auto piecewise_derived = derive_profile_gamma(piecewise, CancellationToken{});
    ASSERT_TRUE(piecewise_derived) << piecewise_derived.error().message;
    const float linear = 0.1F;
    const float gamma = 0.45F;
    const float exponent =
        static_cast<float>(static_cast<double>(gamma) * (1.0 - static_cast<double>(linear)) /
                           (1.0 - static_cast<double>(gamma) * static_cast<double>(linear)));
    const float a = static_cast<float>(
        1.0 / (1.0 + static_cast<double>(linear) * (static_cast<double>(exponent) - 1.0)));
    const float b = linear * (exponent - 1.0F) * a;
    const float c = std::pow(a * linear + b, exponent) / linear;
    constexpr std::size_t kLastLinear = 6553U;
    constexpr std::size_t kFirstPower = 6554U;
    EXPECT_FLOAT_EQ(piecewise_derived.value().table[kLastLinear],
                    c * static_cast<float>(kLastLinear) /
                        static_cast<float>(kProfileGammaLutEntries));
    EXPECT_FLOAT_EQ(
        piecewise_derived.value().table[kFirstPower],
        std::pow(a * static_cast<float>(kFirstPower) / static_cast<float>(kProfileGammaLutEntries) +
                     b,
                 exponent));
}

TEST(ProfileGammaTest, GammaOneLinearOneAndGammaZeroKeepTheirFrozenSpecialBranches)
{
    const auto assert_identity_lut = [](ProfileGammaParams params)
    {
        const auto derived = derive_profile_gamma(params, CancellationToken{});
        ASSERT_TRUE(derived) << derived.error().message;
        ASSERT_EQ(derived.value().table.size(), kProfileGammaLutEntries);
        EXPECT_FLOAT_EQ(derived.value().table[0], 0.0F);
        EXPECT_FLOAT_EQ(derived.value().table[32768U], 0.5F);
        EXPECT_FLOAT_EQ(derived.value().table.back(),
                        65535.0F / static_cast<float>(kProfileGammaLutEntries));
        const auto output = apply_profile_gamma(make_profiled({1.0F, 2.0F, 0.5F}, 1U, 1U), params,
                                                CancellationToken{});
        ASSERT_TRUE(output) << output.error().message;
        EXPECT_FLOAT_EQ(output.value().channels[0], derived.value().unbounded_coefficients[1]);
        EXPECT_FLOAT_EQ(output.value().channels[1],
                        derived.value().unbounded_coefficients[1] *
                            std::pow(2.0F * derived.value().unbounded_coefficients[0],
                                     derived.value().unbounded_coefficients[2]));
        EXPECT_FLOAT_EQ(output.value().channels[2], derived.value().table[32768U]);
    };

    ProfileGammaParams gamma_one;
    gamma_one.mode = std::string(kProfileGammaModeGamma);
    gamma_one.linear = 0.37;
    gamma_one.gamma = 1.0;
    assert_identity_lut(gamma_one);

    ProfileGammaParams linear_one;
    linear_one.mode = std::string(kProfileGammaModeGamma);
    linear_one.linear = 1.0;
    linear_one.gamma = 0.45;
    assert_identity_lut(linear_one);

    ProfileGammaParams gamma_zero;
    gamma_zero.mode = std::string(kProfileGammaModeGamma);
    gamma_zero.linear = 0.0;
    gamma_zero.gamma = 0.0;
    const auto zero_derived = derive_profile_gamma(gamma_zero, CancellationToken{});
    ASSERT_TRUE(zero_derived) << zero_derived.error().message;
    EXPECT_FLOAT_EQ(zero_derived.value().table.front(), 1.0F);
    EXPECT_FLOAT_EQ(zero_derived.value().table.back(), 1.0F);
    const auto zero_output = apply_profile_gamma(make_profiled({0.0F, 1.0F, 4.0F}, 1U, 1U),
                                                 gamma_zero, CancellationToken{});
    ASSERT_TRUE(zero_output) << zero_output.error().message;
    EXPECT_FLOAT_EQ(zero_output.value().channels[0], 1.0F);
    EXPECT_FLOAT_EQ(zero_output.value().channels[1], 1.0F);
    EXPECT_FLOAT_EQ(zero_output.value().channels[2], 1.0F);

    ProfileGammaParams gamma_zero_piecewise = gamma_zero;
    gamma_zero_piecewise.linear = 0.1;
    const auto zero_piecewise = derive_profile_gamma(gamma_zero_piecewise, CancellationToken{});
    ASSERT_TRUE(zero_piecewise) << zero_piecewise.error().message;
    EXPECT_FLOAT_EQ(zero_piecewise.value().table[6553U],
                    10.0F * 6553.0F / static_cast<float>(kProfileGammaLutEntries));
    EXPECT_FLOAT_EQ(zero_piecewise.value().table[6554U], 1.0F);
}

TEST(ProfileGammaTest, RejectsInvalidInputAndCancelsWithoutPublishingOutput)
{
    ProfileGammaParams params;
    params.mode = std::string(kProfileGammaModeGamma);
    auto invalid_dimensions = make_profiled({0.1F, 0.2F, 0.3F}, 2U, 1U);
    const auto dimensions = apply_profile_gamma(invalid_dimensions, params, CancellationToken{});
    ASSERT_FALSE(dimensions);
    EXPECT_EQ(dimensions.error().code, ErrorCode::kValidation);

    auto nonfinite = make_profiled({0.1F, std::numeric_limits<float>::infinity(), 0.3F}, 1U, 1U);
    const auto samples = apply_profile_gamma(nonfinite, params, CancellationToken{});
    ASSERT_FALSE(samples);
    EXPECT_EQ(samples.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(std::isinf(nonfinite.channels[1]));

    auto out_of_range = params;
    out_of_range.gamma = 1.1;
    const auto parameter = derive_profile_gamma(out_of_range, CancellationToken{});
    ASSERT_FALSE(parameter);
    EXPECT_EQ(parameter.error().code, ErrorCode::kValidation);

    ColorProfileState non_rgb_profile;
    non_rgb_profile.model = ColorModel::kLab;
    const auto non_rgb = apply_profile_gamma(
        make_profiled({0.1F, 0.2F, 0.3F}, 1U, 1U, non_rgb_profile), params, CancellationToken{});
    ASSERT_FALSE(non_rgb);
    EXPECT_EQ(non_rgb.error().code, ErrorCode::kUnsupported);

    auto large = make_profiled({}, 1024U, 4096U);
    large.channels.assign(static_cast<std::size_t>(large.width) * large.height * 3U, 0.5F);
    const auto original = large.channels;
    ProfileGammaParams row_params;
    row_params.mode = std::string(kProfileGammaModeLogarithmic);
    const auto deadline = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                            std::chrono::milliseconds{1});
    const auto cancelled = apply_profile_gamma(large, row_params, deadline.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(large.channels, original);

    auto disabled = profile_gamma_operation(params, false);
    const auto identity = apply_profile_gamma(make_profiled({0.1F, 0.2F, 0.3F}, 1U, 1U), disabled,
                                              CancellationToken{});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().channels, (std::vector<float>{0.1F, 0.2F, 0.3F}));

    auto wrong_schema = profile_gamma_operation(params);
    ++wrong_schema.schema_version;
    const auto schema = apply_profile_gamma(make_profiled({0.1F, 0.2F, 0.3F}, 1U, 1U), wrong_schema,
                                            CancellationToken{});
    ASSERT_FALSE(schema);
    EXPECT_EQ(schema.error().code, ErrorCode::kValidation);

    auto masked = profile_gamma_operation(params);
    masked.mask_id = "not-supported";
    const auto mask =
        apply_profile_gamma(make_profiled({0.1F, 0.2F, 0.3F}, 1U, 1U), masked, CancellationToken{});
    ASSERT_FALSE(mask);
    EXPECT_EQ(mask.error().code, ErrorCode::kUnsupported);
}

TEST(ProfileGammaTest, TaggedRasterRunsBothModesBeforeInputColor)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto raster = tagged_raster();
    const AssetDescriptor asset{"tagged-raster", "memory:tagged-raster", std::nullopt};

    ProfileGammaParams logarithmic;
    logarithmic.mode = std::string(kProfileGammaModeLogarithmic);
    ProfileGammaParams gamma;
    gamma.mode = std::string(kProfileGammaModeGamma);
    const std::array<ProfileGammaParams, 2> modes{logarithmic, gamma};
    std::array<std::vector<std::uint8_t>, 2> rendered_pixels;
    std::array<std::array<std::uint64_t, 3>, 2> rendered_sums{};
    // Ravo-owned reference for the fixed tagged 2x2 sRGB raster above.
    const std::array<std::array<double, 3>, 2> references{
        std::array<double, 3>{584.0, 603.0, 547.0},
        std::array<double, 3>{655.0, 645.0, 547.0},
    };
    for (std::size_t index = 0; index < modes.size(); ++index)
    {
        const Recipe recipe = recipe_with_profile_gamma(asset, modes[index]);
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        const auto rendered = engine.value().render_to_image(request, &raster);
        ASSERT_TRUE(rendered) << rendered.error().message;
        EXPECT_EQ(rendered.value().color_profile.identifier, kInputProfileSrgb);

        const auto corrected =
            apply_profile_gamma(profiled_raster(raster), modes[index], CancellationToken{});
        ASSERT_TRUE(corrected) << corrected.error().message;
        EXPECT_EQ(corrected.value().color_profile, raster.color_profile);
        auto input = resolve_input_color(recipe);
        ASSERT_TRUE(input) << input.error().message;
        const auto working =
            apply_input_color(corrected.value(), input.value(), CancellationToken{});
        ASSERT_TRUE(working) << working.error().message;
        const auto expected =
            engine.value().render_linear_working(working.value(), recipe, CancellationToken{});
        ASSERT_TRUE(expected) << expected.error().message;
        EXPECT_EQ(rendered.value().rgb, expected.value().rgb);
        rendered_sums[index] = channel_sums(rendered.value());
        for (std::size_t channel = 0; channel < rendered_sums[index].size(); ++channel)
        {
            EXPECT_NEAR(static_cast<double>(rendered_sums[index][channel]),
                        references[index][channel], 4.0)
                << "mode=" << index << " channel=" << channel;
        }
        rendered_pixels[index] = rendered.value().rgb;
    }
    EXPECT_NE(rendered_pixels[0], rendered_pixels[1]);
}

TEST(ProfileGammaTest, InputColorCacheFingerprintIncludesEveryEnabledProfileGammaField)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const AssetDescriptor asset{"cache", "memory:cache", std::nullopt};
    ProfileGammaParams baseline;
    baseline.mode = std::string(kProfileGammaModeGamma);
    const auto baseline_recipe = recipe_with_profile_gamma(asset, baseline);
    const auto baseline_fingerprint = engine.value().input_color_cache_fingerprint(baseline_recipe);
    ASSERT_TRUE(baseline_fingerprint) << baseline_fingerprint.error().message;

    const auto expect_changed = [&](const ProfileGammaParams &params)
    {
        const auto candidate =
            engine.value().input_color_cache_fingerprint(recipe_with_profile_gamma(asset, params));
        ASSERT_TRUE(candidate) << candidate.error().message;
        EXPECT_NE(candidate.value(), baseline_fingerprint.value());
    };
    auto changed = baseline;
    changed.mode = std::string(kProfileGammaModeLogarithmic);
    expect_changed(changed);
    changed = baseline;
    changed.linear = std::nextafter(baseline.linear, 1.0);
    expect_changed(changed);
    changed = baseline;
    changed.linear += 0.1;
    expect_changed(changed);
    changed = baseline;
    changed.gamma -= 0.1;
    expect_changed(changed);
    changed = baseline;
    changed.dynamic_range += 1.0;
    expect_changed(changed);
    changed = baseline;
    changed.grey_point += 1.0;
    expect_changed(changed);
    changed = baseline;
    changed.shadows_range += 1.0;
    expect_changed(changed);
    changed = baseline;
    changed.security_factor += 1.0;
    expect_changed(changed);
}

TEST(ProfileGammaTest, RecipeOrderPreconditionAndMire1ReferencesCoverBothModes)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto raster = tagged_raster();
    ProfileGammaParams params;
    params.mode = std::string(kProfileGammaModeGamma);
    Recipe invalid;
    invalid.asset = {"tagged-raster", "memory:order", std::nullopt};
    invalid.operations = {input_color_operation(), profile_gamma_operation(params),
                          output_color_operation()};
    const auto rejected =
        engine.value().linear_working_from_raster(raster, invalid, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    const AssetDescriptor asset{"mire1", mire1_path(), std::nullopt};
    ProfileGammaParams logarithmic;
    logarithmic.mode = std::string(kProfileGammaModeLogarithmic);
    ProfileGammaParams gamma;
    gamma.mode = std::string(kProfileGammaModeGamma);
    std::array<std::array<std::uint64_t, 3>, 2> sums{};
    const std::array<ProfileGammaParams, 2> modes{logarithmic, gamma};
    // Ravo-owned macOS reference for 64x48 mire1.cr2 with the canonical
    // profile_gamma defaults followed by the default Sigmoid/output path.
    const std::array<std::array<double, 3>, 2> references{
        std::array<double, 3>{526337.0, 508989.0, 483520.0},
        std::array<double, 3>{455108.0, 429113.0, 400149.0},
    };
    for (std::size_t index = 0; index < modes.size(); ++index)
    {
        const Recipe recipe = recipe_with_profile_gamma(asset, modes[index], true);
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        request.output_width = 64U;
        request.output_height = 48U;
        const auto rendered = engine.value().render_to_image(request, nullptr);
        ASSERT_TRUE(rendered) << rendered.error().message;
        EXPECT_EQ(rendered.value().color_profile.identifier, kInputProfileSrgb);
        sums[index] = channel_sums(rendered.value());
        for (std::size_t channel = 0; channel < sums[index].size(); ++channel)
        {
            EXPECT_NEAR(static_cast<double>(sums[index][channel]), references[index][channel],
                        2500.0)
                << "mode=" << index << " channel=" << channel;
        }
    }
    EXPECT_NE(sums[0], sums[1]);
}

} // namespace
} // namespace ravo
