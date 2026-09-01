#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"

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

[[nodiscard]] std::uint64_t color_checker_patch_bits_hash(const ColorCheckerParams &params)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto byte = [&](const std::uint8_t value)
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    const auto word = [&](const std::uint32_t value)
    {
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
        {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    };
    word(static_cast<std::uint32_t>(params.patches.size()));
    for (const auto &patch : params.patches)
    {
        for (const auto &lab : {&patch.source_lab, &patch.target_lab})
        {
            for (const double component : *lab)
            {
                word(std::bit_cast<std::uint32_t>(static_cast<float>(component)));
            }
        }
    }
    return hash;
}

TEST(RecipeTest, CropMinShortEdgeIsMinOf300AndHalfShortSide)
{
    EXPECT_NEAR(kDevelopCropMinShortEdgePixels, 300.0, 1e-12);
    EXPECT_NEAR(kDevelopCropMinShortEdgeFraction, 0.5, 1e-12);
    EXPECT_NEAR(develop_crop_min_short_edge_pixels(4000, 3000), 300.0, 1e-9);
    EXPECT_NEAR(develop_crop_min_short_edge_pixels(400, 300), 150.0, 1e-9);
    EXPECT_NEAR(develop_crop_min_short_edge_pixels(200, 100), 50.0, 1e-9);
    EXPECT_NEAR(develop_crop_min_short_edge_pixels(0, 0), 0.0, 1e-12);

    DevelopParams tiny_crop;
    tiny_crop.crop_width = 0.02;
    tiny_crop.crop_height = 0.02;
    tiny_crop.crop_x = 0.4;
    tiny_crop.crop_y = 0.4;
    clamp_develop_crop_min_extent(tiny_crop, 4000, 3000);
    EXPECT_GE(std::min(tiny_crop.crop_width * 4000.0, tiny_crop.crop_height * 3000.0),
              300.0 - 1e-6);
    EXPECT_NEAR(tiny_crop.crop_width / tiny_crop.crop_height, 1.0, 1e-6);
    EXPECT_GE(tiny_crop.crop_x, 0.0);
    EXPECT_GE(tiny_crop.crop_y, 0.0);
    EXPECT_LE(tiny_crop.crop_x + tiny_crop.crop_width, 1.0 + 1e-9);
    EXPECT_LE(tiny_crop.crop_y + tiny_crop.crop_height, 1.0 + 1e-9);

    DevelopParams small_source;
    small_source.crop_width = 0.1;
    small_source.crop_height = 0.1;
    small_source.crop_x = 0.4;
    small_source.crop_y = 0.4;
    clamp_develop_crop_min_extent(small_source, 400, 300);
    const double small_short =
        std::min(small_source.crop_width * 400.0, small_source.crop_height * 300.0);
    EXPECT_GE(small_short, 150.0 - 1e-6);
    EXPECT_LT(small_short, 300.0);

    DevelopParams already_ok;
    already_ok.crop_width = 0.5;
    already_ok.crop_height = 0.5;
    already_ok.crop_x = 0.2;
    already_ok.crop_y = 0.2;
    clamp_develop_crop_min_extent(already_ok, 4000, 3000);
    EXPECT_NEAR(already_ok.crop_width, 0.5, 1e-9);
    EXPECT_NEAR(already_ok.crop_height, 0.5, 1e-9);
    EXPECT_NEAR(already_ok.crop_x, 0.2, 1e-9);
    EXPECT_NEAR(already_ok.crop_y, 0.2, 1e-9);
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

TEST(RecipeTest, ColorCheckerSchemaPreservesExplicitDefaultPresenceAndOrderedPatches)
{
    const ColorCheckerParams defaults;
    ASSERT_EQ(defaults.patches.size(), 24U);

    const auto parameters = color_checker_to_parameters(defaults);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(parameters.value().size(), 3U);
    auto decoded = color_checker_from_parameters(parameters.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), defaults);

    DevelopParams develop;
    EXPECT_FALSE(develop.color_checker_enabled);
    develop.color_checker_enabled = true;
    develop.color_checker = defaults;
    develop.tone_eq_midtones = 0.25;
    develop.graduated_density = 0.5;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto *operation = operation_by_id(recipe.value(), kColorCheckerOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->schema_version, kColorCheckerOperationSchemaVersion);
    auto operation_params = color_checker_from_parameters(operation->parameters);
    ASSERT_TRUE(operation_params) << operation_params.error().message;
    EXPECT_EQ(operation_params.value(), defaults);
    const auto *tone_equal = operation_by_id(recipe.value(), "ravo.core.toneequal");
    const auto *graduated = operation_by_id(recipe.value(), "ravo.effect.graduatednd");
    ASSERT_NE(tone_equal, nullptr);
    ASSERT_NE(graduated, nullptr);
    EXPECT_LT(tone_equal, graduated);
    EXPECT_LT(graduated, operation);

    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_checker_enabled);
    EXPECT_EQ(restored.value().color_checker, defaults);
}

TEST(RecipeTest, ColorCheckerDevelopSerializationRejectsInvalidPatchPayloadAtomically)
{
    DevelopParams develop;
    develop.color_checker_enabled = true;
    develop.color_checker.patches.resize(kColorCheckerMaxPatchCount + 1U);
    auto too_many = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error().code, ErrorCode::kValidation);
    EXPECT_EQ(too_many.error().context.at("patch_count"), "50");

    develop.color_checker = ColorCheckerParams{};
    develop.color_checker.patches.front().target_lab[2] = std::numeric_limits<double>::infinity();
    auto non_finite =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_FALSE(non_finite);
    EXPECT_EQ(non_finite.error().code, ErrorCode::kValidation);
    EXPECT_EQ(non_finite.error().context.at("field"), "target_lab");
}

