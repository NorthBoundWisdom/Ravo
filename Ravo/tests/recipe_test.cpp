#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"

#include "color_balance_fixture.h"
#include "temperature_fixture.h"
#include "test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] OperationInstance *operation_by_id(Recipe &recipe, const std::string_view id)
{
    const auto found =
        std::find_if(recipe.operations.begin(), recipe.operations.end(),
                     [id](const OperationInstance &operation) { return operation.id == id; });
    return found == recipe.operations.end() ? nullptr : &*found;
}

TEST(RecipeTest, CanonicalRoundTripValidatesAgainstThePhaseOneRegistry)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;

    const auto serialized = serialize_recipe(test::valid_recipe());
    ASSERT_TRUE(serialized) << serialized.error().message;
    EXPECT_EQ(
        serialized.value(),
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[{"enabled":true,"id":"ravo.color.input","instance_id":"color-input-1","parameters":{"blue_mapping":false,"gamut_normalize":"off","input_profile":"source","input_profile_filename":"","rendering_intent":"perceptual","working_profile":"linear_rec709","working_profile_filename":""},"schema_version":1},{"enabled":true,"id":"ravo.core.exposure","instance_id":"exposure-1","parameters":{"exposure_ev":1.25},"schema_version":1},{"enabled":true,"id":"ravo.color.output","instance_id":"color-output-1","parameters":{"black_point_compensation":true,"output_profile":"srgb","output_profile_filename":"","proof_intent":"relative_colorimetric","proof_mode":"off","proof_profile":"srgb","proof_profile_filename":"","rendering_intent":"perceptual"},"schema_version":1}],"schema_version":3})");

    const auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto valid = validate_recipe(parsed.value(), registry.value());
    EXPECT_TRUE(valid) << valid.error().message;
}

TEST(RecipeTest, OlderSchemasUpgradeToExplicitColorBoundaries)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    auto upgraded = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[{"enabled":true,"id":"ravo.core.exposure","instance_id":"exposure-1","parameters":{"exposure_ev":0.5},"schema_version":1}],"schema_version":1})");
    ASSERT_TRUE(upgraded) << upgraded.error().message;
    EXPECT_EQ(upgraded.value().schema_version, 3);
    ASSERT_EQ(upgraded.value().operations.size(), 3U);
    EXPECT_EQ(upgraded.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(upgraded.value().operations[1].id, "ravo.core.exposure");
    EXPECT_EQ(upgraded.value().operations.back().id, "ravo.color.output");
    ASSERT_TRUE(validate_recipe(upgraded.value(), registry.value()));

    auto reserved = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[{"enabled":true,"id":"ravo.color.input","instance_id":"old-input","parameters":{},"schema_version":1}],"schema_version":1})");
    ASSERT_TRUE(reserved) << reserved.error().message;
    ASSERT_EQ(reserved.value().operations.size(), 2U);
    auto reserved_input =
        input_color_from_parameters(reserved.value().operations.front().parameters);
    ASSERT_TRUE(reserved_input) << reserved_input.error().message;
    EXPECT_EQ(reserved_input.value(), InputColorParams{});
    EXPECT_EQ(reserved.value().operations.back().id, "ravo.color.output");
    ASSERT_TRUE(validate_recipe(reserved.value(), registry.value()));
}

TEST(RecipeTest, RejectsUnknownFieldsRatherThanGuessingCompatibility)
{
    const auto recipe = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[],"schema_version":1,"unexpected":true})");

    ASSERT_FALSE(recipe);
    EXPECT_EQ(recipe.error().code, ErrorCode::kValidation);
    EXPECT_EQ(recipe.error().context.at("path"), "recipe.unexpected");
}

TEST(RecipeTest, ReportsUnknownOperationsAsUnsupported)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    auto recipe = test::valid_recipe();
    auto *exposure = operation_by_id(recipe, "ravo.core.exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->id = "ravo.creative.unknown";

    const auto valid = validate_recipe(recipe, registry.value());
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(valid.error().context.at("operation_id"), "ravo.creative.unknown");
}

TEST(RecipeTest, EnforcesExposureParameterRange)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    auto recipe = test::valid_recipe();
    auto *exposure = operation_by_id(recipe, "ravo.core.exposure");
    ASSERT_NE(exposure, nullptr);
    exposure->parameters["exposure_ev"] = ParameterValue{11.0};

    const auto valid = validate_recipe(recipe, registry.value());
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(valid.error().context.at("parameter"), "exposure_ev");
}

TEST(RecipeTest, StrictDevelopFieldAssignmentRejectsInsteadOfClamping)
{
    DevelopParams params;
    auto rejected = apply_develop_field_strict(params, "exposure", 11.0);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
    EXPECT_DOUBLE_EQ(params.exposure_ev, 0.0);

    auto unknown = apply_develop_field_strict(params, "notADevelopField", 1.0);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::kInvalidArgument);

    auto accepted = apply_develop_field_strict(params, "exposure", -1.0);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_DOUBLE_EQ(params.exposure_ev, -1.0);
}

TEST(RecipeTest, DevelopParamsRoundTripThroughCanonicalRecipe)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    DevelopParams params;
    params.temperature = test::temperature_0000_params();
    params.exposure_ev = -0.3;
    params.contrast = 0.2;
    params.rotate_quarters = 1;
    params.crop_width = 0.8;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto valid = validate_recipe(recipe.value(), registry.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().temperature, params.temperature);
    EXPECT_NEAR(restored.value().exposure_ev, -0.3, 1e-6);
    EXPECT_EQ(restored.value().rotate_quarters, 1);
    EXPECT_NEAR(restored.value().crop_width, 0.8, 1e-6);
    EXPECT_FALSE(restored.value().is_identity());
    EXPECT_TRUE(DevelopParams{}.is_identity());
}

