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
#include "ravo/recipe/rapidraw_tone.h"
#include "ravo/recipe/rapidraw_tone_controls.h"

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
#include "engine_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_test_support;

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

TEST(EngineFacadeTest, RawHighlightReconstructionChangesMire1)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe identity;
    identity.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(identity);
    RenderRequest base_request;
    base_request.asset = identity.asset;
    base_request.recipe = identity;
    base_request.output_width = 64;
    base_request.output_height = 48;
    auto base = engine.value().render_to_image(base_request);
    ASSERT_TRUE(base) << base.error().message;

    Recipe reconstructed = identity;
    reconstructed.operations.push_back({"ravo.raw.highlights",
                                        1,
                                        "raw-hl-1",
                                        true,
                                        {{"mode", ParameterValue{"opposed"}},
                                         {"amount", ParameterValue{1.0}},
                                         {"clip", ParameterValue{0.92}}},
                                        std::nullopt});
    RenderRequest request = base_request;
    request.recipe = reconstructed;
    auto rebuilt = engine.value().render_to_image(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(rebuilt.value().width, base.value().width);
    EXPECT_EQ(rebuilt.value().height, base.value().height);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("highlights"));
    request.cancellation = cancellation.token();
    auto cancelled = engine.value().render_to_image(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);

    DecodedRaw clipped;
    clipped.width = 24U;
    clipped.height = 24U;
    clipped.cfa_width = 2U;
    clipped.cfa_height = 2U;
    clipped.black_level = 100;
    clipped.white_level = 1100U;
    clipped.linear_response_limits.fill(900U);
    clipped.as_shot_white_balance = {2.0F, 1.0F, 1.5F, 1.0F};
    clipped.has_as_shot_white_balance = true;
    clipped.cfa_channels = {0U, 1U, 1U, 2U};
    clipped.pixels.resize(static_cast<std::size_t>(clipped.width) * clipped.height);
    declare_linear_srgb_matrix(clipped);
    for (std::uint32_t row = 0U; row < clipped.height; ++row)
    {
        for (std::uint32_t col = 0U; col < clipped.width; ++col)
        {
            const std::uint8_t channel = clipped.cfa_channels[(row % 2U) * 2U + (col % 2U)];
            clipped.pixels[static_cast<std::size_t>(row) * clipped.width + col] =
                channel == 0U ? 550U :
                channel == 2U ? 700U :
                                900U;
        }
    }
    const auto clipped_source = clipped.pixels;
    auto baseline_recipe = recipe_from_develop(
        {"synthetic-linear-limit", "memory:raw", std::nullopt}, develop_raw_import_baseline());
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto no_reconstruction = develop_raw_import_baseline();
    no_reconstruction.raw_highlights = 0.0;
    auto no_reconstruction_recipe = recipe_from_develop(
        {"synthetic-linear-limit", "memory:raw", std::nullopt}, no_reconstruction);
    ASSERT_TRUE(no_reconstruction_recipe) << no_reconstruction_recipe.error().message;
    const auto baseline_bytes =
        estimate_raw_render_memory(clipped, baseline_recipe.value(), clipped.width, clipped.height);
    const auto no_reconstruction_bytes = estimate_raw_render_memory(
        clipped, no_reconstruction_recipe.value(), clipped.width, clipped.height);
    EXPECT_GE(baseline_bytes, no_reconstruction_bytes +
                                  clipped.pixels.size() * (sizeof(std::uint16_t) + sizeof(float)));
    auto without_limit = clipped;
    without_limit.linear_response_limits.fill(0U);
    auto unrecovered = engine.value().linear_working_from_raw(
        without_limit, baseline_recipe.value(), clipped.width, clipped.height, CancellationToken{});
    auto recovered = engine.value().linear_working_from_raw(
        clipped, baseline_recipe.value(), clipped.width, clipped.height, CancellationToken{});
    ASSERT_TRUE(unrecovered) << unrecovered.error().message;
    ASSERT_TRUE(recovered) << recovered.error().message;
    const std::size_t center =
        (static_cast<std::size_t>(clipped.height / 2U) * clipped.width + clipped.width / 2U) * 3U;
    EXPECT_GT(unrecovered.value().rgb[center] - unrecovered.value().rgb[center + 1U], 0.05F);
    EXPECT_NEAR(recovered.value().rgb[center], recovered.value().rgb[center + 1U], 2.0e-3F);
    EXPECT_NEAR(recovered.value().rgb[center + 1U], recovered.value().rgb[center + 2U], 2.0e-3F);
    EXPECT_EQ(clipped.pixels, clipped_source);

    auto invalid_limit = clipped;
    invalid_limit.linear_response_limits[0] = invalid_limit.white_level + 1U;
    auto rejected = engine.value().linear_working_from_raw(
        invalid_limit, baseline_recipe.value(), clipped.width, clipped.height, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_raw_linear_response_limit");
}

[[nodiscard]] ParameterValue tone_curve_points(const std::vector<ToneCurvePoint> &points)
{
    return tone_curve_points_to_parameter(points);
}

[[nodiscard]] OperationInstance
sigmoid_operation(const double contrast = kSigmoidContrastDefault,
                  const double skew = kSigmoidSkewDefault,
                  const double hue_preservation = kSigmoidHuePreservationDefault)
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-1",
            true,
            {{"working_space", ParameterValue{"linear_srgb"}},
             {"color_processing", ParameterValue{"per_channel"}},
             {"middle_grey_contrast", ParameterValue{contrast}},
             {"contrast_skewness", ParameterValue{skew}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{hue_preservation}}},
            std::nullopt};
}