TEST(RecipeTest, ColorCheckerDevelopFieldsPresetsSelectionResetAndRecipePropagationAreExact)
{
    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCheckerPreset", 1.0));
    EXPECT_TRUE(develop.color_checker_enabled);
    ASSERT_EQ(develop.color_checker.patches.size(), kColorCheckerMaxPatchCount);
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCheckerPatch", 48.0));
    EXPECT_EQ(develop.color_checker_patch, 48);
    const std::array assignments{
        std::pair{"colorCheckerSourceL", -12345.5}, std::pair{"colorCheckerSourceA", 2.5e20},
        std::pair{"colorCheckerSourceB", -2.5e20},  std::pair{"colorCheckerTargetL", 98.25},
        std::pair{"colorCheckerTargetA", -127.75},  std::pair{"colorCheckerTargetB", 126.5},
    };
    for (const auto &[name, value] : assignments)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, name, value)) << name;
    }
    const auto &patch = develop.color_checker.patches[48];
    EXPECT_EQ(patch.source_lab, (std::array<double, 3>{-12345.5, 2.5e20, -2.5e20}));
    EXPECT_EQ(patch.target_lab, (std::array<double, 3>{98.25, -127.75, 126.5}));

    const DevelopParams before_invalid = develop;
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorCheckerPatch", 49.0));
    EXPECT_EQ(develop, before_invalid);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorCheckerSourceL",
                                            std::numeric_limits<double>::max()));
    EXPECT_EQ(develop, before_invalid);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorCheckerTargetB",
                                            std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(develop, before_invalid);

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_checker_enabled);
    EXPECT_EQ(restored.value().color_checker, develop.color_checker);
    EXPECT_EQ(restored.value().color_checker_patch, 0);

    ASSERT_TRUE(reset_develop_field(develop, "colorCheckerTargetL"));
    EXPECT_TRUE(develop.color_checker_enabled);
    EXPECT_DOUBLE_EQ(develop.color_checker.patches[48].target_lab[0],
                     develop.color_checker.patches[48].source_lab[0]);
    ASSERT_TRUE(reset_develop_field(develop, "colorCheckerPatch"));
    EXPECT_EQ(develop.color_checker_patch, 0);
    ASSERT_TRUE(reset_develop_field(develop, "colorChecker"));
    EXPECT_FALSE(develop.color_checker_enabled);
    EXPECT_EQ(develop.color_checker, ColorCheckerParams{});

    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCheckerEnabled", 1.0));
    ASSERT_TRUE(reset_develop_section(develop, "color"));
    EXPECT_FALSE(develop.color_checker_enabled);
    EXPECT_EQ(develop.color_checker, ColorCheckerParams{});
}

TEST(RecipeTest, ColorCheckerBuiltInPresetsRetainEveryFrozenFloatBit)
{
    const auto presets = color_checker_presets();
    ASSERT_EQ(presets.size(), 8U);
    const std::array<std::string_view, 8> ids{
        "it8_skin_tones",
        "expanded_color_checker",
        "helmholtz_kohlrausch_monochrome",
        "fuji_astia",
        "fuji_classic_chrome",
        "fuji_monochrome",
        "fuji_provia",
        "fuji_velvia",
    };
    const std::array<std::size_t, 8> counts{24U, 49U, 24U, 49U, 49U, 49U, 49U, 49U};
    const std::array<std::uint64_t, 8> hashes{
        0xd1fa2f4eeccd087fULL, 0xdad449f7b528d042ULL, 0x16df310bb0600905ULL, 0x653efef5b16f72dfULL,
        0x8616fc5291b47864ULL, 0xf791711d73468245ULL, 0x2b549f35e1d53c30ULL, 0x60efc6c59221405fULL,
    };
    for (std::size_t index = 0U; index < presets.size(); ++index)
    {
        SCOPED_TRACE(ids[index]);
        EXPECT_EQ(presets[index].id, ids[index]);
        EXPECT_EQ(presets[index].patch_count, counts[index]);
        auto params = color_checker_params_for_preset(ids[index]);
        ASSERT_TRUE(params) << params.error().message;
        EXPECT_EQ(params.value().patches.size(), counts[index]);
        EXPECT_EQ(color_checker_patch_bits_hash(params.value()), hashes[index]);
    }
    auto unknown = color_checker_params_for_preset("generic_lut");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::kUnsupported);
}

TEST(RecipeTest, LegacyColorBalanceSchemaRoundTripsAllFrozenV4FieldsIndependently)
{
    ColorBalanceParams params;
    params.mode = std::string(kColorBalanceModeLiftGammaGain);
    params.lift = {0.91, 1.02, 0.97, 1.08};
    params.gamma = {1.12, 0.88, 1.07, 0.94};
    params.gain = {1.09, 1.15, 0.93, 1.04};
    params.input_saturation = 0.83;
    params.contrast = 1.17;
    params.grey_fulcrum_percent = 21.5;
    params.output_saturation = 1.11;

    const auto parameters = color_balance_to_parameters(params);
    EXPECT_EQ(parameters.size(), 19U);
    auto decoded = color_balance_from_parameters(parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);

    DevelopParams develop;
    develop.color_balance_enabled = true;
    develop.color_balance = params;
    develop.color_balance_rgb.global_y = 0.08;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().operations.size(), 4U);
    const auto *legacy = operation_by_id(recipe.value(), kColorBalanceOperationId);
    const auto *rgb = operation_by_id(recipe.value(), "ravo.color.colorbalancergb");
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(rgb, nullptr);
    EXPECT_NE(legacy->instance_id, rgb->instance_id);
    EXPECT_LT(static_cast<std::size_t>(legacy - recipe.value().operations.data()),
              static_cast<std::size_t>(rgb - recipe.value().operations.data()));

    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kColorBalanceOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kColorBalanceOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 19U);
    EXPECT_FALSE(descriptor->supports_mask);
    EXPECT_TRUE(descriptor->cpu_reference_available);
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));

    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().color_balance, params);
    EXPECT_EQ(restored.value().color_balance_rgb, develop.color_balance_rgb);
}