TEST(RecipeTest, PrimariesUseCanonicalRadiansAndFollowInputBeforeOutput)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kPrimariesOperationId);
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(descriptor->parameters.size(), 8U);
    EXPECT_EQ(descriptor->parameters[0].name, "achromatic_tint_hue");
    EXPECT_EQ(descriptor->parameters[1].name, "achromatic_tint_purity");
    EXPECT_EQ(descriptor->parameters[2].name, "red_hue");
    EXPECT_EQ(descriptor->parameters[3].name, "red_purity");
    EXPECT_EQ(descriptor->parameters[4].name, "green_hue");
    EXPECT_EQ(descriptor->parameters[5].name, "green_purity");
    EXPECT_EQ(descriptor->parameters[6].name, "blue_hue");
    EXPECT_EQ(descriptor->parameters[7].name, "blue_purity");
    EXPECT_EQ(descriptor->parameters[0].minimum, kPrimariesHueMin);
    EXPECT_EQ(descriptor->parameters[0].maximum, kPrimariesHueMax);
    EXPECT_EQ(descriptor->parameters[1].minimum, kPrimariesAchromaticTintPurityMin);
    EXPECT_EQ(descriptor->parameters[1].maximum, kPrimariesAchromaticTintPurityMax);
    EXPECT_EQ(descriptor->parameters[3].minimum, kPrimariesPrimaryPurityMin);
    EXPECT_EQ(descriptor->parameters[3].maximum, kPrimariesPrimaryPurityMax);

    DevelopParams params;
    ASSERT_TRUE(apply_develop_field(params, "primariesAchromaticHueDegrees", -30.0));
    ASSERT_TRUE(apply_develop_field(params, "primariesAchromaticPurity", 0.25));
    ASSERT_TRUE(apply_develop_field(params, "primariesRedHueDegrees", 15.0));
    ASSERT_TRUE(apply_develop_field(params, "primariesRedPurity", 1.2));
    ASSERT_TRUE(apply_develop_field(params, "primariesGreenHueDegrees", -45.0));
    ASSERT_TRUE(apply_develop_field(params, "primariesGreenPurity", 0.8));
    ASSERT_TRUE(apply_develop_field(params, "primariesBlueHueDegrees", 90.0));
    ASSERT_TRUE(apply_develop_field(params, "primariesBluePurity", 1.5));
    EXPECT_NEAR(params.primaries.achromatic_tint_hue, -std::numbers::pi / 6.0, 1e-12);
    EXPECT_NEAR(params.primaries.red_hue, std::numbers::pi / 12.0, 1e-12);
    EXPECT_NEAR(params.primaries.green_hue, -std::numbers::pi / 4.0, 1e-12);
    EXPECT_NEAR(params.primaries.blue_hue, std::numbers::pi / 2.0, 1e-12);
    EXPECT_FALSE(params.primaries.is_identity());

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().operations.size(), 3U);
    EXPECT_EQ(recipe.value().operations[0].id, "ravo.color.input");
    EXPECT_EQ(recipe.value().operations[1].id, kPrimariesOperationId);
    EXPECT_EQ(recipe.value().operations.back().id, "ravo.color.output");
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));

    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().primaries, params.primaries);

    auto missing = recipe.value();
    auto *missing_primaries = operation_by_id(missing, kPrimariesOperationId);
    ASSERT_NE(missing_primaries, nullptr);
    missing_primaries->parameters.erase("blue_purity");
    const auto missing_valid = validate_recipe(missing, registry.value());
    ASSERT_FALSE(missing_valid);
    EXPECT_EQ(missing_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(missing_valid.error().context.at("parameter"), "blue_purity");

    auto unknown = recipe.value();
    auto *unknown_primaries = operation_by_id(unknown, kPrimariesOperationId);
    ASSERT_NE(unknown_primaries, nullptr);
    unknown_primaries->parameters.emplace("unrecognized", ParameterValue{0.0});
    const auto unknown_valid = validate_recipe(unknown, registry.value());
    ASSERT_FALSE(unknown_valid);
    EXPECT_EQ(unknown_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(unknown_valid.error().context.at("parameter"), "unrecognized");

    auto out_of_range = recipe.value();
    auto *out_of_range_primaries = operation_by_id(out_of_range, kPrimariesOperationId);
    ASSERT_NE(out_of_range_primaries, nullptr);
    out_of_range_primaries->parameters["red_purity"] = ParameterValue{6.0};
    const auto out_of_range_valid = validate_recipe(out_of_range, registry.value());
    ASSERT_FALSE(out_of_range_valid);
    EXPECT_EQ(out_of_range_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(out_of_range_valid.error().context.at("parameter"), "red_purity");

    auto nonfinite = recipe.value();
    auto *nonfinite_primaries = operation_by_id(nonfinite, kPrimariesOperationId);
    ASSERT_NE(nonfinite_primaries, nullptr);
    nonfinite_primaries->parameters["blue_hue"] =
        ParameterValue{std::numeric_limits<double>::infinity()};
    const auto nonfinite_valid = validate_recipe(nonfinite, registry.value());
    ASSERT_FALSE(nonfinite_valid);
    EXPECT_EQ(nonfinite_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(nonfinite_valid.error().context.at("parameter"), "blue_hue");

    auto duplicate = recipe.value();
    auto duplicate_primaries = duplicate.operations[1];
    duplicate_primaries.instance_id = "primaries-2";
    duplicate.operations.insert(duplicate.operations.end() - 1, std::move(duplicate_primaries));
    const auto duplicate_valid = validate_recipe(duplicate, registry.value());
    ASSERT_FALSE(duplicate_valid);
    EXPECT_EQ(duplicate_valid.error().code, ErrorCode::kConflict);

    auto output_not_last = recipe.value();
    output_not_last.operations.push_back({"ravo.core.exposure",
                                          1,
                                          "exposure-after-output",
                                          true,
                                          {{"exposure_ev", ParameterValue{0.5}}},
                                          std::nullopt});
    const auto output_not_last_valid = validate_recipe(output_not_last, registry.value());
    ASSERT_FALSE(output_not_last_valid);
    EXPECT_EQ(output_not_last_valid.error().code, ErrorCode::kValidation);

    EXPECT_TRUE(reset_develop_field(params, "primariesRedHueDegrees"));
    EXPECT_DOUBLE_EQ(params.primaries.red_hue, 0.0);
    EXPECT_TRUE(reset_develop_section(params, "primaries"));
    EXPECT_EQ(params.primaries, PrimariesParams{});
    EXPECT_FALSE(apply_develop_field(params, "primariesBluePurity",
                                     std::numeric_limits<double>::quiet_NaN()));

    auto identity =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, DevelopParams{});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().operations.size(), 2U);
    EXPECT_EQ(identity.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(identity.value().operations.back().id, "ravo.color.output");
}

TEST(RecipeTest, ExtraDevelopOpsRoundTripAndCropAspect)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_EQ(registry.value().descriptors().size(), kPhase1OperationCount);
    DevelopParams params;
    params.sharpen = 0.4;
    params.vignette = 0.5;
    params.velvia = 0.3;
    params.flip_horizontal = 1;
    params.gamma = 1.2;
    params.split_amount = 0.4;
    params.raw_highlights = 0.7;
    params.hot_pixels_strength = 0.25;
    params.hot_pixels_threshold = 0.04;
    params.hot_pixels_permissive = true;
    params.raw_ca_iterations = 5;
    params.raw_ca_avoid_shift = true;
    params.denoise = 0.4;
    params.lens_k1 = -0.12;
    params.tone_eq_shadows = 0.35;
    params.graduated_density = 0.8;
    params.color_eq_sat[2] = 0.4;
    params.channel_mixer.red = {0.9, 0.1, 0.0};
    params.channel_mixer.green = {0.05, 0.9, 0.05};
    params.channel_mixer.blue = {0.0, 0.15, 0.85};
    params.channel_mixer.normalize_red = true;
    params.channel_mixer.adaptation = std::string(kChannelMixerAdaptationCat16);
    params.channel_mixer.illuminant_x = 0.3819674253463745;
    params.channel_mixer.illuminant_y = 0.36998802423477173;
    params.channel_mixer.gamut = 1.0;
    params.channel_mixer.clip = true;
    params.color_balance_rgb.global_y = 0.12;
    params.color_balance_rgb.shadows_chroma = 0.08;
    params.color_balance_rgb.shadows_hue = 215.0;
    params.color_balance_rgb.saturation_formula = std::string(kColorBalanceRgbFormulaJzAzBz2021);
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *sharpen = operation_by_id(recipe.value(), "ravo.detail.sharpen");
    ASSERT_NE(sharpen, nullptr);
    ASSERT_TRUE(sharpen->parameters.contains("threshold"));
    EXPECT_DOUBLE_EQ(std::get<double>(sharpen->parameters.at("threshold").value), 0.5);
    const auto *vignette = operation_by_id(recipe.value(), "ravo.effect.vignette");
    ASSERT_NE(vignette, nullptr);
    ASSERT_TRUE(vignette->parameters.contains("midpoint"));
    EXPECT_DOUBLE_EQ(std::get<double>(vignette->parameters.at("midpoint").value), 0.8);
    const auto valid = validate_recipe(recipe.value(), registry.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_NEAR(restored.value().sharpen, 0.4, 1e-6);
    EXPECT_NEAR(restored.value().vignette, 0.5, 1e-6);
    EXPECT_EQ(restored.value().flip_horizontal, 1);
    EXPECT_NEAR(restored.value().raw_highlights, 0.7, 1e-6);
    EXPECT_NEAR(restored.value().hot_pixels_strength, 0.25, 1e-6);
    EXPECT_NEAR(restored.value().hot_pixels_threshold, 0.04, 1e-6);
    EXPECT_TRUE(restored.value().hot_pixels_permissive);
    EXPECT_EQ(restored.value().raw_ca_iterations, 5);
    EXPECT_TRUE(restored.value().raw_ca_avoid_shift);
    EXPECT_NEAR(restored.value().denoise, 0.4, 1e-6);
    EXPECT_NEAR(restored.value().lens_k1, -0.12, 1e-6);
    EXPECT_NEAR(restored.value().tone_eq_shadows, 0.35, 1e-6);
    EXPECT_NEAR(restored.value().graduated_density, 0.8, 1e-6);
    EXPECT_NEAR(restored.value().color_eq_sat[2], 0.4, 1e-6);
    EXPECT_NEAR(restored.value().gamma, 1.2, 1e-6);
    EXPECT_EQ(restored.value().channel_mixer, params.channel_mixer);
    EXPECT_EQ(restored.value().color_balance_rgb, params.color_balance_rgb);

    DevelopParams crop;
    ASSERT_TRUE(apply_crop_aspect(crop, "3:2"));
    EXPECT_NEAR(crop.crop_width / crop.crop_height, 1.5, 1e-6);
    EXPECT_TRUE(apply_develop_field(crop, "exposure", -0.25));
    EXPECT_NEAR(crop.exposure_ev, -0.25, 1e-6);
    EXPECT_TRUE(reset_develop_field(crop, "exposure"));
    EXPECT_NEAR(crop.exposure_ev, 0.0, 1e-6);
    EXPECT_TRUE(reset_develop_section(crop, "geometry"));
    EXPECT_NEAR(crop.crop_width, 1.0, 1e-6);
    DevelopParams bands;
    bands.tone_eq_shadows = 0.4;
    bands.graduated_density = 0.8;
    bands.color_eq_band = 3;
    EXPECT_TRUE(reset_develop_section(bands, "toneEqual"));
    EXPECT_NEAR(bands.tone_eq_shadows, 0.0, 1e-6);
    EXPECT_NEAR(bands.graduated_density, 0.8, 1e-6);
    EXPECT_TRUE(reset_develop_section(bands, "graduated"));
    EXPECT_NEAR(bands.graduated_density, 0.0, 1e-6);
    EXPECT_EQ(bands.color_eq_band, 0);
    EXPECT_TRUE(apply_develop_field(bands, "channelMixerRG", 0.25));
    EXPECT_NEAR(bands.channel_mixer.red[1], 0.25, 1e-6);
    EXPECT_TRUE(reset_develop_section(bands, "calibration"));
    EXPECT_TRUE(bands.channel_mixer.is_identity());
    EXPECT_TRUE(apply_develop_field(bands, "whiteBalanceRed", 2.25));
    ASSERT_TRUE(bands.temperature.coefficients);
    EXPECT_EQ(bands.temperature.mode, kTemperatureModeManual);
    EXPECT_NEAR((*bands.temperature.coefficients)[0], 2.25, 1e-6);
    EXPECT_TRUE(reset_develop_section(bands, "whiteBalance"));
    EXPECT_TRUE(bands.temperature.is_identity());
    EXPECT_TRUE(apply_develop_field(bands, "colorBalanceGlobalY", 0.25));
    EXPECT_NEAR(bands.color_balance_rgb.global_y, 0.25, 1e-6);
    EXPECT_TRUE(apply_develop_field(bands, "colorBalanceFormula", 1.0));
    EXPECT_EQ(bands.color_balance_rgb.saturation_formula, kColorBalanceRgbFormulaJzAzBz2021);
    EXPECT_TRUE(reset_develop_field(bands, "colorBalanceGlobalY"));
    EXPECT_NEAR(bands.color_balance_rgb.global_y, 0.0, 1e-6);
    EXPECT_TRUE(reset_develop_section(bands, "color"));
    EXPECT_TRUE(bands.color_balance_rgb.is_identity());

    DevelopParams angled;
    angled.straighten_degrees = 12.5;
    angled.crop_x = 0.1;
    angled.crop_y = 0.2;
    angled.crop_width = 0.3;
    angled.crop_height = 0.4;
    auto angled_recipe =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, angled);
    ASSERT_TRUE(angled_recipe) << angled_recipe.error().message;
    ASSERT_TRUE(validate_recipe(angled_recipe.value(), registry.value()))
        << validate_recipe(angled_recipe.value(), registry.value()).error().message;
    auto angled_restored = develop_from_recipe(angled_recipe.value());
    ASSERT_TRUE(angled_restored) << angled_restored.error().message;
    EXPECT_NEAR(angled_restored.value().straighten_degrees, 12.5, 1e-6);

    transform_crop_for_quarter_turns(angled, 1);
    EXPECT_NEAR(angled.crop_x, 0.4, 1e-6);
    EXPECT_NEAR(angled.crop_y, 0.1, 1e-6);
    EXPECT_NEAR(angled.crop_width, 0.4, 1e-6);
    EXPECT_NEAR(angled.crop_height, 0.3, 1e-6);
    transform_crop_for_quarter_turns(angled, 3);
    EXPECT_NEAR(angled.crop_x, 0.1, 1e-6);
    EXPECT_NEAR(angled.crop_y, 0.2, 1e-6);
    EXPECT_NEAR(angled.crop_width, 0.3, 1e-6);
    EXPECT_NEAR(angled.crop_height, 0.4, 1e-6);
    transform_crop_for_flip(angled, true, false);
    EXPECT_NEAR(angled.crop_x, 0.6, 1e-6);
    EXPECT_NEAR(angled.crop_y, 0.2, 1e-6);

    DevelopParams inscribed;
    inscribed.straighten_degrees = 15.0;
    constrain_crop_to_straighten(inscribed, 1.5);
    EXPECT_LT(inscribed.crop_width, 0.98);
    EXPECT_LT(inscribed.crop_height, 0.98);
    EXPECT_NEAR(inscribed.crop_x + inscribed.crop_width * 0.5, 0.5, 1e-6);
    EXPECT_NEAR(inscribed.crop_y + inscribed.crop_height * 0.5, 0.5, 1e-6);
    EXPECT_NEAR(inscribed.crop_width / inscribed.crop_height, 1.0, 1e-6);
    const double previous_width = inscribed.crop_width;
    constrain_crop_to_straighten(inscribed, 1.5);
    EXPECT_NEAR(inscribed.crop_width, previous_width, 1e-6);
}

TEST(RecipeTest, InputColorSchemaRoundTripsAndRejectsEveryUnknownPolicy)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;

    DevelopParams develop;
    develop.input_color.input_profile = std::string(kInputProfileHlgP3);
    develop.input_color.rendering_intent = std::string(kColorIntentRelative);
    develop.input_color.gamut_normalize = std::string(kColorNormalizeLinearRec2020);
    develop.input_color.blue_mapping = true;
    develop.input_color.working_profile = std::string(kInputProfileProPhotoRgb);
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().input_color, develop.input_color);

    const auto reject = [&](const std::string_view parameter, ParameterValue value)
    {
        auto invalid = recipe.value();
        auto *input = operation_by_id(invalid, "ravo.color.input");
        EXPECT_NE(input, nullptr);
        if (input == nullptr)
        {
            return Result<void>{make_error(ErrorCode::kInternal, "missing test operation")};
        }
        input->parameters[std::string(parameter)] = std::move(value);
        return validate_recipe(invalid, registry.value());
    };
    EXPECT_FALSE(reject("input_profile", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("rendering_intent", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("gamut_normalize", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("working_profile", ParameterValue{"hlg_p3"}));
    EXPECT_FALSE(reject("input_profile_filename", ParameterValue{"unexpected.icc"}));

    auto file_params = develop.input_color;
    file_params.input_profile = std::string(kInputProfileFileIcc);
    file_params.input_profile_filename.clear();
    EXPECT_FALSE(validate_input_color_parameters(input_color_to_parameters(file_params)));

    auto duplicate = recipe.value();
    auto *input = operation_by_id(duplicate, "ravo.color.input");
    ASSERT_NE(input, nullptr);
    auto copy = *input;
    copy.instance_id = "color-input-2";
    duplicate.operations.push_back(std::move(copy));
    auto duplicate_result = validate_recipe(duplicate, registry.value());
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);

    DevelopParams selected;
    EXPECT_TRUE(apply_develop_field(selected, "inputProfile", 7.0));
    EXPECT_TRUE(apply_develop_field(selected, "workingProfile", 1.0));
    EXPECT_TRUE(apply_develop_field(selected, "renderingIntent", 3.0));
    EXPECT_TRUE(apply_develop_field(selected, "gamutNormalize", 1.0));
    EXPECT_TRUE(apply_develop_field(selected, "blueMapping", 1.0));
    EXPECT_EQ(selected.input_color.input_profile, kInputProfileDisplayP3);
    EXPECT_EQ(selected.input_color.working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(selected.input_color.rendering_intent, kColorIntentAbsolute);
    EXPECT_EQ(selected.input_color.gamut_normalize, kColorNormalizeSrgb);
    EXPECT_TRUE(selected.input_color.blue_mapping);
    EXPECT_FALSE(selected.is_identity());
}

TEST(RecipeTest, OutputColorSchemaRoundTripsAndRejectsEveryUnknownPolicy)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;

    DevelopParams develop;
    develop.output_color.output_profile = std::string(kInputProfileDisplayP3);
    develop.output_color.rendering_intent = std::string(kColorIntentRelative);
    develop.output_color.proof_mode = std::string(kProofModeGamutCheck);
    develop.output_color.proof_profile = std::string(kInputProfileAdobeRgb);
    develop.output_color.proof_intent = std::string(kColorIntentAbsolute);
    develop.output_color.black_point_compensation = false;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().output_color, develop.output_color);
    EXPECT_FALSE(restored.value().is_identity());

    const auto reject = [&](const std::string_view parameter, ParameterValue value)
    {
        auto invalid = recipe.value();
        auto *output = operation_by_id(invalid, "ravo.color.output");
        EXPECT_NE(output, nullptr);
        if (output == nullptr)
        {
            return Result<void>{make_error(ErrorCode::kInternal, "missing test operation")};
        }
        output->parameters[std::string(parameter)] = std::move(value);
        return validate_recipe(invalid, registry.value());
    };
    EXPECT_FALSE(reject("output_profile", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("rendering_intent", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("proof_mode", ParameterValue{"unknown"}));
    EXPECT_FALSE(reject("proof_profile", ParameterValue{"lab"}));
    EXPECT_FALSE(reject("output_profile_filename", ParameterValue{"unexpected.icc"}));
    auto gamut_lab = develop.output_color;
    gamut_lab.output_profile = std::string(kInputProfileLab);
    EXPECT_FALSE(validate_output_color_parameters(output_color_to_parameters(gamut_lab)));

    auto file_params = develop.output_color;
    file_params.output_profile = std::string(kInputProfileFileIcc);
    file_params.output_profile_filename.clear();
    EXPECT_FALSE(validate_output_color_parameters(output_color_to_parameters(file_params)));

    auto duplicate = recipe.value();
    auto *output = operation_by_id(duplicate, "ravo.color.output");
    ASSERT_NE(output, nullptr);
    auto copy = *output;
    copy.instance_id = "color-output-2";
    duplicate.operations.push_back(std::move(copy));
    auto duplicate_result = validate_recipe(duplicate, registry.value());
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);

    DevelopParams selected;
    EXPECT_TRUE(apply_develop_field(selected, "outputProfile", 10.0));
    EXPECT_TRUE(apply_develop_field(selected, "outputRenderingIntent", 1.0));
    EXPECT_TRUE(apply_develop_field(selected, "proofMode", 2.0));
    EXPECT_TRUE(apply_develop_field(selected, "proofProfile", 1.0));
    EXPECT_TRUE(apply_develop_field(selected, "proofIntent", 3.0));
    EXPECT_TRUE(apply_develop_field(selected, "outputBlackPointCompensation", 0.0));
    EXPECT_EQ(selected.output_color.output_profile, kInputProfileDisplayP3);
    EXPECT_EQ(selected.output_color.rendering_intent, kColorIntentRelative);
    EXPECT_EQ(selected.output_color.proof_mode, kProofModeGamutCheck);
    EXPECT_EQ(selected.output_color.proof_profile, kInputProfileAdobeRgb);
    EXPECT_EQ(selected.output_color.proof_intent, kColorIntentAbsolute);
    EXPECT_FALSE(selected.output_color.black_point_compensation);
    EXPECT_TRUE(reset_develop_section(selected, "outputProfile"));
    EXPECT_TRUE(selected.output_color.is_identity());
}

TEST(RecipeTest, ChannelMixerSchemaRejectsInvalidEnumsArraysAndNormalization)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto validate = [&](std::map<std::string, ParameterValue, std::less<>> parameters)
    {
        Recipe recipe;
        recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
        recipe.operations.push_back({"ravo.color.channelmixerrgb", 1, "calibration-1", true,
                                     std::move(parameters), std::nullopt});
        return validate_recipe(recipe, registry.value());
    };

    auto valid = channel_mixer_to_parameters(ChannelMixerParams{});
    ASSERT_TRUE(validate(valid));

    auto bad_adaptation = valid;
    bad_adaptation["adaptation"] = ParameterValue{"von_kries_guess"};
    auto adaptation_result = validate(std::move(bad_adaptation));
    ASSERT_FALSE(adaptation_result);
    EXPECT_EQ(adaptation_result.error().code, ErrorCode::kValidation);

    auto bad_matrix = valid;
    auto &red = std::get<ParameterValue::Array>(bad_matrix["red"].value);
    red[0] = ParameterValue{2.01};
    auto matrix_result = validate(std::move(bad_matrix));
    ASSERT_FALSE(matrix_result);
    EXPECT_EQ(matrix_result.error().code, ErrorCode::kValidation);

    auto non_finite = valid;
    auto &green = std::get<ParameterValue::Array>(non_finite["green"].value);
    green[1] = ParameterValue{std::numeric_limits<double>::infinity()};
    auto finite_result = validate(std::move(non_finite));
    ASSERT_FALSE(finite_result);
    EXPECT_EQ(finite_result.error().code, ErrorCode::kValidation);

    auto zero_normalized = valid;
    zero_normalized["red"] = ParameterValue{
        ParameterValue::Array{ParameterValue{1.0}, ParameterValue{-1.0}, ParameterValue{0.0}}};
    zero_normalized["normalize_red"] = ParameterValue{true};
    auto normalized_result = validate(std::move(zero_normalized));
    ASSERT_FALSE(normalized_result);
    EXPECT_EQ(normalized_result.error().code, ErrorCode::kValidation);

    auto invalid_xy = valid;
    invalid_xy["illuminant_x"] = ParameterValue{0.7};
    invalid_xy["illuminant_y"] = ParameterValue{0.4};
    auto xy_result = validate(std::move(invalid_xy));
    ASSERT_FALSE(xy_result);
    EXPECT_EQ(xy_result.error().code, ErrorCode::kValidation);
}