[[nodiscard]] OperationInstance rapidraw_basic_tone_operation()
{
    return {std::string(kRapidRawBasicToneOperationId),
            kRapidRawBasicToneSchemaVersion,
            "rapidraw-basic-tone-1",
            true,
            {{"working_space", ParameterValue{std::string(kRapidRawBasicToneWorkingSpace)}}},
            std::nullopt};
}

[[nodiscard]] OperationInstance
rapidraw_tone_controls_operation(const RapidRawToneControlsParams &params)
{
    auto parameters = rapidraw_tone_controls_to_parameters(params);
    if (!parameters)
        return {};
    return {std::string(kRapidRawToneControlsOperationId),
            kRapidRawToneControlsSchemaVersion,
            "rapidraw-tone-controls-1",
            true,
            std::move(parameters).value(),
            std::nullopt};
}

TEST(EngineFacadeTest, SigmoidMapsSyntheticPixelsAndPreservesHueByPolicy)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto source = gradient_raster();
    auto standard = render_op(engine.value(), source, sigmoid_operation());
    auto skewed = render_op(engine.value(), source, sigmoid_operation(1.5, -0.4, 1.0));
    ASSERT_TRUE(standard) << standard.error().message;
    ASSERT_TRUE(skewed) << skewed.error().message;
    EXPECT_NE(standard.value().rgb, source.srgb);
    EXPECT_NE(skewed.value().rgb, standard.value().rgb);

    const auto saturated = solid_raster(8, 8, 245, 100, 20);
    auto preserved = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 1.0));
    auto shifted = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 0.0));
    ASSERT_TRUE(preserved) << preserved.error().message;
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(preserved.value().rgb, shifted.value().rgb);
    EXPECT_GT(preserved.value().rgb[0], preserved.value().rgb[1]);
    EXPECT_GT(preserved.value().rgb[1], preserved.value().rgb[2]);

    auto ratio = sigmoid_operation();
    ratio.parameters["color_processing"] = ParameterValue{"rgb_ratio"};
    auto ratio_rendered = render_op(engine.value(), saturated, std::move(ratio));
    ASSERT_TRUE(ratio_rendered) << ratio_rendered.error().message;
    EXPECT_NE(ratio_rendered.value().rgb, preserved.value().rgb);

    auto boundary_operation = sigmoid_operation(kSigmoidContrastMax, kSigmoidSkewMax, 0.0);
    boundary_operation.parameters["display_white_target"] = ParameterValue{kSigmoidDisplayWhiteMax};
    boundary_operation.parameters["display_black_target"] = ParameterValue{kSigmoidDisplayBlackMin};
    auto boundary = render_op(engine.value(), source, std::move(boundary_operation));
    ASSERT_TRUE(boundary) << boundary.error().message;
    EXPECT_EQ(boundary.value().rgb.size(), source.srgb.size());

    WorkingImage rapidraw_source;
    rapidraw_source.width = 7U;
    rapidraw_source.height = 1U;
    for (const float value : {0.0F, 0.001F, 0.0031308F, 0.18F, 0.5F, 1.0F, 2.0F})
    {
        rapidraw_source.rgb.insert(rapidraw_source.rgb.end(), 3U, value);
    }
    Recipe rapidraw_recipe;
    rapidraw_recipe.operations.push_back(rapidraw_basic_tone_operation());
    auto rapidraw = apply_recipe_ops(rapidraw_source, rapidraw_recipe, CancellationToken{});
    ASSERT_TRUE(rapidraw) << rapidraw.error().message;
    const std::array<float, 7> rapidraw_gold = {
        0.0F, 0.000434510F, 0.001539832F, 0.207694889F, 0.650689324F, 1.0F, 1.0F};
    ASSERT_EQ(rapidraw.value().rgb.size(), rapidraw_gold.size() * 3U);
    for (std::size_t index = 0; index < rapidraw_gold.size(); ++index)
    {
        EXPECT_NEAR(rapidraw.value().rgb[index * 3U], rapidraw_gold[index], 2.0e-6F) << index;
    }

    auto invalid_rapidraw = rapidraw_basic_tone_operation();
    invalid_rapidraw.parameters.emplace("unknown", ParameterValue{1.0});
    rapidraw_recipe.operations.front() = std::move(invalid_rapidraw);
    auto rejected = apply_recipe_ops(rapidraw_source, rapidraw_recipe, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_rapidraw_basic_tone_parameters");

    const auto apply_rapidraw_controls = [&](const RapidRawToneControlsParams &params)
    {
        WorkingImage input;
        input.width = 3U;
        input.height = 1U;
        input.rgb = {0.05F, 0.05F, 0.05F, 0.18F, 0.18F,
                     0.18F, 0.75F, 0.75F, 0.75F};
        input.canonical_roi_scale =
            CanonicalRoiScale::from_scaled_dimensions(3U, 1U, 3U, 1U);
        Recipe recipe;
        recipe.operations.push_back(rapidraw_tone_controls_operation(params));
        return apply_recipe_ops(std::move(input), recipe, CancellationToken{});
    };
    RapidRawToneControlsParams controls;
    controls.ev_shift = 0.8;
    auto shifted_controls = apply_rapidraw_controls(controls);
    ASSERT_TRUE(shifted_controls) << shifted_controls.error().message;
    EXPECT_NEAR(shifted_controls.value().rgb[0], 0.1F, 1.0e-6F);
    EXPECT_NEAR(shifted_controls.value().rgb[3], 0.36F, 1.0e-6F);

    controls = {};
    controls.exposure = 0.8;
    auto exposed_controls = apply_rapidraw_controls(controls);
    ASSERT_TRUE(exposed_controls) << exposed_controls.error().message;
    EXPECT_NEAR(exposed_controls.value().rgb[0], 0.10794678F, 2.0e-6F);
    EXPECT_NEAR(exposed_controls.value().rgb[3], 0.34097303F, 2.0e-6F);
    EXPECT_NEAR(exposed_controls.value().rgb[6], 0.92406817F, 2.0e-6F);

    controls = {};
    controls.contrast = 100.0;
    auto contrasted_controls = apply_rapidraw_controls(controls);
    ASSERT_TRUE(contrasted_controls) << contrasted_controls.error().message;
    EXPECT_NEAR(contrasted_controls.value().rgb[0], 0.00658398F, 2.0e-6F);
    EXPECT_NEAR(contrasted_controls.value().rgb[3], 0.13854996F, 2.0e-6F);
    EXPECT_NEAR(contrasted_controls.value().rgb[6], 0.96157659F, 2.0e-6F);

    controls = {};
    controls.highlights = -100.0;
    auto lowered_highlights = apply_rapidraw_controls(controls);
    ASSERT_TRUE(lowered_highlights) << lowered_highlights.error().message;
    EXPECT_NEAR(lowered_highlights.value().rgb[6], 0.52392403F, 2.0e-6F);

    controls = {};
    controls.shadows = 100.0;
    auto lifted_shadows = apply_rapidraw_controls(controls);
    ASSERT_TRUE(lifted_shadows) << lifted_shadows.error().message;
    EXPECT_NEAR(lifted_shadows.value().rgb[0], 0.07866573F, 2.0e-5F);
    EXPECT_NEAR(lifted_shadows.value().rgb[3], 0.20559042F, 2.0e-5F);

    controls = {};
    controls.whites = 30.0;
    auto raised_whites = apply_rapidraw_controls(controls);
    ASSERT_TRUE(raised_whites) << raised_whites.error().message;
    EXPECT_NEAR(raised_whites.value().rgb[3], 0.24F, 1.0e-6F);

    controls = {};
    controls.blacks = 40.0;
    auto lifted_blacks = apply_rapidraw_controls(controls);
    ASSERT_TRUE(lifted_blacks) << lifted_blacks.error().message;
    EXPECT_NEAR(lifted_blacks.value().rgb[0], 0.05321993F, 2.0e-5F);
    EXPECT_NEAR(lifted_blacks.value().rgb[3], 0.18032259F, 2.0e-5F);

    auto invalid_controls = rapidraw_tone_controls_operation(controls);
    invalid_controls.parameters["blacks"] =
        ParameterValue{std::numeric_limits<double>::quiet_NaN()};
    Recipe invalid_controls_recipe;
    invalid_controls_recipe.operations.push_back(std::move(invalid_controls));
    auto invalid_controls_result =
        apply_recipe_ops(rapidraw_source, invalid_controls_recipe, CancellationToken{});
    ASSERT_FALSE(invalid_controls_result);
    EXPECT_EQ(invalid_controls_result.error().context.at("reason"),
              "invalid_rapidraw_tone_controls");

    CancellationSource cancelled_controls;
    ASSERT_TRUE(cancelled_controls.cancel("rapidraw controls"));
    Recipe cancelled_controls_recipe;
    cancelled_controls_recipe.operations.push_back(rapidraw_tone_controls_operation(controls));
    auto cancelled_controls_result = apply_recipe_ops(rapidraw_source, cancelled_controls_recipe,
                                                       cancelled_controls.token());
    ASSERT_FALSE(cancelled_controls_result);
    EXPECT_EQ(cancelled_controls_result.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, SigmoidHasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request, nullptr);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    std::size_t clipped_channels = 0;
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            const auto value = rendered.value().rgb[index + channel];
            sums[channel] += value;
            clipped_channels += value == 255 ? 1U : 0U;
        }
    }
    // Ravo-owned macOS reference statistics for the pinned mire1.cr2 fixture
    // after the default RCD demosaic. The tolerance permits platform libm/SIMD
    // rounding without accepting a changed look.
    EXPECT_NEAR(static_cast<double>(sums[0]), 310551.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 285591.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 269480.0, 2000.0);
    EXPECT_LT(clipped_channels, rendered.value().rgb.size() / 100U);
}