TEST(RecipeTest, LegacyColorBalanceStrictSchemaFieldAssignmentClampAndResetAreComplete)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto validate = [&](std::map<std::string, ParameterValue, std::less<>> parameters)
    {
        Recipe recipe;
        recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
        recipe.operations.push_back({std::string(kColorBalanceOperationId),
                                     kColorBalanceOperationSchemaVersion, "colorbalance-1", true,
                                     std::move(parameters), std::nullopt});
        return validate_recipe(recipe, registry.value());
    };

    const auto canonical = color_balance_to_parameters(ColorBalanceParams{});
    ASSERT_TRUE(validate(canonical));
    for (const std::string_view key :
         {"working_space", "algorithm", "mode", "lift_factor", "lift_red", "lift_green",
          "lift_blue", "gamma_factor", "gamma_red", "gamma_green", "gamma_blue", "gain_factor",
          "gain_red", "gain_green", "gain_blue", "input_saturation", "contrast",
          "grey_fulcrum_percent", "output_saturation"})
    {
        auto missing = canonical;
        missing.erase(std::string(key));
        auto result = validate(std::move(missing));
        ASSERT_FALSE(result) << key;
        EXPECT_EQ(result.error().code, ErrorCode::kValidation) << key;
    }

    for (const auto &[key, value] : std::array<std::pair<std::string_view, ParameterValue>, 8>{
             std::pair{"working_space", ParameterValue{std::string("prophoto")}},
             std::pair{"algorithm", ParameterValue{std::string("simplified")}},
             std::pair{"mode", ParameterValue{std::string("legacy")}},
             std::pair{"lift_red", ParameterValue{-0.01}},
             std::pair{"gamma_green", ParameterValue{2.01}},
             std::pair{"contrast", ParameterValue{0.0}},
             std::pair{"grey_fulcrum_percent", ParameterValue{101.0}},
             std::pair{"gain_blue", ParameterValue{std::numeric_limits<double>::infinity()}},
         })
    {
        auto invalid = canonical;
        invalid[std::string(key)] = value;
        auto result = validate(std::move(invalid));
        ASSERT_FALSE(result) << key;
        EXPECT_EQ(result.error().code, ErrorCode::kValidation) << key;
    }
    auto unknown = canonical;
    unknown["rgb_global"] = ParameterValue{1.0};
    ASSERT_FALSE(validate(std::move(unknown)));

    DevelopParams develop;
    const std::array assignments{
        std::pair{"legacyColorBalanceLiftFactor", 0.91},
        std::pair{"legacyColorBalanceLiftRed", 1.02},
        std::pair{"legacyColorBalanceLiftGreen", 0.97},
        std::pair{"legacyColorBalanceLiftBlue", 1.08},
        std::pair{"legacyColorBalanceGammaFactor", 1.12},
        std::pair{"legacyColorBalanceGammaRed", 0.88},
        std::pair{"legacyColorBalanceGammaGreen", 1.07},
        std::pair{"legacyColorBalanceGammaBlue", 0.94},
        std::pair{"legacyColorBalanceGainFactor", 1.09},
        std::pair{"legacyColorBalanceGainRed", 1.15},
        std::pair{"legacyColorBalanceGainGreen", 0.93},
        std::pair{"legacyColorBalanceGainBlue", 1.04},
        std::pair{"legacyColorBalanceInputSaturation", 0.83},
        std::pair{"legacyColorBalanceContrast", 1.17},
        std::pair{"legacyColorBalanceGreyFulcrum", 21.5},
        std::pair{"legacyColorBalanceOutputSaturation", 1.11},
    };
    ASSERT_TRUE(apply_develop_field_strict(develop, "legacyColorBalanceMode", 0.0));
    for (const auto &[name, value] : assignments)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, name, value)) << name;
    }
    EXPECT_EQ(develop.color_balance.mode, kColorBalanceModeLiftGammaGain);
    EXPECT_TRUE(develop.color_balance_enabled);
    EXPECT_FALSE(develop.color_balance.is_identity());
    auto before_invalid = develop;
    EXPECT_FALSE(apply_develop_field_strict(develop, "legacyColorBalanceContrast", 0.0));
    EXPECT_EQ(develop, before_invalid);
    EXPECT_FALSE(apply_develop_field_strict(develop, "legacyColorBalanceLiftRed",
                                            std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(develop, before_invalid);

    for (const auto &[name, ignored] : assignments)
    {
        static_cast<void>(ignored);
        ASSERT_TRUE(reset_develop_field(develop, name)) << name;
    }
    ASSERT_TRUE(reset_develop_field(develop, "legacyColorBalanceMode"));
    EXPECT_TRUE(develop.color_balance_enabled);
    EXPECT_TRUE(develop.color_balance.is_identity());

    develop.color_balance.lift[1] = 4.0;
    develop.color_balance.contrast = -1.0;
    develop.color_balance.grey_fulcrum_percent = std::numeric_limits<double>::quiet_NaN();
    develop.color_balance.mode = "unknown";
    clamp_develop(develop);
    EXPECT_DOUBLE_EQ(develop.color_balance.lift[1], 2.0);
    EXPECT_DOUBLE_EQ(develop.color_balance.contrast, 0.01);
    EXPECT_DOUBLE_EQ(develop.color_balance.grey_fulcrum_percent, 18.0);
    EXPECT_EQ(develop.color_balance.mode, kColorBalanceModeSlopeOffsetPower);
    ASSERT_TRUE(reset_develop_field(develop, "legacyColorBalance"));
    EXPECT_FALSE(develop.color_balance_enabled);
    EXPECT_EQ(develop.color_balance, ColorBalanceParams{});
}

TEST(RecipeTest, ExplicitDefaultLegacyColorBalancePresenceSurvivesDevelopRoundTrip)
{
    Recipe recipe;
    recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    recipe.operations.push_back({std::string(kColorBalanceOperationId),
                                 kColorBalanceOperationSchemaVersion, "colorbalance-explicit", true,
                                 color_balance_to_parameters(ColorBalanceParams{}), std::nullopt});

    auto develop = develop_from_recipe(recipe);
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_TRUE(develop.value().color_balance_enabled);
    EXPECT_EQ(develop.value().color_balance, ColorBalanceParams{});
    EXPECT_FALSE(develop.value().is_identity());

    auto restored = recipe_from_develop(recipe.asset, develop.value());
    ASSERT_TRUE(restored) << restored.error().message;
    auto *operation = operation_by_id(restored.value(), kColorBalanceOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->parameters.size(), 19U);
    auto decoded = color_balance_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorBalanceParams{});

    ASSERT_TRUE(reset_develop_field(develop.value(), "legacyColorBalanceLiftRed"));
    EXPECT_TRUE(develop.value().color_balance_enabled);
    ASSERT_TRUE(reset_develop_field(develop.value(), "legacyColorBalance"));
    EXPECT_FALSE(develop.value().color_balance_enabled);
    auto absent = recipe_from_develop(recipe.asset, develop.value());
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_EQ(operation_by_id(absent.value(), kColorBalanceOperationId), nullptr);
}