TEST(RecipeTest, TemperatureSchemaRoundTripsFixturesAndRejectsHiddenFallbacks)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto operation = [](const TemperatureParams &params)
    {
        return OperationInstance{
            "ravo.color.temperature",          1,           "temperature-1", true,
            temperature_to_parameters(params), std::nullopt};
    };
    const auto validate = [&](TemperatureParams params)
    {
        Recipe recipe;
        recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
        recipe.operations.push_back(operation(params));
        return validate_recipe(recipe, registry.value());
    };

    const auto fixture = test::temperature_0000_params();
    auto parameters = temperature_to_parameters(fixture);
    auto restored = temperature_from_parameters(parameters);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value(), fixture);
    ASSERT_TRUE(validate(fixture));

    auto four_channel = temperature_from_parameters(
        temperature_to_parameters(test::temperature_0177_four_channel_params()));
    ASSERT_TRUE(four_channel) << four_channel.error().message;
    EXPECT_EQ(four_channel.value(), test::temperature_0177_four_channel_params());

    auto unknown = parameters;
    unknown["kelvin_guess"] = ParameterValue{6500.0};
    auto unknown_result = temperature_from_parameters(unknown);
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().context.at("parameter"), "kelvin_guess");

    auto bad_mode = parameters;
    bad_mode["mode"] = ParameterValue{"camera_magic"};
    auto mode_result = temperature_from_parameters(bad_mode);
    ASSERT_FALSE(mode_result);
    EXPECT_EQ(mode_result.error().code, ErrorCode::kValidation);

    auto missing = parameters;
    missing.erase("coefficients");
    auto missing_result = temperature_from_parameters(missing);
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error().context.at("parameter"), "coefficients");

    auto zero = parameters;
    auto &zero_values = std::get<ParameterValue::Array>(zero["coefficients"].value);
    zero_values[0] = ParameterValue{0.0};
    auto zero_result = temperature_from_parameters(zero);
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(zero_result.error().context.at("parameter"), "coefficients");

    auto non_finite = parameters;
    auto &finite_values = std::get<ParameterValue::Array>(non_finite["coefficients"].value);
    finite_values[2] = ParameterValue{std::numeric_limits<double>::infinity()};
    auto finite_result = temperature_from_parameters(non_finite);
    ASSERT_FALSE(finite_result);
    EXPECT_EQ(finite_result.error().context.at("parameter"), "coefficients");

    auto too_many = parameters;
    auto &too_many_values = std::get<ParameterValue::Array>(too_many["coefficients"].value);
    too_many_values.emplace_back(1.0);
    auto count_result = temperature_from_parameters(too_many);
    ASSERT_FALSE(count_result);
    EXPECT_EQ(count_result.error().context.at("parameter"), "coefficients");

    auto boolean = parameters;
    auto &boolean_values = std::get<ParameterValue::Array>(boolean["coefficients"].value);
    boolean_values[1] = ParameterValue{true};
    auto boolean_result = temperature_from_parameters(boolean);
    ASSERT_FALSE(boolean_result);
    EXPECT_EQ(boolean_result.error().context.at("parameter"), "coefficients");

    auto late = test::temperature_0171_late_params();
    auto late_without_cat = validate(late);
    ASSERT_FALSE(late_without_cat);
    EXPECT_EQ(late_without_cat.error().code, ErrorCode::kValidation);

    ChannelMixerParams cat;
    cat.adaptation = std::string(kChannelMixerAdaptationCat16);
    Recipe late_recipe;
    late_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    late_recipe.operations.push_back(operation(late));
    late_recipe.operations.push_back({"ravo.color.channelmixerrgb", 1, "calibration-1", true,
                                      channel_mixer_to_parameters(cat), std::nullopt});
    auto late_valid = validate_recipe(late_recipe, registry.value());
    ASSERT_TRUE(late_valid) << late_valid.error().message;

    Recipe wrong_order = late_recipe;
    std::swap(wrong_order.operations[0], wrong_order.operations[1]);
    auto wrong_order_result = validate_recipe(wrong_order, registry.value());
    ASSERT_FALSE(wrong_order_result);
    EXPECT_EQ(wrong_order_result.error().code, ErrorCode::kValidation);

    Recipe after_cfa;
    after_cfa.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    after_cfa.operations.push_back({"ravo.raw.hotpixels",
                                    1,
                                    "hotpixels-1",
                                    true,
                                    {{"strength", ParameterValue{0.25}},
                                     {"threshold", ParameterValue{0.05}},
                                     {"permissive", ParameterValue{false}}},
                                    std::nullopt});
    after_cfa.operations.push_back(operation(fixture));
    auto after_cfa_result = validate_recipe(after_cfa, registry.value());
    ASSERT_FALSE(after_cfa_result);
    EXPECT_EQ(after_cfa_result.error().code, ErrorCode::kValidation);
}