TEST(EngineFacadeTest, TemperatureManualAndCameraReferenceHaveRealRawReferences)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    Recipe manual_recipe;
    manual_recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(manual_recipe);
    manual_recipe.operations.push_back(temperature_operation(test::temperature_0000_params()));
    auto linear = engine.value().linear_working_from_raw(decoded.value(), manual_recipe, 64, 48,
                                                         CancellationToken{});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_EQ(decoded.value().pixels, original_pixels);

    const auto render_temperature = [&](TemperatureParams params)
    {
        Recipe recipe;
        recipe.asset = {"mire1", mire1_path(), std::nullopt};
        declare_input(recipe);
        recipe.operations.push_back(temperature_operation(params));
        recipe.operations.push_back(sigmoid_operation());
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        request.output_width = 64;
        request.output_height = 48;
        return engine.value().render_to_image(request);
    };
    const auto sums = [](const RenderedImage &image)
    {
        std::array<std::uint64_t, 3> result{};
        for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
        {
            for (std::size_t channel = 0; channel < result.size(); ++channel)
            {
                result[channel] += image.rgb[index + channel];
            }
        }
        return result;
    };

    auto manual = render_temperature(test::temperature_0000_params());
    ASSERT_TRUE(manual) << manual.error().message;
    const auto manual_sums = sums(manual.value());
    EXPECT_NEAR(static_cast<double>(manual_sums[0]), 310551.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(manual_sums[1]), 285591.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(manual_sums[2]), 269480.0, 2000.0);

    TemperatureParams reference;
    reference.mode = std::string(kTemperatureModeCameraReference);
    auto camera = render_temperature(reference);
    ASSERT_TRUE(camera) << camera.error().message;
    const auto camera_sums = sums(camera.value());
    EXPECT_NEAR(static_cast<double>(camera_sums[0]), 372514.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(camera_sums[1]), 289629.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(camera_sums[2]), 248367.0, 2000.0);
    EXPECT_NE(camera_sums, manual_sums);
}