TEST(RecipeTest, ColorCorrectionDevelopFieldsAreStrictResettableAndCanonicallyOrdered)
{
    DevelopParams develop;
    EXPECT_FALSE(develop.color_correction_enabled);
    EXPECT_EQ(develop.color_correction, ColorCorrectionParams{});

    struct Boundary
    {
        std::string_view field;
        double minimum;
        double maximum;
    };
    constexpr std::array boundaries{
        Boundary{"colorCorrectionHighlightA", kColorCorrectionEndpointMin,
                 kColorCorrectionEndpointMax},
        Boundary{"colorCorrectionHighlightB", kColorCorrectionEndpointMin,
                 kColorCorrectionEndpointMax},
        Boundary{"colorCorrectionShadowA", kColorCorrectionEndpointMin,
                 kColorCorrectionEndpointMax},
        Boundary{"colorCorrectionShadowB", kColorCorrectionEndpointMin,
                 kColorCorrectionEndpointMax},
        Boundary{"colorCorrectionSaturation", kColorCorrectionSaturationMin,
                 kColorCorrectionSaturationMax},
    };
    for (const auto &[field, minimum, maximum] : boundaries)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, field, minimum)) << field;
        EXPECT_TRUE(develop.color_correction_enabled) << field;
        ASSERT_TRUE(apply_develop_field_strict(develop, field, maximum)) << field;
        const DevelopParams before_low = develop;
        EXPECT_FALSE(apply_develop_field_strict(develop, field, minimum - 0.01)) << field;
        EXPECT_EQ(develop, before_low) << field;
        const DevelopParams before_high = develop;
        EXPECT_FALSE(apply_develop_field_strict(develop, field, maximum + 0.01)) << field;
        EXPECT_EQ(develop, before_high) << field;
    }
    const DevelopParams before_nonfinite = develop;
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorCorrectionHighlightA",
                                            std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(develop, before_nonfinite);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorCorrectionEnabled", 0.5));
    EXPECT_EQ(develop, before_nonfinite);

    DevelopParams repaired;
    repaired.color_correction_enabled = true;
    repaired.color_correction =
        ColorCorrectionParams{std::numeric_limits<double>::infinity(), -41.0,
                              std::numeric_limits<double>::quiet_NaN(), 41.0, -4.0};
    clamp_develop(repaired);
    EXPECT_TRUE(repaired.color_correction_enabled);
    EXPECT_EQ(repaired.color_correction,
              (ColorCorrectionParams{0.0, kColorCorrectionEndpointMin, 0.0,
                                     kColorCorrectionEndpointMax, kColorCorrectionSaturationMin}));

    const std::array assignments{
        std::pair{"colorCorrectionHighlightA", 12.5},
        std::pair{"colorCorrectionHighlightB", -8.25},
        std::pair{"colorCorrectionShadowA", -4.75},
        std::pair{"colorCorrectionShadowB", 9.5},
        std::pair{"colorCorrectionSaturation", 1.375},
    };
    for (const auto &[field, value] : assignments)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, field, value)) << field;
    }
    EXPECT_EQ(develop.color_correction, (ColorCorrectionParams{12.5, -8.25, -4.75, 9.5, 1.375}));

    develop.color_balance_rgb.global_y = 0.01;
    develop.color_contrast_enabled = true;
    develop.color_contrast.a_steepness = 1.25;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto rgb_balance =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == "ravo.color.colorbalancergb"; });
    const auto correction =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == kColorCorrectionOperationId; });
    const auto contrast =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == "ravo.color.colorcontrast"; });
    ASSERT_NE(rgb_balance, recipe.value().operations.end());
    ASSERT_NE(correction, recipe.value().operations.end());
    ASSERT_NE(contrast, recipe.value().operations.end());
    EXPECT_LT(rgb_balance, correction);
    EXPECT_LT(correction, contrast);
    auto canonical = color_correction_from_parameters(correction->parameters);
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(canonical.value(), develop.color_correction);

    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_correction_enabled);
    EXPECT_EQ(restored.value().color_correction, develop.color_correction);

    for (const auto &[field, ignored] : assignments)
    {
        static_cast<void>(ignored);
        ASSERT_TRUE(reset_develop_field(develop, field)) << field;
        EXPECT_TRUE(develop.color_correction_enabled) << field;
    }
    EXPECT_EQ(develop.color_correction, ColorCorrectionParams{});
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCorrectionEnabled", 0.0));
    EXPECT_FALSE(develop.color_correction_enabled);
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCorrectionEnabled", 1.0));
    EXPECT_TRUE(develop.color_correction_enabled);
    ASSERT_TRUE(reset_develop_field(develop, "colorCorrection"));
    EXPECT_FALSE(develop.color_correction_enabled);
    EXPECT_EQ(develop.color_correction, ColorCorrectionParams{});
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorCorrectionHighlightA", 1.0));
    ASSERT_TRUE(reset_develop_section(develop, "color"));
    EXPECT_FALSE(develop.color_correction_enabled);
    EXPECT_EQ(develop.color_correction, ColorCorrectionParams{});
}

