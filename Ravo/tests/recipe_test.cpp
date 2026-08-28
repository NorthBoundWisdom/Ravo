#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
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
    EXPECT_EQ(upgraded.value().operations[1].schema_version, kExposureOperationSchemaVersion);
    auto exposure = exposure_from_parameters(upgraded.value().operations[1].parameters);
    ASSERT_TRUE(exposure) << exposure.error().message;
    EXPECT_EQ(exposure.value().mode, kExposureModeManual);
    EXPECT_DOUBLE_EQ(exposure.value().black, 0.0);
    EXPECT_DOUBLE_EQ(exposure.value().exposure_ev, 0.5);
    EXPECT_DOUBLE_EQ(exposure.value().deflicker_percentile, kExposureDeflickerPercentileDefault);
    EXPECT_DOUBLE_EQ(exposure.value().deflicker_target_ev, kExposureDeflickerTargetEvDefault);
    EXPECT_FALSE(exposure.value().compensate_exposure_bias);
    EXPECT_FALSE(exposure.value().compensate_highlight_preservation);
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

TEST(RecipeTest, ExposureSchemaV2IsExplicitAndRejectsAmbiguousParameters)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kExposureOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kExposureOperationSchemaVersion);
    ASSERT_EQ(descriptor->parameters.size(), 7U);
    EXPECT_TRUE(std::all_of(descriptor->parameters.begin(), descriptor->parameters.end(),
                            [](const ParameterRule &rule) { return rule.required; }));

    Recipe recipe;
    recipe.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    ExposureParams params;
    params.black = -1.0 / 4096.0;
    params.exposure_ev = 18.0;
    params.deflicker_percentile = 100.0;
    params.deflicker_target_ev = -18.0;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-1", true, exposure_to_parameters(params), std::nullopt});
    ASSERT_TRUE(validate_recipe(recipe, registry.value()));

    auto invalid_mode = recipe;
    invalid_mode.operations.front().parameters["mode"] = ParameterValue{"automatic"};
    auto mode_result = validate_recipe(invalid_mode, registry.value());
    ASSERT_FALSE(mode_result);
    EXPECT_EQ(mode_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(mode_result.error().context.at("parameter"), "mode");

    auto invalid_boolean = recipe;
    invalid_boolean.operations.front().parameters["compensate_exposure_bias"] =
        ParameterValue{std::int64_t{1}};
    auto boolean_result = validate_recipe(invalid_boolean, registry.value());
    ASSERT_FALSE(boolean_result);
    EXPECT_EQ(boolean_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(boolean_result.error().context.at("parameter"), "compensate_exposure_bias");

    auto invalid_denominator = recipe;
    invalid_denominator.operations.front().parameters["black"] = ParameterValue{1.0};
    invalid_denominator.operations.front().parameters["exposure_ev"] = ParameterValue{0.0};
    auto denominator_result = validate_recipe(invalid_denominator, registry.value());
    ASSERT_FALSE(denominator_result);
    EXPECT_EQ(denominator_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(denominator_result.error().context.at("reason"), "invalid_exposure_denominator");

    auto non_finite = recipe;
    non_finite.operations.front().parameters["exposure_ev"] =
        ParameterValue{std::numeric_limits<double>::quiet_NaN()};
    auto finite_result = validate_recipe(non_finite, registry.value());
    ASSERT_FALSE(finite_result);
    EXPECT_EQ(finite_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(finite_result.error().context.at("parameter"), "exposure_ev");
}

TEST(RecipeTest, ColorContrastSchemaV2AndExistingV1RecipesNormalizeDeterministically)
{
    ColorContrastParams params;
    params.a_steepness = 2.6;
    params.a_offset = -12.5;
    params.b_steepness = 2.5;
    params.b_offset = 7.25;
    params.unbound = false;
    const auto encoded = color_contrast_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 7U);
    EXPECT_EQ(std::get<std::string>(encoded.value().at("working_space").value),
              kColorContrastWorkingSpaceLabD50);
    EXPECT_EQ(std::get<std::string>(encoded.value().at("algorithm").value),
              kColorContrastAlgorithmAxisAffineV2);
    const auto decoded = color_contrast_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_EQ(registry.value().descriptors().size(), kPhase1OperationCount);
    const auto *descriptor = registry.value().find(kColorContrastOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_EQ(descriptor->parameters.size(), 7U);
    EXPECT_TRUE(std::all_of(descriptor->parameters.begin(), descriptor->parameters.end(),
                            [](const ParameterRule &rule) { return rule.required; }));
    EXPECT_FALSE(descriptor->supports_mask);
    EXPECT_TRUE(descriptor->cpu_reference_available);

    Recipe existing;
    existing.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    existing.operations.push_back({std::string(kColorContrastOperationId),
                                   1,
                                   "colorcontrast-v1",
                                   true,
                                   {{"amount", ParameterValue{0.25}}},
                                   std::nullopt});
    EXPECT_TRUE(validate_recipe(existing, registry.value()));
    const auto upgraded = upgrade_recipe(existing);
    ASSERT_TRUE(upgraded) << upgraded.error().message;
    ASSERT_EQ(upgraded.value().operations.size(), 1U);
    const auto &normalized = upgraded.value().operations.front();
    EXPECT_EQ(normalized.schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_TRUE(normalized.enabled);
    const auto compatible = color_contrast_from_parameters(normalized.parameters);
    ASSERT_TRUE(compatible) << compatible.error().message;
    EXPECT_EQ(compatible.value(), (ColorContrastParams{1.25, 0.0, 1.25, 0.0, true}));

    auto zero = existing.operations.front();
    zero.parameters["amount"] = ParameterValue{0.0};
    ASSERT_TRUE(upgrade_color_contrast_operation(zero));
    EXPECT_EQ(zero.schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_FALSE(zero.enabled) << "the existing v1 zero control must retain its skip semantics";
    const auto zero_params = color_contrast_from_parameters(zero.parameters);
    ASSERT_TRUE(zero_params) << zero_params.error().message;
    EXPECT_EQ(zero_params.value(), ColorContrastParams{});

    auto invalid_v1 = existing.operations.front();
    invalid_v1.parameters.emplace("unknown", ParameterValue{0.0});
    const auto rejected_upgrade = upgrade_color_contrast_operation(invalid_v1);
    ASSERT_FALSE(rejected_upgrade);
    EXPECT_EQ(rejected_upgrade.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_v1.schema_version, 1);
    ASSERT_EQ(invalid_v1.parameters.size(), 2U);
    ASSERT_NE(invalid_v1.parameters.find("amount"), invalid_v1.parameters.end());
    ASSERT_NE(invalid_v1.parameters.find("unknown"), invalid_v1.parameters.end());
    EXPECT_DOUBLE_EQ(std::get<double>(invalid_v1.parameters.at("amount").value), 0.25);
    EXPECT_DOUBLE_EQ(std::get<double>(invalid_v1.parameters.at("unknown").value), 0.0);

    const auto expect_invalid = [](auto parameters)
    {
        const auto result = color_contrast_from_parameters(parameters);
        EXPECT_FALSE(result);
        if (!result)
        {
            EXPECT_EQ(result.error().code, ErrorCode::kValidation);
            EXPECT_EQ(result.error().context.at("reason"), "invalid_colorcontrast_parameters");
        }
    };
    auto invalid = encoded.value();
    invalid.emplace("unknown", ParameterValue{0.0});
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid.erase("a_offset");
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["working_space"] = ParameterValue{"linear_rec709"};
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["algorithm"] = ParameterValue{"symmetric_saturation"};
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["a_steepness"] = ParameterValue{kColorContrastSteepnessMin - 0.01};
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["b_steepness"] = ParameterValue{kColorContrastSteepnessMax + 0.01};
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["a_offset"] = ParameterValue{std::numeric_limits<double>::infinity()};
    expect_invalid(invalid);
    invalid = encoded.value();
    invalid["unbound"] = ParameterValue{std::int64_t{1}};
    expect_invalid(invalid);
}

TEST(RecipeTest, ColorHarmonizerSchemaRoundTripsTheTwoReal0176ParameterStates)
{
    // Exact little-endian decode of records 12 and 13 in the frozen 0176 XMP.
    // Record 12 is the post-init default, including the four custom hue knots.
    const ColorHarmonizerParams record12;
    auto encoded = color_harmonizer_to_parameters(record12);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(encoded.value().size(), 17U);
    auto decoded = color_harmonizer_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), record12);

    ColorHarmonizerParams record13;
    record13.rule = ColorHarmonizerRule::kSplitComplementary;
    record13.anchor_hue = 0.55000001192092896;
    record13.pull_strength = 0.81999999284744263;
    record13.pull_width = 1.8400000333786011;
    record13.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    encoded = color_harmonizer_to_parameters(record13);
    ASSERT_TRUE(encoded) << encoded.error().message;
    decoded = color_harmonizer_from_parameters(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), record13);

    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    EXPECT_EQ(registry.value().descriptors().size(), kPhase1OperationCount);
    const auto *descriptor = registry.value().find(kColorHarmonizerOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kColorHarmonizerOperationSchemaVersion);
    ASSERT_EQ(descriptor->parameters.size(), 17U);
    EXPECT_TRUE(std::ranges::all_of(descriptor->parameters,
                                    [](const ParameterRule &rule) { return rule.required; }));
    EXPECT_TRUE(descriptor->supports_mask);
    EXPECT_TRUE(descriptor->cpu_reference_available);

    const std::array<std::string_view, 17> expected_names{
        "working_space",     "algorithm",         "rule",
        "anchor_hue",        "pull_strength",     "neutral_protection",
        "pull_width",        "custom_hue_0",      "custom_hue_1",
        "custom_hue_2",      "custom_hue_3",      "num_custom_nodes",
        "node_saturation_0", "node_saturation_1", "node_saturation_2",
        "node_saturation_3", "smoothing"};
    for (std::size_t index = 0U; index < expected_names.size(); ++index)
    {
        EXPECT_EQ(descriptor->parameters[index].name, expected_names[index]);
    }

    auto checker = color_checker_to_parameters(ColorCheckerParams{});
    ASSERT_TRUE(checker) << checker.error().message;
    Recipe canonical;
    canonical.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    canonical.operations.push_back({std::string(kColorCheckerOperationId),
                                    kColorCheckerOperationSchemaVersion, "colorchecker-1", true,
                                    std::move(checker).value(), std::nullopt});
    canonical.operations.push_back({std::string(kColorHarmonizerOperationId),
                                    kColorHarmonizerOperationSchemaVersion, "colorharmonizer-1",
                                    true, encoded.value(), std::nullopt});
    canonical.operations.push_back(
        {std::string(kColorBalanceOperationId), kColorBalanceOperationSchemaVersion,
         "colorbalance-1", true, color_balance_to_parameters(ColorBalanceParams{}), std::nullopt});
    ASSERT_TRUE(validate_recipe(canonical, registry.value()));
    const auto serialized = serialize_recipe(canonical);
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto restored = parse_recipe_json(serialized.value());
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().operations.size(), 3U);
    EXPECT_EQ(restored.value().operations[0].id, kColorCheckerOperationId);
    EXPECT_EQ(restored.value().operations[1].id, kColorHarmonizerOperationId);
    EXPECT_EQ(restored.value().operations[2].id, kColorBalanceOperationId);
}

TEST(RecipeTest, ColorHarmonizerRejectsEveryUnfrozenSchemaAndPresentationState)
{
    const std::array<std::pair<ColorHarmonizerRule, std::string_view>, 10> rules{
        std::pair{ColorHarmonizerRule::kMonochromatic, "monochromatic"},
        std::pair{ColorHarmonizerRule::kAnalogous, "analogous"},
        std::pair{ColorHarmonizerRule::kAnalogousComplementary, "analogous_complementary"},
        std::pair{ColorHarmonizerRule::kComplementary, "complementary"},
        std::pair{ColorHarmonizerRule::kSplitComplementary, "split_complementary"},
        std::pair{ColorHarmonizerRule::kDyad, "dyad"},
        std::pair{ColorHarmonizerRule::kTriad, "triad"},
        std::pair{ColorHarmonizerRule::kTetrad, "tetrad"},
        std::pair{ColorHarmonizerRule::kSquare, "square"},
        std::pair{ColorHarmonizerRule::kCustom, "custom"},
    };
    for (const auto &[rule, name] : rules)
    {
        ColorHarmonizerParams params;
        params.rule = rule;
        const auto encoded = color_harmonizer_to_parameters(params);
        ASSERT_TRUE(encoded) << encoded.error().message;
        EXPECT_EQ(std::get<std::string>(encoded.value().at("rule").value), name);
        const auto decoded = color_harmonizer_from_parameters(encoded.value());
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value(), params);
    }

    auto canonical = color_harmonizer_to_parameters(ColorHarmonizerParams{});
    ASSERT_TRUE(canonical) << canonical.error().message;
    const auto expect_invalid = [](const auto &parameters, const std::string_view parameter = {})
    {
        const auto result = color_harmonizer_from_parameters(parameters);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().context.at("reason"), "invalid_colorharmonizer_parameters");
        if (!parameter.empty())
        {
            EXPECT_EQ(result.error().context.at("parameter"), parameter);
        }
    };

    auto invalid = canonical.value();
    invalid.erase("anchor_hue");
    expect_invalid(invalid);
    invalid = canonical.value();
    invalid["multi_priority"] = ParameterValue{std::int64_t{0}};
    expect_invalid(invalid, "multi_priority");
    invalid = canonical.value();
    invalid["working_space"] = ParameterValue{"linear_rec709"};
    expect_invalid(invalid, "working_space");
    invalid = canonical.value();
    invalid["algorithm"] = ParameterValue{"nearest_hue"};
    expect_invalid(invalid, "algorithm");
    invalid = canonical.value();
    invalid["rule"] = ParameterValue{"automatic"};
    expect_invalid(invalid, "rule");

    const std::array bounds{
        std::tuple{"anchor_hue", -0.01, 1.01},         std::tuple{"pull_strength", -0.01, 1.01},
        std::tuple{"neutral_protection", -0.01, 1.01}, std::tuple{"pull_width", 0.24, 4.01},
        std::tuple{"custom_hue_0", -0.01, 1.01},       std::tuple{"custom_hue_1", -0.01, 1.01},
        std::tuple{"custom_hue_2", -0.01, 1.01},       std::tuple{"custom_hue_3", -0.01, 1.01},
        std::tuple{"node_saturation_0", -0.01, 2.01},  std::tuple{"node_saturation_1", -0.01, 2.01},
        std::tuple{"node_saturation_2", -0.01, 2.01},  std::tuple{"node_saturation_3", -0.01, 2.01},
        std::tuple{"smoothing", -0.01, 2.01},
    };
    for (const auto &[name, below, above] : bounds)
    {
        invalid = canonical.value();
        invalid[name] = ParameterValue{below};
        expect_invalid(invalid, name);
        invalid[name] = ParameterValue{above};
        expect_invalid(invalid, name);
        invalid[name] = ParameterValue{std::numeric_limits<double>::quiet_NaN()};
        expect_invalid(invalid, name);
    }
    invalid = canonical.value();
    invalid["num_custom_nodes"] = ParameterValue{std::int64_t{1}};
    expect_invalid(invalid, "num_custom_nodes");
    invalid["num_custom_nodes"] = ParameterValue{std::int64_t{5}};
    expect_invalid(invalid, "num_custom_nodes");
    invalid["num_custom_nodes"] = ParameterValue{4.0};
    expect_invalid(invalid, "num_custom_nodes");

    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    Recipe masked;
    masked.asset = {"asset-1", "file:///fixture.raw", std::nullopt};
    masked.masks.push_back({"mask-1", 1, MaskKind::kAll});
    masked.operations.push_back({std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion, "colorharmonizer-mask",
                                 true, canonical.value(), "mask-1"});
    const auto accepted = validate_recipe(masked, registry.value());
    EXPECT_TRUE(accepted) << accepted.error().message;
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
    auto rejected = apply_develop_field_strict(params, "exposure", 19.0);
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