TEST(EngineFacadeTest, TemperatureLateReferenceUsesOnlyExplicitChannelMixerCat)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ChannelMixerParams calibration;
    calibration.adaptation = std::string(kChannelMixerAdaptationCat16);
    calibration.illuminant_x = 0.3819674253463745;
    calibration.illuminant_y = 0.36998802423477173;
    calibration.gamut = 1.0;
    calibration.clip = true;
    const auto render = [&](TemperatureParams params)
    {
        Recipe recipe;
        recipe.asset = {"mire1", mire1_path(), std::nullopt};
        declare_input(recipe);
        recipe.operations.push_back(temperature_operation(params));
        recipe.operations.push_back(channel_mixer_operation(calibration));
        recipe.operations.push_back(sigmoid_operation());
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        request.output_width = 64;
        request.output_height = 48;
        return engine.value().render_to_image(request);
    };

    auto manual_params = test::temperature_0000_params();
    auto manual = render(manual_params);
    ASSERT_TRUE(manual) << manual.error().message;
    auto late = render(test::temperature_0171_late_params());
    ASSERT_TRUE(late) << late.error().message;
    EXPECT_EQ(late.value().rgb, manual.value().rgb);
}

TEST(EngineFacadeTest, ChannelMixerHasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ChannelMixerParams calibration;
    calibration.adaptation = std::string(kChannelMixerAdaptationCat16);
    calibration.illuminant_x = 0.3819674253463745;
    calibration.illuminant_y = 0.36998802423477173;
    calibration.gamut = 1.0;
    calibration.clip = true;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(channel_mixer_operation(calibration));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference for the frozen 0085 default CAT16 parameters on
    // mire1.cr2 after the default RCD demosaic.
    EXPECT_NEAR(static_cast<double>(sums[0]), 258339.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 296045.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 307843.0, 2000.0);
}

TEST(EngineFacadeTest, ColorBalanceRgb0083HasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(color_balance_rgb_operation(test::color_balance_0083_params()));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned macOS reference for the statically decoded 0083 schema-v4 parameters
    // in explicit linear_srgb_d50 working space. Cross-platform libm tolerance is recorded
    // without treating the unavailable legacy runner as an oracle.
    EXPECT_NEAR(static_cast<double>(sums[0]), 270856.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 283113.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 241983.0, 2500.0);
}

TEST(EngineFacadeTest, ColorChecker0098HasARealRawReferenceAndPreservesTheSource)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before.has_value());
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorCheckerParams params;
    params.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    params.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    params.patches[22].target_lab = {45.439998626708984, -0.41999998688697815, 59.32999801635742};
    auto parameters = color_checker_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back({std::string(kColorCheckerOperationId),
                                 kColorCheckerOperationSchemaVersion, "colorchecker-0098", true,
                                 std::move(parameters).value(), std::nullopt});
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned macOS reference for the verbatim frozen 0098 active patch set on
    // the pinned RAW fixture. The independent scalar oracle above owns fit parity.
    EXPECT_NEAR(static_cast<double>(sums[0]), 308446.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 293347.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 246523.0, 2500.0);
    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);
}

TEST(EngineFacadeTest, LegacyColorBalanceV4HasARealRawReferenceAndPreservesTheSource)
{
    const auto source_before = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_before.has_value());
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorBalanceParams params;
    params.mode = std::string(kColorBalanceModeLiftGammaGain);
    params.lift = {0.96, 1.03, 0.98, 1.06};
    params.gamma = {1.08, 0.91, 1.05, 0.97};
    params.gain = {1.04, 1.12, 0.95, 1.08};
    params.input_saturation = 0.84;
    params.contrast = 1.16;
    params.grey_fulcrum_percent = 18.0;
    params.output_saturation = 1.09;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(legacy_color_balance_operation(params));
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64U;
    request.output_height = 48U;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0U; index + 2U < rendered.value().rgb.size(); index += 3U)
    {
        for (std::size_t channel = 0U; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference for the frozen v4 LGG path on the pinned RAW fixture.
    // The tolerance permits cross-platform libm rounding without accepting a mode,
    // working-space, or channel-order change.
    EXPECT_NEAR(static_cast<double>(sums[0]), 378994.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 281591.0, 2500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 356197.0, 2500.0);
    const auto source_after = source_file_snapshot(mire1_path());
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);
}