TEST(RecipeTest, ExplicitDefaultColorCorrectionPresenceSurvivesDevelopRoundTrip)
{
    const auto parameters = color_correction_to_parameters(ColorCorrectionParams{});
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe recipe;
    recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    recipe.operations.push_back({std::string(kColorCorrectionOperationId),
                                 kColorCorrectionOperationSchemaVersion, "colorcorrection-explicit",
                                 true, parameters.value(), std::nullopt});

    auto develop = develop_from_recipe(recipe);
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_TRUE(develop.value().color_correction_enabled);
    EXPECT_EQ(develop.value().color_correction, ColorCorrectionParams{});
    EXPECT_FALSE(develop.value().is_identity());

    auto restored = recipe_from_develop(recipe.asset, develop.value());
    ASSERT_TRUE(restored) << restored.error().message;
    const auto *operation = operation_by_id(restored.value(), kColorCorrectionOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->enabled);
    auto decoded = color_correction_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorCorrectionParams{});

    auto absent = recipe_from_develop(recipe.asset, DevelopParams{});
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_EQ(operation_by_id(absent.value(), kColorCorrectionOperationId), nullptr);
}

TEST(RecipeTest, ColorContrastDevelopFieldsAreStrictResettableAndCanonicallyOrdered)
{
    DevelopParams develop;
    EXPECT_FALSE(develop.color_contrast_enabled);
    EXPECT_EQ(develop.color_contrast, ColorContrastParams{});
    EXPECT_TRUE(develop.is_identity());

    constexpr std::array slope_fields{"colorContrastASteepness", "colorContrastBSteepness"};
    for (const auto field : slope_fields)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, field, kColorContrastSteepnessMin))
            << field;
        EXPECT_TRUE(develop.color_contrast_enabled) << field;
        ASSERT_TRUE(apply_develop_field_strict(develop, field, kColorContrastSteepnessMax))
            << field;
        const DevelopParams before_low = develop;
        EXPECT_FALSE(apply_develop_field_strict(develop, field, kColorContrastSteepnessMin - 0.01))
            << field;
        EXPECT_EQ(develop, before_low) << field;
        const DevelopParams before_high = develop;
        EXPECT_FALSE(apply_develop_field_strict(develop, field, kColorContrastSteepnessMax + 0.01))
            << field;
        EXPECT_EQ(develop, before_high) << field;
    }

    constexpr std::array offset_fields{"colorContrastAOffset", "colorContrastBOffset"};
    for (const auto field : offset_fields)
    {
        ASSERT_TRUE(apply_develop_field_strict(
            develop, field, static_cast<double>(std::numeric_limits<float>::lowest())))
            << field;
        ASSERT_TRUE(apply_develop_field_strict(
            develop, field, static_cast<double>(std::numeric_limits<float>::max())))
            << field;
        const DevelopParams before_overflow = develop;
        EXPECT_FALSE(apply_develop_field_strict(
            develop, field, static_cast<double>(std::numeric_limits<float>::max()) * 2.0))
            << field;
        EXPECT_EQ(develop, before_overflow) << field;
    }
    const DevelopParams before_nonfinite = develop;
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorContrastAOffset",
                                            std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(develop, before_nonfinite);
    for (const auto field : {"colorContrastEnabled", "colorContrastUnbound"})
    {
        EXPECT_FALSE(apply_develop_field_strict(develop, field, 0.5)) << field;
        EXPECT_EQ(develop, before_nonfinite) << field;
    }

    DevelopParams repaired;
    repaired.color_contrast_enabled = true;
    repaired.color_contrast =
        ColorContrastParams{1.0e300, static_cast<double>(std::numeric_limits<float>::max()) * 2.0,
                            -1.0, std::numeric_limits<double>::quiet_NaN(), false};
    clamp_develop(repaired);
    EXPECT_TRUE(repaired.color_contrast_enabled);
    EXPECT_EQ(repaired.color_contrast,
              (ColorContrastParams{kColorContrastSteepnessMax, 0.0, kColorContrastSteepnessMin, 0.0,
                                   false}));
    repaired.color_contrast.a_steepness = std::numeric_limits<double>::infinity();
    clamp_develop(repaired);
    EXPECT_DOUBLE_EQ(repaired.color_contrast.a_steepness, 1.0);

    const std::array assignments{
        std::pair{"colorContrastASteepness", 2.5}, std::pair{"colorContrastAOffset", -7.25},
        std::pair{"colorContrastBSteepness", 3.5}, std::pair{"colorContrastBOffset", 8.75},
        std::pair{"colorContrastUnbound", 0.0},
    };
    develop = DevelopParams{};
    for (const auto &[field, value] : assignments)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, field, value)) << field;
        EXPECT_TRUE(develop.color_contrast_enabled) << field;
    }
    const ColorContrastParams expected{2.5, -7.25, 3.5, 8.75, false};
    EXPECT_EQ(develop.color_contrast, expected);
    EXPECT_FALSE(develop.is_identity());

    develop.color_correction_enabled = true;
    develop.velvia_present = true;
    develop.velvia_enabled = true;
    develop.velvia = {25.0, 1.0};
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *correction = operation_by_id(recipe.value(), kColorCorrectionOperationId);
    const auto *contrast = operation_by_id(recipe.value(), kColorContrastOperationId);
    const auto *velvia = operation_by_id(recipe.value(), "ravo.color.velvia");
    ASSERT_NE(correction, nullptr);
    ASSERT_NE(contrast, nullptr);
    ASSERT_NE(velvia, nullptr);
    EXPECT_LT(correction, contrast);
    EXPECT_LT(contrast, velvia);
    EXPECT_EQ(contrast->schema_version, kColorContrastOperationSchemaVersion);
    auto canonical = color_contrast_from_parameters(contrast->parameters);
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(canonical.value(), expected);

    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_contrast_enabled);
    EXPECT_EQ(restored.value().color_contrast, expected);

    for (const auto &[field, ignored] : assignments)
    {
        static_cast<void>(ignored);
        ASSERT_TRUE(reset_develop_field(develop, field)) << field;
        EXPECT_TRUE(develop.color_contrast_enabled) << field;
    }
    EXPECT_EQ(develop.color_contrast, ColorContrastParams{});
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorContrastEnabled", 0.0));
    EXPECT_FALSE(develop.color_contrast_enabled);
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorContrastEnabled", 1.0));
    EXPECT_TRUE(develop.color_contrast_enabled);
    ASSERT_TRUE(reset_develop_field(develop, "colorContrast"));
    EXPECT_FALSE(develop.color_contrast_enabled);
    EXPECT_EQ(develop.color_contrast, ColorContrastParams{});
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorContrastAOffset", 1.0));
    ASSERT_TRUE(reset_develop_section(develop, "color"));
    EXPECT_FALSE(develop.color_contrast_enabled);
    EXPECT_EQ(develop.color_contrast, ColorContrastParams{});
}