TEST(RecipeTest, ColorBalanceRgbFullSchemaRoundTripsTheFrozen0083Parameters)
{
    const ColorBalanceRgbParams fixture = test::color_balance_0083_params();

    auto parameters = color_balance_rgb_to_parameters(fixture);
    auto decoded = color_balance_rgb_from_parameters(parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), fixture);

    DevelopParams develop;
    develop.color_balance_rgb = fixture;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().operations.size(), 3U);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.color.input"), nullptr);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.color.colorbalancergb"), nullptr);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.color.output"), nullptr);
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));
    auto json = serialize_recipe(recipe.value());
    ASSERT_TRUE(json) << json.error().message;
    auto parsed = parse_recipe_json(json.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().color_balance_rgb, fixture);
}

TEST(RecipeTest, ColorBalanceRgbSchemaFailsFastOnEveryInvalidPolicyClass)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto validate = [&](std::map<std::string, ParameterValue, std::less<>> parameters)
    {
        Recipe recipe;
        recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
        recipe.operations.push_back({"ravo.color.colorbalancergb", 1, "colorbalancergb-1", true,
                                     std::move(parameters), std::nullopt});
        return validate_recipe(recipe, registry.value());
    };
    const auto valid = color_balance_rgb_to_parameters(ColorBalanceRgbParams{});
    ASSERT_TRUE(validate(valid));

    auto jzazbz = valid;
    jzazbz["saturation_formula"] = ParameterValue{std::string(kColorBalanceRgbFormulaJzAzBz2021)};
    ASSERT_TRUE(validate(jzazbz));

    auto unknown = valid;
    unknown["legacy_picker"] = ParameterValue{0.0};
    auto unknown_result = validate(std::move(unknown));
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().context.at("parameter"), "legacy_picker");

    auto missing = valid;
    missing.erase("global_y");
    auto missing_result = validate(std::move(missing));
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error().context.at("parameter"), "global_y");

    auto non_finite = valid;
    non_finite["contrast"] = ParameterValue{std::numeric_limits<double>::quiet_NaN()};
    auto finite_result = validate(std::move(non_finite));
    ASSERT_FALSE(finite_result);
    EXPECT_EQ(finite_result.error().context.at("parameter"), "contrast");

    auto out_of_range = valid;
    out_of_range["global_hue"] = ParameterValue{360.1};
    auto range_result = validate(std::move(out_of_range));
    ASSERT_FALSE(range_result);
    EXPECT_EQ(range_result.error().context.at("parameter"), "global_hue");

    auto formula = valid;
    formula["saturation_formula"] = ParameterValue{"hsl"};
    auto formula_result = validate(std::move(formula));
    ASSERT_FALSE(formula_result);
    EXPECT_EQ(formula_result.error().code, ErrorCode::kValidation);

    auto workspace = valid;
    workspace["working_space"] = ParameterValue{"linear_srgb_d65"};
    auto workspace_result = validate(std::move(workspace));
    ASSERT_FALSE(workspace_result);
    EXPECT_EQ(workspace_result.error().code, ErrorCode::kValidation);

    auto mask_fulcrum = valid;
    mask_fulcrum["mask_grey_fulcrum"] = ParameterValue{0.0};
    auto mask_result = validate(std::move(mask_fulcrum));
    ASSERT_FALSE(mask_result);
    EXPECT_EQ(mask_result.error().context.at("parameter"), "mask_grey_fulcrum");

    auto grey_fulcrum = valid;
    grey_fulcrum["grey_fulcrum"] = ParameterValue{0.0};
    auto grey_result = validate(std::move(grey_fulcrum));
    ASSERT_FALSE(grey_result);
    EXPECT_EQ(grey_result.error().context.at("parameter"), "grey_fulcrum");

    auto singular_power = valid;
    singular_power["midtones_y"] = ParameterValue{-1.0};
    auto power_result = validate(std::move(singular_power));
    ASSERT_FALSE(power_result);
    EXPECT_EQ(power_result.error().context.at("parameter"), "midtones_y");
}