TEST(EngineFacadeTest, HotPixelsHasARealRawReferenceAndKeepsDecodedFrameImmutable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(hot_pixels_operation());
    recipe.operations.push_back(sigmoid_operation());

    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original_pixels = decoded.value().pixels;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(decoded.value().pixels, original_pixels);

    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    // Ravo-owned reference for hot-pixel repair followed by the default RCD
    // demosaic on mire1.cr2.
    EXPECT_NEAR(static_cast<double>(sums[0]), 310551.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 285591.0, 1500.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 269480.0, 1500.0);
}

TEST(EngineFacadeTest, RawCaCorrectRunsFrozenDefaultOnMire1)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(raw_ca_operation());
    recipe.operations.push_back(sigmoid_operation());
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto original = decoded.value().pixels;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_EQ(decoded.value().pixels, original);

    RenderRequest budgeted;
    budgeted.asset = recipe.asset;
    budgeted.recipe = recipe;
    budgeted.output_width = 64;
    budgeted.output_height = 48;
    budgeted.memory_budget_bytes = 64U * 1024U * 1024U;
    auto budget_failure = engine.value().render_to_image(budgeted);
    ASSERT_FALSE(budget_failure);
    EXPECT_EQ(budget_failure.error().code, ErrorCode::kValidation);

    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.cacorrect")
        {
            operation.enabled = false;
        }
    }
    auto rendered =
        engine.value().render_linear_working(working.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 64U);
    EXPECT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    EXPECT_NEAR(static_cast<double>(sums[0]), 309659.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 285554.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 269558.0, 2000.0);
}

TEST(EngineFacadeTest, RawCaCorrectCoversFrozen0084AvoidShiftParameters)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(raw_ca_operation(5, true));
    recipe.operations.push_back(sigmoid_operation());
    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto working = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                          CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.cacorrect")
        {
            operation.enabled = false;
        }
    }
    auto rendered =
        engine.value().render_linear_working(working.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(rendered) << rendered.error().message;
    std::array<std::uint64_t, 3> sums{};
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            sums[channel] += rendered.value().rgb[index + channel];
        }
    }
    EXPECT_NEAR(static_cast<double>(sums[0]), 310081.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 285694.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 268986.0, 2000.0);
}

TEST(EngineFacadeTest, LinearWorkingRenderMatchesDirectRawRender)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    declare_input(recipe);
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto direct = engine.value().render_to_image(request);
    ASSERT_TRUE(direct) << direct.error().message;

    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto linear = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                         CancellationToken{});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_EQ(linear.value().width, 64U);
    EXPECT_EQ(linear.value().height, 48U);
    ASSERT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.highlights")
        {
            operation.enabled = false;
        }
    }
    auto from_working =
        engine.value().render_linear_working(linear.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(from_working) << from_working.error().message;
    EXPECT_EQ(from_working.value().rgb, direct.value().rgb);

    Recipe exposed = rgb_recipe;
    exposed.operations.insert(exposed.operations.begin(), {"ravo.core.exposure",
                                                           1,
                                                           "exposure-1",
                                                           true,
                                                           {{"exposure_ev", ParameterValue{1.0}}},
                                                           std::nullopt});
    auto shifted =
        engine.value().render_linear_working(linear.value(), exposed, CancellationToken{});
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(shifted.value().rgb, from_working.value().rgb);
    EXPECT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
}

TEST(EngineFacadeTest, RgbHistogramMatchesFrozenDisplayBinning)
{
    const auto raster = solid_raster(8, 4, 220, 20, 20);
    auto histogram = collect_rgb_histogram(raster);
    ASSERT_TRUE(histogram) << histogram.error().message;
    EXPECT_EQ(histogram.value().red[220], 32U);
    EXPECT_EQ(histogram.value().green[20], 32U);
    EXPECT_EQ(histogram.value().blue[20], 32U);
    EXPECT_EQ(histogram.value().red[0], 0U);
    EXPECT_EQ(histogram.value().max_count, 32U);
    EXPECT_GT(
        histogram.value()
            .luma[static_cast<std::size_t>(std::lround(0.2126 * 220 + 0.7152 * 20 + 0.0722 * 20))],
        0U);

    RasterBuffer empty;
    auto rejected = collect_rgb_histogram(empty);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
}

TEST(EngineFacadeTest, RgbParadePlacesFullWhiteAtEightNinths)
{
    const auto raster = solid_raster(12, 8, 255, 255, 255);
    auto parade = collect_rgb_parade(raster);
    ASSERT_TRUE(parade) << parade.error().message;
    EXPECT_EQ(parade.value().tones, kWaveformTones);
    ASSERT_GT(parade.value().bins, 0U);
    const std::uint32_t width = parade.value().bins * 3U;
    const std::uint32_t height = parade.value().tones;
    ASSERT_EQ(parade.value().rgb.size(), static_cast<std::size_t>(width) * height * 3U);
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y, const std::uint32_t ch)
    { return parade.value().rgb[(static_cast<std::size_t>(y) * width + x) * 3U + ch]; };
    // Frozen waveform maps 1.0 to 8/9 of the tone axis: ceil((8/9)*159) = 142.
    constexpr std::uint32_t kTone = 142;
    const std::uint32_t y = kWaveformTones - 1U - kTone;
    EXPECT_GT(sample(0, y, 0), 0U);
    EXPECT_EQ(sample(0, y, 1), 0U);
    EXPECT_EQ(sample(0, height - 1U, 0), 0U);
}