TEST(RecipeTest, ColorContrastV1AndExplicitDefaultV2PresenceSurviveDevelopRoundTrip)
{
    const auto parameters = color_contrast_to_parameters(ColorContrastParams{});
    ASSERT_TRUE(parameters) << parameters.error().message;
    Recipe canonical;
    canonical.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    canonical.operations.push_back({std::string(kColorContrastOperationId),
                                    kColorContrastOperationSchemaVersion, "colorcontrast-explicit",
                                    true, parameters.value(), std::nullopt});

    auto develop = develop_from_recipe(canonical);
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_TRUE(develop.value().color_contrast_enabled);
    EXPECT_EQ(develop.value().color_contrast, ColorContrastParams{});
    EXPECT_FALSE(develop.value().is_identity());

    auto restored = recipe_from_develop(canonical.asset, develop.value());
    ASSERT_TRUE(restored) << restored.error().message;
    const auto *operation = operation_by_id(restored.value(), kColorContrastOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorContrastOperationSchemaVersion);
    auto decoded = color_contrast_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorContrastParams{});

    Recipe disabled = canonical;
    disabled.operations.front().enabled = false;
    auto disabled_develop = develop_from_recipe(disabled);
    ASSERT_TRUE(disabled_develop) << disabled_develop.error().message;
    EXPECT_FALSE(disabled_develop.value().color_contrast_enabled);
    EXPECT_TRUE(disabled_develop.value().is_identity());

    Recipe masked = canonical;
    masked.operations.front().mask_id = "mask-1";
    auto rejected_mask = develop_from_recipe(masked);
    ASSERT_FALSE(rejected_mask);
    EXPECT_EQ(rejected_mask.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected_mask.error().context.at("reason"), "unsupported_colorcontrast_mask");

    Recipe v1 = canonical;
    v1.operations.front().schema_version = 1;
    v1.operations.front().parameters = {{"amount", ParameterValue{0.25}}};
    auto upgraded = develop_from_recipe(v1);
    ASSERT_TRUE(upgraded) << upgraded.error().message;
    EXPECT_TRUE(upgraded.value().color_contrast_enabled);
    EXPECT_EQ(upgraded.value().color_contrast, (ColorContrastParams{1.25, 0.0, 1.25, 0.0, true}));

    v1.operations.front().parameters = {{"amount", ParameterValue{0.0}}};
    auto skipped = develop_from_recipe(v1);
    ASSERT_TRUE(skipped) << skipped.error().message;
    EXPECT_FALSE(skipped.value().color_contrast_enabled);
    EXPECT_EQ(skipped.value().color_contrast, ColorContrastParams{});
    EXPECT_TRUE(skipped.value().is_identity());

    auto absent = recipe_from_develop(canonical.asset, DevelopParams{});
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_EQ(operation_by_id(absent.value(), kColorContrastOperationId), nullptr);
}