[[nodiscard]] double point_segment_distance(const double px, const double py, const double ax,
                                            const double ay, const double bx, const double by)
{
    const double vx = bx - ax;
    const double vy = by - ay;
    const double wx = px - ax;
    const double wy = py - ay;
    const double length2 = vx * vx + vy * vy;
    const double t = length2 > 0.0 ? std::clamp((wx * vx + wy * vy) / length2, 0.0, 1.0) : 0.0;
    return std::hypot(ax + t * vx - px, ay + t * vy - py);
}

[[nodiscard]] double distance_to_quad(const double x, const double y,
                                      const std::array<double, 8> &corners)
{
    double best = 1.0e9;
    for (int index = 0; index < 4; ++index)
    {
        const int next = (index + 1) % 4;
        best = std::min(best,
                        point_segment_distance(x, y, corners[static_cast<std::size_t>(index * 2)],
                                               corners[static_cast<std::size_t>(index * 2 + 1)],
                                               corners[static_cast<std::size_t>(next * 2)],
                                               corners[static_cast<std::size_t>(next * 2 + 1)]));
    }
    return best;
}

TEST(RecipeTest, StraightenedSourceQuadTouchesInscribedCrop)
{
    std::array<double, 8> identity{};
    straightened_source_quad(0.0, 1.5, identity);
    EXPECT_NEAR(identity[0], 0.0, 1e-9);
    EXPECT_NEAR(identity[1], 0.0, 1e-9);
    EXPECT_NEAR(identity[2], 1.0, 1e-9);
    EXPECT_NEAR(identity[3], 0.0, 1e-9);
    EXPECT_NEAR(identity[4], 1.0, 1e-9);
    EXPECT_NEAR(identity[5], 1.0, 1e-9);
    EXPECT_NEAR(identity[6], 0.0, 1e-9);
    EXPECT_NEAR(identity[7], 1.0, 1e-9);

    std::array<double, 8> corners{};
    straightened_source_quad(15.0, 1.5, corners);
    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_w = 1.0;
    double crop_h = 1.0;
    inscribed_crop_for_straighten(15.0, 1.5, 1.0, crop_x, crop_y, crop_w, crop_h);
    const double sides[8] = {crop_x,          crop_y,          crop_x + crop_w, crop_y,
                             crop_x + crop_w, crop_y + crop_h, crop_x,          crop_y + crop_h};
    int touching = 0;
    double closest = 1.0;
    for (int side = 0; side < 4; ++side)
    {
        const int next = (side + 1) % 4;
        const double ax = sides[side * 2];
        const double ay = sides[side * 2 + 1];
        const double bx = sides[next * 2];
        const double by = sides[next * 2 + 1];
        double side_distance = 1.0;
        for (int sample = 0; sample <= 20; ++sample)
        {
            const double t = static_cast<double>(sample) / 20.0;
            const double x = ax + (bx - ax) * t;
            const double y = ay + (by - ay) * t;
            side_distance = std::min(side_distance, distance_to_quad(x, y, corners));
            double source_x = 0.0;
            double source_y = 0.0;
            map_straighten_normalized(x, y, 15.0, 1.5, true, source_x, source_y);
            EXPECT_GE(source_x, -1e-6);
            EXPECT_LE(source_x, 1.0 + 1e-6);
            EXPECT_GE(source_y, -1e-6);
            EXPECT_LE(source_y, 1.0 + 1e-6);
        }
        closest = std::min(closest, side_distance);
        if (side_distance < 1e-6)
        {
            ++touching;
        }
    }
    EXPECT_LT(closest, 1e-6);
    EXPECT_GE(touching, 2);

    DevelopParams fitted;
    fitted.straighten_degrees = 15.0;
    fitted.crop_width = 1.0;
    fitted.crop_height = 1.0;
    fit_crop_to_straighten(fitted, 1.5);
    EXPECT_NEAR(fitted.crop_x, crop_x, 1e-9);
    EXPECT_NEAR(fitted.crop_y, crop_y, 1e-9);
    EXPECT_NEAR(fitted.crop_width, crop_w, 1e-9);
    EXPECT_NEAR(fitted.crop_height, crop_h, 1e-9);
}