TEST(EngineFacadeTest, WaveformOverlaysChannelsAtFrozenToneAndSplitOwnsBothHalves)
{
    const auto raster = solid_raster(12, 8, 255, 255, 255);
    auto waveform = collect_rgb_waveform(raster);
    ASSERT_TRUE(waveform) << waveform.error().message;
    ASSERT_EQ(waveform.value().height, kWaveformTones);
    ASSERT_GT(waveform.value().width, 0U);
    constexpr std::uint32_t kTone = 142U;
    const std::uint32_t y = kWaveformTones - 1U - kTone;
    const std::size_t pixel = static_cast<std::size_t>(y) * waveform.value().width * 3U;
    EXPECT_GT(waveform.value().rgb[pixel], 0U);
    EXPECT_GT(waveform.value().rgb[pixel + 1U], 0U);
    EXPECT_GT(waveform.value().rgb[pixel + 2U], 0U);

    auto split = collect_split_scope(raster);
    ASSERT_TRUE(split) << split.error().message;
    EXPECT_EQ(split.value().height, kWaveformTones);
    EXPECT_EQ(split.value().width, waveform.value().width + kWaveformTones);
    ASSERT_EQ(split.value().rgb.size(),
              static_cast<std::size_t>(split.value().width) * split.value().height * 3U);
    const std::size_t split_pixel = static_cast<std::size_t>(y) * split.value().width * 3U;
    EXPECT_EQ(split.value().rgb[split_pixel], waveform.value().rgb[pixel]);
    bool right_has_signal = false;
    for (std::uint32_t row = 0U; row < split.value().height; ++row)
        for (std::uint32_t column = waveform.value().width; column < split.value().width; ++column)
            right_has_signal =
                right_has_signal ||
                split.value()
                        .rgb[(static_cast<std::size_t>(row) * split.value().width + column) * 3U] !=
                    0U;
    EXPECT_TRUE(right_has_signal);
}

TEST(EngineFacadeTest, D50UvVectorscopeCentersNeutralAndSeparatesSaturatedRed)
{
    const auto neutral = solid_raster(8, 8, 128, 128, 128);
    auto neutral_scope = collect_uv_vectorscope(neutral);
    ASSERT_TRUE(neutral_scope) << neutral_scope.error().message;
    EXPECT_EQ(neutral_scope.value().width, kVectorscopeDiameter);
    EXPECT_EQ(neutral_scope.value().height, kVectorscopeDiameter);
    const auto intensity =
        [](const RgbScopeImage &scope, const std::uint32_t x, const std::uint32_t y)
    { return scope.rgb[(static_cast<std::size_t>(y) * scope.width + x) * 3U]; };
    const std::uint32_t center = (kVectorscopeDiameter - 1U) / 2U;
    std::uint8_t neutral_peak = 0U;
    for (std::uint32_t y = center - 1U; y <= center + 1U; ++y)
        for (std::uint32_t x = center - 1U; x <= center + 1U; ++x)
            neutral_peak = std::max(neutral_peak, intensity(neutral_scope.value(), x, y));
    EXPECT_GT(neutral_peak, 0U);

    const auto red = solid_raster(8, 8, 255, 0, 0);
    auto red_scope = collect_uv_vectorscope(red);
    ASSERT_TRUE(red_scope) << red_scope.error().message;
    std::size_t peak_index = 0U;
    for (std::size_t pixel = 1U;
         pixel < static_cast<std::size_t>(red_scope.value().width) * red_scope.value().height;
         ++pixel)
    {
        if (red_scope.value().rgb[pixel * 3U] > red_scope.value().rgb[peak_index * 3U])
            peak_index = pixel;
    }
    const std::uint32_t peak_x = static_cast<std::uint32_t>(peak_index % red_scope.value().width);
    const std::uint32_t peak_y = static_cast<std::uint32_t>(peak_index / red_scope.value().width);
    EXPECT_GT(red_scope.value().rgb[peak_index * 3U], 0U);
    EXPECT_GT(std::abs(static_cast<int>(peak_x) - static_cast<int>(center)) +
                  std::abs(static_cast<int>(peak_y) - static_cast<int>(center)),
              20);

    RasterBuffer oversized = neutral;
    oversized.srgb.push_back(0U);
    const auto rejected = collect_uv_vectorscope(oversized);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
}

TEST(EngineFacadeTest, ToneCurveMapsSyntheticRasterAndAcceptsLab)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    auto identity = render_op(engine.value(), gray,
                              {"ravo.core.tonecurve",
                               1,
                               "curve-identity",
                               true,
                               {{"working_space", ParameterValue{"rgb"}},
                                {"interpolation", ParameterValue{"monotone_hermite"}},
                                {"channel_mode", ParameterValue{"rgb"}},
                                {"preserve_colors", ParameterValue{"average"}},
                                {"points", tone_curve_points({{0.0, 0.0}, {1.0, 1.0}})}},
                               std::nullopt});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb[0], 128);

    const auto lifted_points = tone_curve_points({{0.0, 0.0}, {128.0 / 255.0, 0.75}, {1.0, 1.0}});
    auto rgb = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-rgb",
                          true,
                          {{"working_space", ParameterValue{"rgb"}},
                           {"interpolation", ParameterValue{"monotone_hermite"}},
                           {"channel_mode", ParameterValue{"rgb"}},
                           {"preserve_colors", ParameterValue{"average"}},
                           {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_GT(rgb.value().rgb[0], 128);

    auto lab = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-lab",
                          true,
                          {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(lab) << lab.error().message;
    EXPECT_GT(lab.value().rgb[0], 128);

    const auto red = solid_raster(8, 8, 220, 40, 30);
    auto rgb_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-rgb-red",
                              true,
                              {{"working_space", ParameterValue{"rgb"}},
                               {"preserve_colors", ParameterValue{"average"}},
                               {"points", lifted_points}},
                              std::nullopt});
    auto lab_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-lab-red",
                              true,
                              {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                              std::nullopt});
    ASSERT_TRUE(rgb_red) << rgb_red.error().message;
    ASSERT_TRUE(lab_red) << lab_red.error().message;
    EXPECT_NE(lab_red.value().rgb, rgb_red.value().rgb);
}