TEST(RecipeTest, ColorHarmonizerActiveNodeCountFollowsRuleNotCustomCount)
{
    const std::array<std::pair<ColorHarmonizerRule, std::int64_t>, 9> predefined{
        std::pair{ColorHarmonizerRule::kMonochromatic, 1},
        std::pair{ColorHarmonizerRule::kAnalogous, 3},
        std::pair{ColorHarmonizerRule::kAnalogousComplementary, 4},
        std::pair{ColorHarmonizerRule::kComplementary, 2},
        std::pair{ColorHarmonizerRule::kSplitComplementary, 3},
        std::pair{ColorHarmonizerRule::kDyad, 2},
        std::pair{ColorHarmonizerRule::kTriad, 3},
        std::pair{ColorHarmonizerRule::kTetrad, 4},
        std::pair{ColorHarmonizerRule::kSquare, 4},
    };
    for (const auto &[rule, nodes] : predefined)
    {
        ColorHarmonizerParams params;
        params.rule = rule;
        params.num_custom_nodes = 2;
        EXPECT_EQ(color_harmonizer_active_node_count(params), nodes)
            << color_harmonizer_rule_name(rule);
        EXPECT_TRUE(color_harmonizer_uses_anchor_hue(rule));
        EXPECT_FALSE(color_harmonizer_uses_custom_hue(params, 0));
        EXPECT_TRUE(color_harmonizer_uses_node_saturation(params, 0));
        EXPECT_EQ(
            color_harmonizer_uses_node_saturation(params, static_cast<std::size_t>(nodes - 1)),
            true);
        if (nodes < 4)
        {
            EXPECT_FALSE(
                color_harmonizer_uses_node_saturation(params, static_cast<std::size_t>(nodes)));
        }
    }

    ColorHarmonizerParams custom;
    custom.rule = ColorHarmonizerRule::kCustom;
    custom.num_custom_nodes = 2;
    EXPECT_EQ(color_harmonizer_active_node_count(custom), 2);
    EXPECT_FALSE(color_harmonizer_uses_anchor_hue(custom.rule));
    EXPECT_TRUE(color_harmonizer_uses_custom_hue(custom, 1));
    EXPECT_FALSE(color_harmonizer_uses_custom_hue(custom, 2));
    EXPECT_FALSE(color_harmonizer_uses_custom_hue(custom, std::numeric_limits<std::size_t>::max()));
    custom.num_custom_nodes = 3;
    EXPECT_EQ(color_harmonizer_active_node_count(custom), 3);
    EXPECT_TRUE(color_harmonizer_uses_custom_hue(custom, 2));
    EXPECT_FALSE(color_harmonizer_uses_custom_hue(custom, 3));
}

TEST(RecipeTest, ColorHarmonizerHueDegreesConversionIsTheSingleDevelopOwner)
{
    auto turns = color_harmonizer_hue_degrees_to_turns(90.0);
    ASSERT_TRUE(turns) << turns.error().message;
    EXPECT_DOUBLE_EQ(turns.value(), 0.25);
    EXPECT_DOUBLE_EQ(color_harmonizer_hue_turns_to_degrees(0.25), 90.0);
    EXPECT_FALSE(color_harmonizer_hue_degrees_to_turns(-0.01));
    EXPECT_FALSE(color_harmonizer_hue_degrees_to_turns(360.01));

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerAnchorHueDegrees", 90.0));
    EXPECT_DOUBLE_EQ(develop.color_harmonizer.anchor_hue, 0.25);
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerCustomHue1Degrees", 180.0));
    EXPECT_DOUBLE_EQ(develop.color_harmonizer.custom_hue[1], 0.5);
}