TEST(RecipeTest, DevelopFieldAssignmentAndResetCoverTheFullExposureV2Contract)
{
    DevelopParams params;
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureMode", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureBlack", -0.125));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposure", 1.25));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureDeflickerPercentile", 72.5));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureDeflickerTarget", -3.25));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureCompensateBias", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(params, "exposureCompensateHighlight", 1.0));
    EXPECT_EQ(params.exposure_mode, kExposureModeDeflicker);
    EXPECT_DOUBLE_EQ(params.exposure_black, -0.125);
    EXPECT_DOUBLE_EQ(params.exposure_ev, 1.25);
    EXPECT_DOUBLE_EQ(params.exposure_deflicker_percentile, 72.5);
    EXPECT_DOUBLE_EQ(params.exposure_deflicker_target_ev, -3.25);
    EXPECT_TRUE(params.exposure_compensate_exposure_bias);
    EXPECT_TRUE(params.exposure_compensate_highlight_preservation);

    for (const auto &[name, value] : std::array<std::pair<std::string_view, double>, 6>{
             std::pair{"exposureMode", 0.5},
             std::pair{"exposureBlack", std::numeric_limits<double>::infinity()},
             std::pair{"exposureDeflickerPercentile", 100.01},
             std::pair{"exposureDeflickerTarget", -18.01}, std::pair{"exposureCompensateBias", 0.5},
             std::pair{"exposureCompensateHighlight", -1.0}})
    {
        auto invalid = apply_develop_field_strict(params, name, value);
        EXPECT_FALSE(invalid) << name;
    }

    for (const std::string_view name :
         {"exposureMode", "exposureBlack", "exposure", "exposureDeflickerPercentile",
          "exposureDeflickerTarget", "exposureCompensateBias", "exposureCompensateHighlight"})
    {
        EXPECT_TRUE(reset_develop_field(params, name)) << name;
    }
    const DevelopParams identity;
    EXPECT_EQ(params.exposure_mode, identity.exposure_mode);
    EXPECT_DOUBLE_EQ(params.exposure_black, identity.exposure_black);
    EXPECT_DOUBLE_EQ(params.exposure_ev, identity.exposure_ev);
    EXPECT_DOUBLE_EQ(params.exposure_deflicker_percentile, identity.exposure_deflicker_percentile);
    EXPECT_DOUBLE_EQ(params.exposure_deflicker_target_ev, identity.exposure_deflicker_target_ev);
    EXPECT_EQ(params.exposure_compensate_exposure_bias, identity.exposure_compensate_exposure_bias);
    EXPECT_EQ(params.exposure_compensate_highlight_preservation,
              identity.exposure_compensate_highlight_preservation);
}