TEST(EngineFacadeTest, RgbLevelsMatchesLeftoverLutAndLinkedPreserve)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    auto identity = render_op(engine.value(), gray,
                              {"ravo.color.rgblevels", 1, "levels-identity", true,
                               rgb_levels_to_parameters(RgbLevelsParams{}), std::nullopt});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb[0], 128);
    EXPECT_EQ(identity.value().rgb[1], 128);
    EXPECT_EQ(identity.value().rgb[2], 128);

    RgbLevelsParams lifted;
    lifted.mode = std::string(kRgbLevelsModeLinked);
    lifted.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    lifted.levels[0] = {0.0, 0.25, 1.0};
    auto brighter = render_op(engine.value(), gray,
                              {"ravo.color.rgblevels", 1, "levels-lift", true,
                               rgb_levels_to_parameters(lifted), std::nullopt});
    ASSERT_TRUE(brighter) << brighter.error().message;
    EXPECT_GT(brighter.value().rgb[0], 128);

    RgbLevelsParams clipped;
    clipped.mode = std::string(kRgbLevelsModeLinked);
    clipped.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    clipped.levels[0] = {0.6, 0.8, 1.0};
    auto black = render_op(engine.value(), gray,
                           {"ravo.color.rgblevels", 1, "levels-clip", true,
                            rgb_levels_to_parameters(clipped), std::nullopt});
    ASSERT_TRUE(black) << black.error().message;
    EXPECT_EQ(black.value().rgb[0], 0);
    EXPECT_EQ(black.value().rgb[1], 0);
    EXPECT_EQ(black.value().rgb[2], 0);

    const auto red = solid_raster(8, 8, 220, 40, 30);
    RgbLevelsParams independent;
    independent.mode = std::string(kRgbLevelsModeIndependent);
    independent.levels[0] = {0.0, 0.25, 1.0};
    independent.levels[1] = {0.0, 0.5, 1.0};
    independent.levels[2] = {0.0, 0.5, 1.0};
    auto red_only = render_op(engine.value(), red,
                              {"ravo.color.rgblevels", 1, "levels-indep", true,
                               rgb_levels_to_parameters(independent), std::nullopt});
    ASSERT_TRUE(red_only) << red_only.error().message;
    EXPECT_GT(red_only.value().rgb[0], 220);
    EXPECT_EQ(red_only.value().rgb[1], 40);
    EXPECT_EQ(red_only.value().rgb[2], 30);

    RgbLevelsParams preserve;
    preserve.mode = std::string(kRgbLevelsModeLinked);
    preserve.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
    preserve.levels[0] = {0.0, 0.25, 1.0};
    auto preserved = render_op(engine.value(), red,
                               {"ravo.color.rgblevels", 1, "levels-preserve", true,
                                rgb_levels_to_parameters(preserve), std::nullopt});
    auto unpreserved = render_op(engine.value(), red,
                                 {"ravo.color.rgblevels", 1, "levels-none", true,
                                  rgb_levels_to_parameters(lifted), std::nullopt});
    ASSERT_TRUE(preserved) << preserved.error().message;
    ASSERT_TRUE(unpreserved) << unpreserved.error().message;
    EXPECT_NE(preserved.value().rgb, unpreserved.value().rgb);

    auto inverted = render_op(engine.value(), gray,
                              {"ravo.color.rgblevels",
                               1,
                               "levels-bad",
                               true,
                               {{"black", ParameterValue{0.8}}, {"white", ParameterValue{0.2}}},
                               std::nullopt});
    ASSERT_FALSE(inverted);
    EXPECT_EQ(inverted.error().code, ErrorCode::kValidation);
}