TEST(RecipeTest, ToneCurveRoundTripAndRejectsUnknownColourPolicy)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_NEAR(evaluate_tone_curve({}, 0.35), 0.35, 1e-9);
    EXPECT_NEAR(evaluate_tone_curve({{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}}, 0.5), 0.75, 1e-9);

    DevelopParams params;
    params.tone_curve = {{0.0, 0.0}, {0.5, 0.65}, {1.0, 1.0}};
    params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceLinearRgb);
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto valid = validate_recipe(recipe.value(), registry.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().tone_curve.size(), 3U);
    EXPECT_NEAR(restored.value().tone_curve[1].y, 0.65, 1e-6);
    EXPECT_EQ(restored.value().tone_curve_working_space, kToneCurveWorkingSpaceLinearRgb);
    EXPECT_FALSE(restored.value().is_identity());
    EXPECT_TRUE(reset_develop_field(restored.value(), "toneCurve"));
    EXPECT_TRUE(restored.value().tone_curve.empty());

    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));

    auto lab = recipe.value();
    auto *lab_curve = operation_by_id(lab, "ravo.core.tonecurve");
    ASSERT_NE(lab_curve, nullptr);
    lab_curve->parameters["working_space"] = ParameterValue{"lab"};
    const auto accepted_lab = validate_recipe(lab, registry.value());
    ASSERT_TRUE(accepted_lab) << accepted_lab.error().message;

    auto unknown_space = recipe.value();
    auto *unknown_curve = operation_by_id(unknown_space, "ravo.core.tonecurve");
    ASSERT_NE(unknown_curve, nullptr);
    unknown_curve->parameters["working_space"] = ParameterValue{"display_p3"};
    const auto rejected_space = validate_recipe(unknown_space, registry.value());
    ASSERT_FALSE(rejected_space);
    EXPECT_EQ(rejected_space.error().code, ErrorCode::kValidation);

    auto decreasing = recipe.value();
    auto *decreasing_curve = operation_by_id(decreasing, "ravo.core.tonecurve");
    ASSERT_NE(decreasing_curve, nullptr);
    decreasing_curve->parameters["points"] = ParameterValue{ParameterValue::Array{
        ParameterValue{
            ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
        ParameterValue{
            ParameterValue::Object{{"x", ParameterValue{0.2}}, {"y", ParameterValue{0.4}}}},
        ParameterValue{
            ParameterValue::Object{{"x", ParameterValue{0.1}}, {"y", ParameterValue{0.8}}}},
        ParameterValue{
            ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
    }};
    const auto rejected_points = validate_recipe(decreasing, registry.value());
    ASSERT_FALSE(rejected_points);
    EXPECT_EQ(rejected_points.error().code, ErrorCode::kValidation);
}

TEST(RecipeTest, SigmoidRoundTripRequiresExplicitFiniteColorPolicy)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    DevelopParams params;
    ASSERT_TRUE(apply_develop_field(params, "sigmoidContrast", 1.8));
    params.sigmoid_skew = -0.25;
    params.sigmoid_hue_preservation = 0.75;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().operations.size(), 3U);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.color.input"), nullptr);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.display.sigmoid"), nullptr);
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.color.output"), nullptr);
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));

    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().sigmoid_enabled);
    EXPECT_NEAR(restored.value().sigmoid_contrast, 1.8, 1e-9);
    EXPECT_NEAR(restored.value().sigmoid_skew, -0.25, 1e-9);
    EXPECT_NEAR(restored.value().sigmoid_hue_preservation, 0.75, 1e-9);

    auto rgb_ratio = recipe.value();
    auto *ratio_sigmoid = operation_by_id(rgb_ratio, "ravo.display.sigmoid");
    ASSERT_NE(ratio_sigmoid, nullptr);
    ratio_sigmoid->parameters["color_processing"] = ParameterValue{"rgb_ratio"};
    const auto accepted_ratio = validate_recipe(rgb_ratio, registry.value());
    ASSERT_TRUE(accepted_ratio) << accepted_ratio.error().message;

    auto unsupported = recipe.value();
    auto *unsupported_sigmoid = operation_by_id(unsupported, "ravo.display.sigmoid");
    ASSERT_NE(unsupported_sigmoid, nullptr);
    unsupported_sigmoid->parameters["color_processing"] = ParameterValue{"unknown"};
    const auto rejected_mode = validate_recipe(unsupported, registry.value());
    ASSERT_FALSE(rejected_mode);
    EXPECT_EQ(rejected_mode.error().code, ErrorCode::kValidation);

    auto missing = recipe.value();
    auto *missing_sigmoid = operation_by_id(missing, "ravo.display.sigmoid");
    ASSERT_NE(missing_sigmoid, nullptr);
    missing_sigmoid->parameters.erase("working_space");
    const auto rejected_missing = validate_recipe(missing, registry.value());
    ASSERT_FALSE(rejected_missing);
    EXPECT_EQ(rejected_missing.error().code, ErrorCode::kValidation);

    auto non_finite = recipe.value();
    auto *non_finite_sigmoid = operation_by_id(non_finite, "ravo.display.sigmoid");
    ASSERT_NE(non_finite_sigmoid, nullptr);
    non_finite_sigmoid->parameters["middle_grey_contrast"] =
        ParameterValue{std::numeric_limits<double>::quiet_NaN()};
    const auto rejected_non_finite = validate_recipe(non_finite, registry.value());
    ASSERT_FALSE(rejected_non_finite);
    EXPECT_EQ(rejected_non_finite.error().code, ErrorCode::kValidation);
}

TEST(RecipeTest, RejectsNewerSchemaVersionsBeforeValidation)
{
    const auto recipe = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[],"schema_version":4})");

    ASSERT_FALSE(recipe);
    EXPECT_EQ(recipe.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