TEST(RecipeTest, DevelopRoundTripRetainsTheFullExposureV2Contract)
{
    DevelopParams params;
    params.exposure_mode = std::string(kExposureModeDeflicker);
    params.exposure_black = -1.0 / 4096.0;
    params.exposure_ev = 0.7;
    params.exposure_deflicker_percentile = 72.5;
    params.exposure_deflicker_target_ev = -3.25;
    params.exposure_compensate_exposure_bias = true;
    params.exposure_compensate_highlight_preservation = true;

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto *operation = operation_by_id(recipe.value(), kExposureOperationId);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->schema_version, kExposureOperationSchemaVersion);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().exposure_mode, params.exposure_mode);
    EXPECT_DOUBLE_EQ(restored.value().exposure_black, params.exposure_black);
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, params.exposure_ev);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_percentile,
                     params.exposure_deflicker_percentile);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_target_ev,
                     params.exposure_deflicker_target_ev);
    EXPECT_TRUE(restored.value().exposure_compensate_exposure_bias);
    EXPECT_TRUE(restored.value().exposure_compensate_highlight_preservation);
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

TEST(RecipeTest, ProfileGammaIsExplicitAndImmediatelyPrecedesInputColour)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    const auto *descriptor = registry.value().find(kProfileGammaOperationId);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->parameter_schema_version, kProfileGammaOperationSchemaVersion);
    ASSERT_EQ(descriptor->parameters.size(), 7U);
    EXPECT_EQ(descriptor->parameters[0].name, "mode");
    EXPECT_EQ(descriptor->parameters[1].name, "linear");
    EXPECT_EQ(descriptor->parameters[2].name, "gamma");
    EXPECT_EQ(descriptor->parameters[3].name, "dynamic_range");
    EXPECT_EQ(descriptor->parameters[4].name, "grey_point");
    EXPECT_EQ(descriptor->parameters[5].name, "shadows_range");
    EXPECT_EQ(descriptor->parameters[6].name, "security_factor");
    EXPECT_EQ(descriptor->parameters[1].minimum, kProfileGammaLinearMin);
    EXPECT_EQ(descriptor->parameters[1].maximum, kProfileGammaLinearMax);
    EXPECT_EQ(descriptor->parameters[3].minimum, kProfileGammaDynamicRangeMin);
    EXPECT_EQ(descriptor->parameters[3].maximum, kProfileGammaDynamicRangeMax);
    EXPECT_EQ(descriptor->parameters[6].minimum, kProfileGammaSecurityFactorMin);
    EXPECT_EQ(descriptor->parameters[6].maximum, kProfileGammaSecurityFactorMax);

    ProfileGammaParams defaults;
    EXPECT_TRUE(defaults.is_default());
    EXPECT_EQ(defaults.mode, kProfileGammaModeLogarithmic);
    EXPECT_DOUBLE_EQ(defaults.linear, 0.1);
    EXPECT_DOUBLE_EQ(defaults.gamma, 0.45);
    EXPECT_DOUBLE_EQ(defaults.dynamic_range, 10.0);
    EXPECT_DOUBLE_EQ(defaults.grey_point, 18.0);
    EXPECT_DOUBLE_EQ(defaults.shadows_range, -5.0);
    EXPECT_DOUBLE_EQ(defaults.security_factor, 0.0);

    ProfileGammaParams invalid_profile_gamma;
    invalid_profile_gamma.mode = "automatic";
    const auto invalid_profile_gamma_parameters =
        profile_gamma_to_parameters(invalid_profile_gamma);
    ASSERT_FALSE(invalid_profile_gamma_parameters);
    EXPECT_EQ(invalid_profile_gamma_parameters.error().code, ErrorCode::kValidation);

    DevelopParams invalid_profile_gamma_recipe;
    invalid_profile_gamma_recipe.profile_gamma_enabled = true;
    invalid_profile_gamma_recipe.profile_gamma = invalid_profile_gamma;
    const auto invalid_profile_gamma_from_develop = recipe_from_develop(
        {"asset-1", "file:///fixture.raw", std::nullopt}, invalid_profile_gamma_recipe);
    ASSERT_FALSE(invalid_profile_gamma_from_develop);
    EXPECT_EQ(invalid_profile_gamma_from_develop.error().code, ErrorCode::kValidation);

    DevelopParams disabled_invalid_profile_gamma;
    disabled_invalid_profile_gamma.profile_gamma.mode = "automatic";
    clamp_develop(disabled_invalid_profile_gamma);
    EXPECT_EQ(disabled_invalid_profile_gamma.profile_gamma.mode, kProfileGammaModeLogarithmic);

    DevelopParams params;
    ASSERT_TRUE(apply_develop_field(params, "profileGammaEnabled", 1.0));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaModeIndex", 1.0));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaLinear", 0.2));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaGamma", 0.8));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaDynamicRange", 14.0));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaGreyPoint", 20.0));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaShadowsRange", -7.0));
    ASSERT_TRUE(apply_develop_field(params, "profileGammaSecurityFactor", 12.5));
    EXPECT_TRUE(params.profile_gamma_enabled);
    EXPECT_EQ(params.profile_gamma.mode, kProfileGammaModeGamma);

    DevelopParams strict_params;
    const auto strict_rejected =
        apply_develop_field_strict(strict_params, "profileGammaDynamicRange", 0.0);
    ASSERT_FALSE(strict_rejected);
    EXPECT_EQ(strict_rejected.error().code, ErrorCode::kInvalidArgument);
    EXPECT_DOUBLE_EQ(strict_params.profile_gamma.dynamic_range, kProfileGammaDynamicRangeDefault);
    ASSERT_TRUE(apply_develop_field_strict(strict_params, "profileGammaDynamicRange", 12.0));
    EXPECT_DOUBLE_EQ(strict_params.profile_gamma.dynamic_range, 12.0);

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_EQ(recipe.value().operations.size(), 3U);
    EXPECT_EQ(recipe.value().operations[0].id, kProfileGammaOperationId);
    EXPECT_EQ(recipe.value().operations[1].id, "ravo.color.input");
    EXPECT_EQ(recipe.value().operations.back().id, "ravo.color.output");
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));

    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(validate_recipe(parsed.value(), registry.value()));

    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().profile_gamma_enabled);
    EXPECT_EQ(restored.value().profile_gamma, params.profile_gamma);

    auto missing = recipe.value();
    auto *missing_profile_gamma = operation_by_id(missing, kProfileGammaOperationId);
    ASSERT_NE(missing_profile_gamma, nullptr);
    missing_profile_gamma->parameters.erase("gamma");
    const auto missing_valid = validate_recipe(missing, registry.value());
    ASSERT_FALSE(missing_valid);
    EXPECT_EQ(missing_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(missing_valid.error().context.at("parameter"), "gamma");

    auto unknown = recipe.value();
    auto *unknown_profile_gamma = operation_by_id(unknown, kProfileGammaOperationId);
    ASSERT_NE(unknown_profile_gamma, nullptr);
    unknown_profile_gamma->parameters.emplace("unknown", ParameterValue{0.0});
    const auto unknown_valid = validate_recipe(unknown, registry.value());
    ASSERT_FALSE(unknown_valid);
    EXPECT_EQ(unknown_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(unknown_valid.error().context.at("parameter"), "unknown");

    auto invalid_mode = recipe.value();
    auto *invalid_mode_profile_gamma = operation_by_id(invalid_mode, kProfileGammaOperationId);
    ASSERT_NE(invalid_mode_profile_gamma, nullptr);
    invalid_mode_profile_gamma->parameters["mode"] = ParameterValue{"automatic"};
    const auto invalid_mode_valid = validate_recipe(invalid_mode, registry.value());
    ASSERT_FALSE(invalid_mode_valid);
    EXPECT_EQ(invalid_mode_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_mode_valid.error().context.at("parameter"), "mode");

    auto out_of_range = recipe.value();
    auto *out_of_range_profile_gamma = operation_by_id(out_of_range, kProfileGammaOperationId);
    ASSERT_NE(out_of_range_profile_gamma, nullptr);
    out_of_range_profile_gamma->parameters["dynamic_range"] = ParameterValue{32.1};
    const auto out_of_range_valid = validate_recipe(out_of_range, registry.value());
    ASSERT_FALSE(out_of_range_valid);
    EXPECT_EQ(out_of_range_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(out_of_range_valid.error().context.at("parameter"), "dynamic_range");

    auto non_finite = recipe.value();
    auto *non_finite_profile_gamma = operation_by_id(non_finite, kProfileGammaOperationId);
    ASSERT_NE(non_finite_profile_gamma, nullptr);
    non_finite_profile_gamma->parameters["grey_point"] =
        ParameterValue{std::numeric_limits<double>::quiet_NaN()};
    const auto non_finite_valid = validate_recipe(non_finite, registry.value());
    ASSERT_FALSE(non_finite_valid);
    EXPECT_EQ(non_finite_valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(non_finite_valid.error().context.at("parameter"), "grey_point");

    auto duplicate = recipe.value();
    auto duplicate_profile_gamma = duplicate.operations.front();
    duplicate_profile_gamma.instance_id = "profilegamma-2";
    duplicate.operations.insert(duplicate.operations.begin() + 1,
                                std::move(duplicate_profile_gamma));
    const auto duplicate_valid = validate_recipe(duplicate, registry.value());
    ASSERT_FALSE(duplicate_valid);
    EXPECT_EQ(duplicate_valid.error().code, ErrorCode::kConflict);

    auto misordered = recipe.value();
    std::swap(misordered.operations[0], misordered.operations[1]);
    const auto misordered_valid = validate_recipe(misordered, registry.value());
    ASSERT_FALSE(misordered_valid);
    EXPECT_EQ(misordered_valid.error().code, ErrorCode::kValidation);

    auto disabled = recipe.value();
    disabled.operations.front().enabled = false;
    ASSERT_TRUE(validate_recipe(disabled, registry.value()));
    auto disabled_develop = develop_from_recipe(disabled);
    ASSERT_TRUE(disabled_develop) << disabled_develop.error().message;
    EXPECT_FALSE(disabled_develop.value().profile_gamma_enabled);
    EXPECT_TRUE(disabled_develop.value().profile_gamma.is_default());

    DevelopParams gamma_one;
    gamma_one.profile_gamma_enabled = true;
    gamma_one.profile_gamma.mode = std::string(kProfileGammaModeGamma);
    gamma_one.profile_gamma.gamma = 1.0;
    auto gamma_one_recipe =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, gamma_one);
    ASSERT_TRUE(gamma_one_recipe) << gamma_one_recipe.error().message;
    EXPECT_NE(operation_by_id(gamma_one_recipe.value(), kProfileGammaOperationId), nullptr);

    DevelopParams linear_one;
    linear_one.profile_gamma_enabled = true;
    linear_one.profile_gamma.mode = std::string(kProfileGammaModeGamma);
    linear_one.profile_gamma.linear = 1.0;
    auto linear_one_recipe =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, linear_one);
    ASSERT_TRUE(linear_one_recipe) << linear_one_recipe.error().message;
    EXPECT_NE(operation_by_id(linear_one_recipe.value(), kProfileGammaOperationId), nullptr);

    DevelopParams disabled_identity;
    disabled_identity.profile_gamma.gamma = 1.0;
    EXPECT_TRUE(disabled_identity.is_identity());
    DevelopParams enabled_default;
    enabled_default.profile_gamma_enabled = true;
    EXPECT_FALSE(enabled_default.is_identity());

    EXPECT_TRUE(reset_develop_field(params, "profileGammaGamma"));
    EXPECT_DOUBLE_EQ(params.profile_gamma.gamma, kProfileGammaGammaDefault);
    const ProfileGammaParams session_profile_gamma = params.profile_gamma;
    EXPECT_TRUE(reset_develop_field(params, "profileGammaEnabled"));
    EXPECT_FALSE(params.profile_gamma_enabled);
    EXPECT_EQ(params.profile_gamma, session_profile_gamma);
    ASSERT_TRUE(apply_develop_field(params, "profileGammaEnabled", 1.0));
    EXPECT_TRUE(params.profile_gamma_enabled);
    EXPECT_EQ(params.profile_gamma, session_profile_gamma);
    ASSERT_TRUE(apply_develop_field(params, "profileGammaEnabled", 0.0));
    auto disabled_canonical =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(disabled_canonical) << disabled_canonical.error().message;
    EXPECT_EQ(operation_by_id(disabled_canonical.value(), kProfileGammaOperationId), nullptr);
    auto disabled_roundtrip = develop_from_recipe(disabled_canonical.value());
    ASSERT_TRUE(disabled_roundtrip) << disabled_roundtrip.error().message;
    EXPECT_FALSE(disabled_roundtrip.value().profile_gamma_enabled);
    EXPECT_TRUE(disabled_roundtrip.value().profile_gamma.is_default());
    EXPECT_TRUE(reset_develop_section(params, "profileGamma"));
    EXPECT_FALSE(params.profile_gamma_enabled);
    EXPECT_TRUE(params.profile_gamma.is_default());

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
    develop.velvia = 0.25;
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

TEST(RecipeTest, DevelopChangeSummaryReportsSignedSliderDeltas)
{
    DevelopParams before;
    DevelopParams after;
    after.highlights = 0.3;
    after.shadows = -0.2;
    const auto changes = develop_change_summary(before, after);
    ASSERT_EQ(changes.size(), 2U);
    EXPECT_EQ(changes[0].field, "highlights");
    EXPECT_EQ(changes[0].value, "+3");
    EXPECT_EQ(changes[1].field, "shadows");
    EXPECT_EQ(changes[1].value, "-2");

    DevelopParams reset_from = after;
    const auto reset = develop_change_summary(reset_from, DevelopParams{});
    ASSERT_EQ(reset.size(), 1U);
    EXPECT_EQ(reset.front().field, "reset");
}

TEST(RecipeTest, SectionEffectBypassKeepsParametersAndDisablesOperations)
{
    DevelopParams params;
    EXPECT_FALSE(develop_section_modified(params, "inputProfile"));
    EXPECT_TRUE(develop_section_effect_enabled(params, "inputProfile"));
    params.input_color.working_profile = std::string(kInputProfileLinearRec2020);
    EXPECT_TRUE(develop_section_modified(params, "inputProfile"));
    ASSERT_TRUE(set_develop_section_effect_enabled(params, "inputProfile", false));
    EXPECT_FALSE(develop_section_effect_enabled(params, "inputProfile"));

    params.exposure_ev = 0.75;
    ASSERT_TRUE(set_develop_section_effect_enabled(params, "light", false));
    EXPECT_TRUE(develop_section_modified(params, "light"));
    EXPECT_FALSE(develop_section_effect_enabled(params, "light"));

    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto *input = operation_by_id(recipe.value(), "ravo.color.input");
    ASSERT_NE(input, nullptr);
    EXPECT_FALSE(input->enabled);
    const auto *exposure = operation_by_id(recipe.value(), kExposureOperationId);
    ASSERT_NE(exposure, nullptr);
    EXPECT_FALSE(exposure->enabled);

    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().input_color.working_profile, kInputProfileLinearRec2020);
    EXPECT_FALSE(develop_section_effect_enabled(restored.value(), "inputProfile"));
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, 0.75);
    EXPECT_FALSE(develop_section_effect_enabled(restored.value(), "light"));

    ASSERT_TRUE(reset_develop_section(params, "inputProfile"));
    EXPECT_FALSE(develop_section_modified(params, "inputProfile"));
    EXPECT_TRUE(develop_section_effect_enabled(params, "inputProfile"));
}

} // namespace
} // namespace ravo