TEST(EngineFacadeTest, RgbCurveMatchesHermiteLutAndIndependentChannels)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    RgbCurveParams identity;
    auto identity_render = render_op(engine.value(), gray,
                                     {"ravo.color.rgbcurve", 1, "curve-identity", true,
                                      rgb_curve_to_parameters(identity), std::nullopt});
    ASSERT_TRUE(identity_render) << identity_render.error().message;
    EXPECT_EQ(identity_render.value().rgb[0], 128);

    RgbCurveParams lifted;
    lifted.mode = std::string(kRgbLevelsModeLinked);
    lifted.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    lifted.channels[0] = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    auto brighter = render_op(engine.value(), gray,
                              {"ravo.color.rgbcurve", 1, "curve-lift", true,
                               rgb_curve_to_parameters(lifted), std::nullopt});
    ASSERT_TRUE(brighter) << brighter.error().message;
    EXPECT_GT(brighter.value().rgb[0], 128);

    const auto red = solid_raster(8, 8, 220, 40, 30);
    RgbCurveParams independent;
    independent.mode = std::string(kRgbLevelsModeIndependent);
    independent.channels[0] = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    auto red_only = render_op(engine.value(), red,
                              {"ravo.color.rgbcurve", 1, "curve-indep", true,
                               rgb_curve_to_parameters(independent), std::nullopt});
    ASSERT_TRUE(red_only) << red_only.error().message;
    EXPECT_GT(red_only.value().rgb[0], 220);
    EXPECT_EQ(red_only.value().rgb[1], 40);
    EXPECT_EQ(red_only.value().rgb[2], 30);

    RgbCurveParams compensated = lifted;
    compensated.compensate_middle_grey = true;
    auto with_grey = render_op(engine.value(), gray,
                               {"ravo.color.rgbcurve", 1, "curve-grey", true,
                                rgb_curve_to_parameters(compensated), std::nullopt});
    ASSERT_TRUE(with_grey) << with_grey.error().message;
    EXPECT_NE(with_grey.value().rgb, brighter.value().rgb);

    RgbCurveParams cubic;
    cubic.mode = std::string(kRgbLevelsModeLinked);
    cubic.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    cubic.interpolation = std::string(kToneCurveInterpolationCubicSpline);
    cubic.channels[0] = {{0.0, 0.0}, {0.25, 0.8}, {1.0, 1.0}};
    RgbCurveParams hermite_steep = cubic;
    hermite_steep.interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    auto cubic_render = render_op(engine.value(), gray,
                                  {"ravo.color.rgbcurve", 1, "curve-cubic", true,
                                   rgb_curve_to_parameters(cubic), std::nullopt});
    auto hermite_steep_render = render_op(engine.value(), gray,
                                          {"ravo.color.rgbcurve", 1, "curve-hermite-steep", true,
                                           rgb_curve_to_parameters(hermite_steep), std::nullopt});
    ASSERT_TRUE(cubic_render) << cubic_render.error().message;
    ASSERT_TRUE(hermite_steep_render) << hermite_steep_render.error().message;
    EXPECT_NE(cubic_render.value().rgb, hermite_steep_render.value().rgb);

    RgbCurveParams parametric;
    parametric.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    parametric.parametric_shadows = 0.6;
    auto lifted_shadows = render_op(engine.value(), solid_raster(8, 8, 32, 32, 32),
                                    {"ravo.color.rgbcurve", 1, "curve-parametric", true,
                                     rgb_curve_to_parameters(parametric), std::nullopt});
    ASSERT_TRUE(lifted_shadows) << lifted_shadows.error().message;
    EXPECT_GT(lifted_shadows.value().rgb[0], 32);
}

TEST(EngineFacadeTest, UnknownCpuOperationFailsFast)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto rendered = render_op(engine.value(), solid_raster(4, 4, 10, 20, 30),
                              {"ravo.creative.unknown", 1, "x", true, {}, std::nullopt});
    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kUnsupported);
}

TEST(EngineFacadeTest, InteractivePrefixCacheIsExactAndPublishesOnlyCompletedGenerations)
{
    auto created = EngineFacade::create_phase1();
    ASSERT_TRUE(created) << created.error().message;
    const auto raster = solid_raster(96U, 64U, 92U, 126U, 171U);

    DevelopParams develop;
    develop.denoise = 0.31;
    develop.denoise_chroma = 0.17;
    develop.denoise_radius = 1.2;
    develop.exposure_ev = 0.4;
    develop.sigmoid_enabled = true;
    auto recipe =
        recipe_from_develop({"interactive", "memory:interactive", "generation-a"}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto working =
        created.value().linear_working_from_raster(raster, recipe.value(), CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;

    InteractivePreviewRenderCache cache;
    auto first = created.value().render_interactive_linear_working(working.value(), recipe.value(),
                                                                   cache, CancellationToken{});
    auto first_reference =
        created.value().render_linear_working(working.value(), recipe.value(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first_reference) << first_reference.error().message;
    EXPECT_EQ(first.value().rgb, first_reference.value().rgb);
    ASSERT_TRUE(cache.populated());
    const auto first_generation = cache.generation();
    ASSERT_GT(first_generation, 0U);

    develop.exposure_ev = -0.7;
    auto exposed = recipe_from_develop(recipe.value().asset, develop);
    ASSERT_TRUE(exposed) << exposed.error().message;
    auto second = created.value().render_interactive_linear_working(
        working.value(), exposed.value(), cache, CancellationToken{});
    auto second_reference = created.value().render_linear_working(working.value(), exposed.value(),
                                                                  CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(second_reference) << second_reference.error().message;
    EXPECT_EQ(second.value().rgb, second_reference.value().rgb);
    EXPECT_EQ(cache.generation(), first_generation);

    develop.denoise = 0.48;
    auto changed_prefix = recipe_from_develop(recipe.value().asset, develop);
    ASSERT_TRUE(changed_prefix) << changed_prefix.error().message;
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("cancel-prefix-rebuild"));
    auto rejected = created.value().render_interactive_linear_working(
        working.value(), changed_prefix.value(), cache, cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(cache.generation(), first_generation);

    auto rebuilt = created.value().render_interactive_linear_working(
        working.value(), changed_prefix.value(), cache, CancellationToken{});
    auto rebuilt_reference = created.value().render_linear_working(
        working.value(), changed_prefix.value(), CancellationToken{});
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    ASSERT_TRUE(rebuilt_reference) << rebuilt_reference.error().message;
    EXPECT_EQ(rebuilt.value().rgb, rebuilt_reference.value().rgb);
    EXPECT_GT(cache.generation(), first_generation);
}

} // namespace
} // namespace ravo