TEST(RecipeTest, ColorHarmonizerDevelopFieldsAreStrictResettableAndPreservePresence)
{
    DevelopParams develop;
    EXPECT_FALSE(develop.color_harmonizer_enabled);
    EXPECT_EQ(develop.color_harmonizer, ColorHarmonizerParams{});
    EXPECT_TRUE(develop.is_identity());

    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerEnabled", 1.0));
    EXPECT_TRUE(develop.color_harmonizer_enabled);
    EXPECT_EQ(develop.color_harmonizer, ColorHarmonizerParams{});
    EXPECT_FALSE(develop.is_identity());
    auto explicit_default =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(explicit_default) << explicit_default.error().message;
    const auto *operation = operation_by_id(explicit_default.value(), kColorHarmonizerOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->instance_id, "colorharmonizer-1");
    EXPECT_EQ(operation->parameters.size(), 17U);
    auto decoded = color_harmonizer_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorHarmonizerParams{});
    EXPECT_DOUBLE_EQ(decoded.value().smoothing, 0.0);
    auto restored = develop_from_recipe(explicit_default.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer, ColorHarmonizerParams{});

    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength", 0.0));
    EXPECT_TRUE(develop.color_harmonizer_enabled);
    ASSERT_TRUE(reset_develop_field(develop, "colorHarmonizerEnabled"));
    EXPECT_FALSE(develop.color_harmonizer_enabled);
    auto absent = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(absent) << absent.error().message;
    const auto *disabled = operation_by_id(absent.value(), kColorHarmonizerOperationId);
    ASSERT_NE(disabled, nullptr);
    EXPECT_FALSE(disabled->enabled);

    develop = DevelopParams{};
    for (std::int64_t index = 0; index < static_cast<std::int64_t>(kColorHarmonizerRuleCount);
         ++index)
    {
        ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerRuleIndex",
                                               static_cast<double>(index)))
            << index;
        EXPECT_EQ(color_harmonizer_rule_index(develop.color_harmonizer.rule), index);
    }
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerRuleIndex", 3.5));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerRuleIndex", 10.0));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerRuleIndex",
                                            std::numeric_limits<double>::max()));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerEnabled", 0.5));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 2.5));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 1.0));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 5.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 2.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 3.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerCustomNodeCount", 4.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerAnchorHueDegrees", 0.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerAnchorHueDegrees", 360.0));
    EXPECT_DOUBLE_EQ(develop.color_harmonizer.anchor_hue, 1.0);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerAnchorHueDegrees", -0.01));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerAnchorHueDegrees", 360.01));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength", 0.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength", 1.0));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength", -0.01));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerPullWidth", 0.24));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerNodeSaturation0", 2.01));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerSmoothing", 0.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerSmoothing", 1.25));
    EXPECT_DOUBLE_EQ(develop.color_harmonizer.smoothing, 1.25);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerSmoothing", -0.01));
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerSmoothing", 2.01));
    EXPECT_FALSE(apply_develop_field_strict(develop, "unknownHarmonizer", 1.0));
    const DevelopParams before_nan = develop;
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength",
                                            std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(develop, before_nan);
    EXPECT_FALSE(apply_develop_field_strict(develop, "colorHarmonizerPullStrength",
                                            std::numeric_limits<double>::infinity()));
    EXPECT_EQ(develop, before_nan);

    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerCustomHue0Degrees", 90.0));
    ASSERT_TRUE(reset_develop_field(develop, "colorHarmonizerCustomHue0Degrees"));
    EXPECT_DOUBLE_EQ(develop.color_harmonizer.custom_hue[0], 0.0);
    EXPECT_TRUE(develop.color_harmonizer_enabled);
    ASSERT_TRUE(reset_develop_field(develop, "colorHarmonizer"));
    EXPECT_FALSE(develop.color_harmonizer_enabled);
    EXPECT_EQ(develop.color_harmonizer, ColorHarmonizerParams{});
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerPullWidth", 1.5));
    ASSERT_TRUE(reset_develop_section(develop, "colorHarmonizer"));
    EXPECT_FALSE(develop.color_harmonizer_enabled);
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorHarmonizerNeutralProtection", 0.25));
    ASSERT_TRUE(reset_develop_section(develop, "color"));
    EXPECT_FALSE(develop.color_harmonizer_enabled);
    EXPECT_EQ(develop.color_harmonizer, ColorHarmonizerParams{});

    Recipe masked;
    masked.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    masked.masks.push_back({"mask-1", kCanonicalMaskSchemaVersion, MaskKind::kAll});
    auto parameters = color_harmonizer_to_parameters(ColorHarmonizerParams{});
    ASSERT_TRUE(parameters) << parameters.error().message;
    masked.operations.push_back({std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion, "colorharmonizer-1", true,
                                 parameters.value(), "mask-1"});
    auto restored_mask = develop_from_recipe(masked);
    ASSERT_TRUE(restored_mask) << restored_mask.error().message;
    EXPECT_EQ(restored_mask.value().masks, masked.masks);
    EXPECT_EQ(restored_mask.value().color_harmonizer_mask_id, std::optional<std::string>{"mask-1"});

    Recipe duplicate = masked;
    duplicate.operations.front().mask_id.reset();
    duplicate.operations.push_back(duplicate.operations.front());
    auto rejected_duplicate = develop_from_recipe(duplicate);
    ASSERT_FALSE(rejected_duplicate);
    EXPECT_EQ(rejected_duplicate.error().context.at("reason"),
              "duplicate_colorharmonizer_operation");

    ColorHarmonizerParams positive;
    positive.smoothing = 0.25;
    auto positive_parameters = color_harmonizer_to_parameters(positive);
    ASSERT_TRUE(positive_parameters) << positive_parameters.error().message;
    Recipe positive_recipe;
    positive_recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    positive_recipe.operations.push_back(
        {std::string(kColorHarmonizerOperationId), kColorHarmonizerOperationSchemaVersion,
         "colorharmonizer-1", true, positive_parameters.value(), std::nullopt});
    auto positive_develop = develop_from_recipe(positive_recipe);
    ASSERT_TRUE(positive_develop) << positive_develop.error().message;
    EXPECT_DOUBLE_EQ(positive_develop.value().color_harmonizer.smoothing, 0.25);
    clamp_develop(positive_develop.value());
    EXPECT_DOUBLE_EQ(positive_develop.value().color_harmonizer.smoothing, 0.25);
}

TEST(RecipeTest, ColorHarmonizerPresenceAndParameterFingerprintsDiffer)
{
    const auto fingerprint = [](const DevelopParams &params) -> std::string
    {
        auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
        EXPECT_TRUE(recipe) << recipe.error().message;
        if (!recipe)
        {
            return {};
        }
        auto serialized = serialize_recipe(recipe.value());
        EXPECT_TRUE(serialized) << serialized.error().message;
        return serialized ? serialized.value() : std::string{};
    };

    DevelopParams baseline;
    const auto absent = fingerprint(baseline);
    baseline.color_harmonizer_enabled = true;
    const auto present = fingerprint(baseline);
    EXPECT_NE(absent, present);
    EXPECT_NE(present.find("ravo.color.colorharmonizer"), std::string::npos);

    const std::array mutations{
        std::pair{"colorHarmonizerRuleIndex", 4.0},
        std::pair{"colorHarmonizerAnchorHueDegrees", 90.0},
        std::pair{"colorHarmonizerPullStrength", 0.2},
        std::pair{"colorHarmonizerNeutralProtection", 0.75},
        std::pair{"colorHarmonizerPullWidth", 1.5},
        std::pair{"colorHarmonizerSmoothing", 0.5},
        std::pair{"colorHarmonizerCustomHue0Degrees", 12.0},
        std::pair{"colorHarmonizerCustomNodeCount", 3.0},
        std::pair{"colorHarmonizerNodeSaturation0", 1.25},
    };
    for (const auto &[field, value] : mutations)
    {
        DevelopParams edited = baseline;
        ASSERT_TRUE(apply_develop_field_strict(edited, field, value)) << field;
        EXPECT_NE(fingerprint(edited), present) << field;
    }
    ASSERT_TRUE(reset_develop_field(baseline, "colorHarmonizer"));
    EXPECT_EQ(fingerprint(baseline), absent);
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

} // namespace
} // namespace ravo
