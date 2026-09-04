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

    const std::vector<ToneCurvePoint> s_curve{{0.0, 0.0}, {0.3, 0.62}, {1.0, 1.0}};
    const double hermite = evaluate_tone_curve(s_curve, 0.55);
    const double centripetal =
        evaluate_tone_curve(s_curve, 0.55, kToneCurveInterpolationCatmullRom);
    const double cubic = evaluate_tone_curve(s_curve, 0.55, kToneCurveInterpolationCubicSpline);
    EXPECT_NE(hermite, centripetal);
    EXPECT_NE(hermite, cubic);
    EXPECT_GE(hermite, 0.0);
    EXPECT_LE(hermite, 1.0);
    for (const auto interpolation :
         {kToneCurveInterpolationMonotoneHermite, kToneCurveInterpolationCatmullRom,
          kToneCurveInterpolationCubicSpline})
    {
        auto lut = build_tone_curve_lut(s_curve, interpolation, 257U);
        ASSERT_TRUE(lut) << lut.error().message;
        ASSERT_EQ(lut.value().size(), 257U);
        for (std::size_t index = 0; index < lut.value().size(); ++index)
        {
            const double x = static_cast<double>(index) / 257.0;
            EXPECT_EQ(lut.value()[index],
                      static_cast<float>(evaluate_tone_curve(s_curve, x, interpolation)));
        }
    }
    auto empty_lut = build_tone_curve_lut(s_curve, kToneCurveInterpolationMonotoneHermite, 0U);
    ASSERT_FALSE(empty_lut);
    EXPECT_EQ(empty_lut.error().code, ErrorCode::kInvalidArgument);
    auto oversized_lut = build_tone_curve_lut(s_curve, kToneCurveInterpolationMonotoneHermite,
                                              std::vector<float>{}.max_size() + 1U);
    ASSERT_FALSE(oversized_lut);
    EXPECT_EQ(oversized_lut.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(oversized_lut.error().context.at("reason"), "sample_count_too_large");
    auto unknown_lut = build_tone_curve_lut(s_curve, "future", 16U);
    ASSERT_FALSE(unknown_lut);
    EXPECT_EQ(unknown_lut.error().code, ErrorCode::kValidation);

    DevelopParams parametric;
    parametric.rgb_curve.parametric_shadows = 0.4;
    EXPECT_FALSE(parametric.rgb_curve.is_identity());
    EXPECT_GT(evaluate_rgb_curve_parametric(parametric.rgb_curve, 0.08), 0.08);
    auto parametric_recipe =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, parametric);
    ASSERT_TRUE(parametric_recipe) << parametric_recipe.error().message;
    ASSERT_TRUE(validate_recipe(parametric_recipe.value(), registry.value()));
    auto parametric_restored = develop_from_recipe(parametric_recipe.value());
    ASSERT_TRUE(parametric_restored) << parametric_restored.error().message;
    EXPECT_NEAR(parametric_restored.value().rgb_curve.parametric_shadows, 0.4, 1e-6);
    EXPECT_TRUE(develop_section_modified(parametric_restored.value(), "curves"));
    EXPECT_TRUE(reset_develop_section(parametric_restored.value(), "curves"));
    EXPECT_FALSE(develop_section_modified(parametric_restored.value(), "curves"));

    DevelopParams independent_lab;
    independent_lab.tone_curve = {{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}};
    independent_lab.tone_curve_a = {{0.0, 0.0}, {0.5, 0.4}, {1.0, 1.0}};
    independent_lab.tone_curve_working_space = std::string(kToneCurveWorkingSpaceLabIndependent);
    independent_lab.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
    independent_lab.tone_curve_interpolation = std::string(kToneCurveInterpolationCatmullRom);
    auto lab_recipe =
        recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, independent_lab);
    ASSERT_TRUE(lab_recipe) << lab_recipe.error().message;
    ASSERT_TRUE(validate_recipe(lab_recipe.value(), registry.value()));
    auto lab_restored = develop_from_recipe(lab_recipe.value());
    ASSERT_TRUE(lab_restored) << lab_restored.error().message;
    ASSERT_EQ(lab_restored.value().tone_curve_a.size(), 3U);
    EXPECT_NEAR(lab_restored.value().tone_curve_a[1].y, 0.4, 1e-6);
    EXPECT_EQ(lab_restored.value().tone_curve_interpolation, kToneCurveInterpolationCatmullRom);

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

TEST(RecipeTest, DisplaySrgbRgbCurveRejectsScenePoliciesDuringRecipeValidation)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;

    DevelopParams params;
    params.sigmoid_enabled = true;
    params.rgb_curve.mode = std::string(kRgbLevelsModeIndependent);
    params.rgb_curve.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    params.rgb_curve.application_space = std::string(kRgbCurveApplicationSpaceDisplaySrgb);
    params.rgb_curve.channels[0] = {{0.0, 0.0}, {0.5, 0.6}, {1.0, 1.0}};
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));
    auto *curve = operation_by_id(recipe.value(), "ravo.color.rgbcurve");
    ASSERT_NE(curve, nullptr);

    const auto rejects_policy = [&](const std::string_view parameter, const ParameterValue &value)
    {
        auto invalid = recipe.value();
        auto *invalid_curve = operation_by_id(invalid, "ravo.color.rgbcurve");
        ASSERT_NE(invalid_curve, nullptr);
        invalid_curve->parameters[std::string(parameter)] = value;
        auto result = validate_recipe(invalid, registry.value());
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().context.at("reason"), "unsupported_display_srgb_curve_policy");
    };
    rejects_policy("mode", ParameterValue{std::string(kRgbLevelsModeLinked)});
    rejects_policy("preserve_colors",
                   ParameterValue{std::string(kToneCurvePreserveColorsLuminance)});
    rejects_policy("compensate_middle_grey", ParameterValue{true});
    rejects_policy("parametric_shadows", ParameterValue{0.2});

    auto unknown_preserve = recipe.value();
    auto *unknown_curve = operation_by_id(unknown_preserve, "ravo.color.rgbcurve");
    ASSERT_NE(unknown_curve, nullptr);
    unknown_curve->parameters["preserve_colors"] = ParameterValue{"invented"};
    auto rejected_preserve = validate_recipe(unknown_preserve, registry.value());
    ASSERT_FALSE(rejected_preserve);
    EXPECT_EQ(rejected_preserve.error().code, ErrorCode::kValidation);
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

TEST(RecipeStyleTest, CanonicalTemplateRoundTripsAndAppliesOnlyTargetIdentity)
{
    DevelopParams develop;
    develop.exposure_ev = 0.75;
    Mask mask{"style-mask", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    mask.payload = CircleMask{0.5, 0.5, 0.2, 0.05};
    develop.masks.push_back(mask);
    RetouchRegion region;
    region.mask_id = mask.id;
    region.mode = RetouchMode::kFill;
    region.fill_mode = RetouchFillMode::kColor;
    region.fill_color = {0.1, 0.2, 0.3};
    develop.retouch.regions.push_back(region);
    auto recipe = recipe_from_develop({"asset-a", "file:///source-a.raw", "hash-a"}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto style =
        recipe_style_from_recipe("Warm repair", "Complete reproducible look", recipe.value());
    ASSERT_TRUE(style) << style.error().message;
    EXPECT_EQ(style.value().recipe.asset.id, kRecipeStyleAssetId);
    EXPECT_EQ(style.value().recipe.asset.input_uri, kRecipeStyleInputUri);
    EXPECT_FALSE(style.value().recipe.asset.content_hash);
    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto again = serialize_recipe_style(style.value());
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), serialized.value());
    auto parsed = parse_recipe_style_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().name, "Warm repair");
    EXPECT_EQ(parsed.value().description, "Complete reproducible look");
    EXPECT_EQ(parsed.value().recipe.operations.size(), recipe.value().operations.size());
    EXPECT_EQ(parsed.value().recipe.masks, recipe.value().masks);

    AssetDescriptor target{"asset-b", "file:///source-b.jpg", "hash-b"};
    auto applied = apply_recipe_style(parsed.value(), target);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().asset.id, target.id);
    EXPECT_EQ(applied.value().asset.input_uri, target.input_uri);
    EXPECT_EQ(applied.value().asset.content_hash, target.content_hash);
    EXPECT_EQ(applied.value().operations.size(), recipe.value().operations.size());
    EXPECT_EQ(applied.value().masks, recipe.value().masks);
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().retouch, develop.retouch);
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, 0.75);
}

TEST(RecipeStyleTest, SelectivePresetAppliesOnlyChosenModifiedFields)
{
    DevelopParams source;
    source.exposure_ev = 1.25;
    source.contrast = 0.35;
    source.saturation = 0.4;
    auto source_recipe =
        recipe_from_develop({"source", "file:///source.raw", "source-hash"}, source);
    ASSERT_TRUE(source_recipe) << source_recipe.error().message;

    auto style =
        recipe_style_from_selected_fields("Contrast and color", "Selected field overlay",
                                          source_recipe.value(), {"saturation", "contrast"});
    ASSERT_TRUE(style) << style.error().message;
    EXPECT_EQ(style.value().schema_version, kRecipeStyleSelectedSchemaVersion);
    EXPECT_EQ(style.value().selected_fields, (std::vector<std::string>{"contrast", "saturation"}));

    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_recipe_style_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().selected_fields, style.value().selected_fields);
    auto noncanonical = serialized.value();
    const auto selection = noncanonical.find("\"selected_fields\":[\"contrast\",\"saturation\"]");
    ASSERT_NE(selection, std::string::npos);
    noncanonical.replace(selection,
                         std::string("\"selected_fields\":[\"contrast\",\"saturation\"]").size(),
                         "\"selected_fields\":[\"saturation\",\"contrast\"]");
    auto rejected_selection = parse_recipe_style_json(noncanonical);
    ASSERT_FALSE(rejected_selection);
    EXPECT_EQ(rejected_selection.error().context.at("reason"),
              "noncanonical_recipe_style_selection");

    DevelopParams target;
    target.exposure_ev = -0.75;
    target.contrast = -0.2;
    target.saturation = -0.1;
    target.whites = 0.3;
    auto target_recipe =
        recipe_from_develop({"target", "file:///target.raw", "target-hash"}, target);
    ASSERT_TRUE(target_recipe) << target_recipe.error().message;
    auto applied = apply_recipe_style(parsed.value(), target_recipe.value());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().asset.id, "target");
    EXPECT_EQ(applied.value().asset.input_uri, "file:///target.raw");
    EXPECT_EQ(applied.value().asset.content_hash, "target-hash");
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().contrast, source.contrast);
    EXPECT_DOUBLE_EQ(restored.value().saturation, source.saturation);
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, target.exposure_ev);
    EXPECT_DOUBLE_EQ(restored.value().whites, target.whites);

    auto missing_target = apply_recipe_style(
        parsed.value(), AssetDescriptor{"other", "file:///other.raw", std::nullopt});
    ASSERT_FALSE(missing_target);
    EXPECT_EQ(missing_target.error().context.at("reason"),
              "selective_recipe_style_requires_target_recipe");
}

TEST(RecipeStyleTest, SelectivePresetInventoryRejectsEmptyUnknownAndDuplicateFields)
{
    DevelopParams baseline;
    DevelopParams edited;
    edited.exposure_ev = 0.5;
    edited.color_correction_enabled = true;
    edited.color_correction.highlight_a = 4.0;
    edited.demosaic_mode = std::string(kDemosaicModePpg);
    edited.perspective_vertical = 0.2;
    edited.texture.strength = 0.3;
    edited.velvia_present = true;
    edited.velvia_enabled = true;
    edited.velvia.strength = 35.0;
    edited.lut3d_present = true;
    edited.lut3d_enabled = true;
    edited.lut3d.file_path = "look.cube";
    const auto changes = develop_modified_fields(baseline, edited);
    const std::set<std::string> names = [&changes]
    {
        std::set<std::string> result;
        for (const auto &change : changes)
            result.insert(change.field);
        return result;
    }();
    EXPECT_TRUE(names.contains("exposure"));
    EXPECT_TRUE(names.contains("colorCorrection"));
    EXPECT_TRUE(names.contains("demosaic"));
    EXPECT_TRUE(names.contains("perspective"));
    EXPECT_TRUE(names.contains("texture"));
    EXPECT_TRUE(names.contains("velvia"));
    EXPECT_TRUE(names.contains("lut3d"));
    EXPECT_FALSE(names.contains("saturation"));

    DevelopParams overlaid;
    auto applied = apply_develop_selected_fields(
        overlaid, edited, {"demosaic", "perspective", "texture", "velvia", "lut3d"});
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(overlaid.demosaic_mode, edited.demosaic_mode);
    EXPECT_DOUBLE_EQ(overlaid.perspective_vertical, edited.perspective_vertical);
    EXPECT_EQ(overlaid.texture, edited.texture);
    EXPECT_TRUE(overlaid.velvia_present);
    EXPECT_EQ(overlaid.velvia, edited.velvia);
    EXPECT_TRUE(overlaid.lut3d_present);
    EXPECT_EQ(overlaid.lut3d, edited.lut3d);

    auto recipe = recipe_from_develop({"source", "file:///source.raw", std::nullopt}, edited);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto empty = recipe_style_from_selected_fields("Empty", {}, recipe.value(), {});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().context.at("reason"), "empty_recipe_style_selection");
    auto unknown =
        recipe_style_from_selected_fields("Unknown", {}, recipe.value(), {"futureField"});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("reason"), "unsupported_recipe_style_selected_field");
    auto duplicate = recipe_style_from_selected_fields("Duplicate", {}, recipe.value(),
                                                       {"exposure", "exposure"});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "noncanonical_recipe_style_selection");
}

TEST(RecipeStyleTest, SelectivePresetMergesRequiredMasksAndKeepsTargetOnlyGraph)
{
    DevelopParams source;
    Mask source_mask{"source-mask", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    source_mask.payload = CircleMask{0.4, 0.5, 0.2, 0.1};
    source.masks.push_back(source_mask);
    source.color_harmonizer_present = true;
    source.color_harmonizer_enabled = true;
    source.color_harmonizer.pull_strength = 0.25;
    source.color_harmonizer_mask_id = source_mask.id;
    auto source_recipe =
        recipe_from_develop({"source", "file:///source.raw", std::nullopt}, source);
    ASSERT_TRUE(source_recipe) << source_recipe.error().message;
    auto style = recipe_style_from_selected_fields("Masked harmony", {}, source_recipe.value(),
                                                   {"colorHarmonizer"});
    ASSERT_TRUE(style) << style.error().message;

    DevelopParams target;
    Mask target_mask{"target-mask", kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    target_mask.payload = LinearGradientMask{0.5, 0.4, 10.0, 0.2};
    target.masks.push_back(target_mask);
    target.graduated_present = true;
    target.graduated_enabled = true;
    target.graduated_density = 0.5;
    target.graduated_mask_id = target_mask.id;
    auto target_recipe =
        recipe_from_develop({"target", "file:///target.raw", std::nullopt}, target);
    ASSERT_TRUE(target_recipe) << target_recipe.error().message;

    auto applied = apply_recipe_style(style.value(), target_recipe.value());
    ASSERT_TRUE(applied) << applied.error().message;
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().color_harmonizer_mask_id, source.color_harmonizer_mask_id);
    EXPECT_EQ(restored.value().graduated_mask_id, target.graduated_mask_id);
    EXPECT_EQ(restored.value().masks.size(), 2U);
    EXPECT_NE(
        std::find(restored.value().masks.cbegin(), restored.value().masks.cend(), source_mask),
        restored.value().masks.cend());
    EXPECT_NE(
        std::find(restored.value().masks.cbegin(), restored.value().masks.cend(), target_mask),
        restored.value().masks.cend());
}

TEST(RecipeStyleTest, SelectiveStyleCarriesMultiInstanceExposure)
{
    DevelopParams source;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.name = "Global";
    global.exposure_ev = 0.15;
    DevelopExposureInstance dodge;
    dodge.instance_id = "exposure-dodge";
    dodge.name = "Dodge";
    dodge.exposure_ev = 0.4;
    dodge.mask_id = "style-exposure-mask";
    source.exposure_instances = {global, dodge};
    Mask mask{"style-exposure-mask", kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    mask.payload = EllipseMask{0.55, 0.4, 0.18, 0.12, 5.0, 0.04};
    source.masks.push_back(mask);
    source.saturation = 0.25;
    auto source_recipe =
        recipe_from_develop({"source", "file:///source.raw", std::nullopt}, source);
    ASSERT_TRUE(source_recipe) << source_recipe.error().message;
    auto style = recipe_style_from_selected_fields("Multi exposure", {}, source_recipe.value(),
                                                   {"exposure"});
    ASSERT_TRUE(style) << style.error().message;

    DevelopParams target;
    target.saturation = -0.1;
    target.contrast = 0.2;
    auto target_recipe =
        recipe_from_develop({"target", "file:///target.raw", std::nullopt}, target);
    ASSERT_TRUE(target_recipe) << target_recipe.error().message;
    auto applied = apply_recipe_style(style.value(), target_recipe.value());
    ASSERT_TRUE(applied) << applied.error().message;
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().exposure_instances.size(), 2U);
    EXPECT_EQ(restored.value().exposure_instances[1].mask_id, "style-exposure-mask");
    EXPECT_NEAR(restored.value().saturation, -0.1, 1e-9);
    EXPECT_NEAR(restored.value().contrast, 0.2, 1e-9);
    EXPECT_NE(std::find(restored.value().masks.begin(), restored.value().masks.end(), mask),
              restored.value().masks.end());
}

TEST(RecipeStyleTest, CompleteStyleCarriesMultiInstanceColorBalanceRgb)
{
    DevelopParams source;
    DevelopColorBalanceRgbInstance master;
    master.instance_id = "cbr-master";
    master.params.contrast = 0.08;
    DevelopColorBalanceRgbInstance grade;
    grade.instance_id = "cbr-grade";
    grade.name = "Cool";
    grade.params.vibrance = -0.15;
    grade.mask_id = "style-cbr-mask";
    source.color_balance_rgb_instances = {master, grade};
    Mask mask{"style-cbr-mask", kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    mask.payload = LinearGradientMask{0.3, 0.5, 45.0, 0.15};
    source.masks.push_back(mask);
    auto source_recipe =
        recipe_from_develop({"source", "file:///source.raw", std::nullopt}, source);
    ASSERT_TRUE(source_recipe) << source_recipe.error().message;
    auto style = recipe_style_from_recipe("Multi CBR", {}, source_recipe.value());
    ASSERT_TRUE(style) << style.error().message;

    AssetDescriptor target{"asset-b", "file:///target.jpg", "hash-b"};
    auto applied = apply_recipe_style(style.value(), target);
    ASSERT_TRUE(applied) << applied.error().message;
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().color_balance_rgb_instances.size(), 2U);
    EXPECT_EQ(restored.value().color_balance_rgb_instances[1].mask_id, "style-cbr-mask");
    EXPECT_EQ(restored.value().masks, source.masks);
}

TEST(RecipeStyleTest, RejectsLegacyUnknownNewerAndNonPlaceholderState)
{
    auto legacy = parse_recipe_style_json("<darktable_style version=\"1.0\"></darktable_style>");
    ASSERT_FALSE(legacy);
    EXPECT_EQ(legacy.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(legacy.error().context.at("reason"), "unsupported_legacy_dtstyle");

    Recipe recipe;
    recipe.asset = {"asset", "file:///source.raw", std::nullopt};
    auto style = recipe_style_from_recipe("Style", "", recipe);
    ASSERT_TRUE(style);
    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized);
    auto unknown = serialized.value();
    unknown.insert(1U, "\"future\":true,");
    auto rejected = parse_recipe_style_json(unknown);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unknown_recipe_style_field");
    auto newer = serialized.value();
    const auto schema = newer.find("\"schema_version\":1");
    ASSERT_NE(schema, std::string::npos);
    newer.replace(schema, std::string("\"schema_version\":1").size(), "\"schema_version\":3");
    rejected = parse_recipe_style_json(newer);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    auto wrong_asset = serialized.value();
    const auto asset = wrong_asset.find(std::string(kRecipeStyleAssetId));
    ASSERT_NE(asset, std::string::npos);
    wrong_asset.replace(asset, kRecipeStyleAssetId.size(), "wrong-template");
    rejected = parse_recipe_style_json(wrong_asset);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_recipe_style_asset");
}

TEST(DevelopSetFieldsTest, ListsAcceptedCliFieldsWithBoundsAndRejectsUnknownNames)
{
    const auto fields = list_develop_set_fields();
    ASSERT_FALSE(fields.empty());
    std::set<std::string> names;
    bool has_exposure = false;
    bool has_canvas = false;
    bool has_watermark_text = false;
    bool has_color_correction = false;
    bool has_color_contrast = false;
    bool has_color_reconstruction = false;
    for (const auto &field : fields)
    {
        EXPECT_TRUE(names.insert(field.name).second) << field.name;
        if (field.name == "exposure")
        {
            has_exposure = true;
            EXPECT_EQ(field.kind, DevelopSetFieldKind::Number);
            ASSERT_TRUE(field.minimum);
            ASSERT_TRUE(field.maximum);
            EXPECT_LT(*field.minimum, 0.0);
            EXPECT_GT(*field.maximum, 0.0);
            DevelopParams at_min;
            EXPECT_TRUE(apply_develop_field_strict(at_min, field.name, *field.minimum));
            DevelopParams at_max;
            EXPECT_TRUE(apply_develop_field_strict(at_max, field.name, *field.maximum));
            EXPECT_FALSE(apply_develop_field_strict(at_max, field.name, *field.maximum + 1.0));
        }
        if (field.name == "canvasEnabled")
        {
            has_canvas = true;
            EXPECT_EQ(field.kind, DevelopSetFieldKind::Toggle);
            EXPECT_EQ(field.minimum, 0.0);
            EXPECT_EQ(field.maximum, 1.0);
        }
        if (field.name == "watermarkText")
        {
            has_watermark_text = true;
            EXPECT_EQ(field.kind, DevelopSetFieldKind::Text);
        }
        if (field.name == "colorCorrectionHighlightA")
        {
            has_color_correction = true;
        }
        if (field.name == "colorContrastASteepness")
        {
            has_color_contrast = true;
        }
        if (field.name == "colorReconstructionThreshold")
        {
            has_color_reconstruction = true;
        }
        if (field.kind != DevelopSetFieldKind::Text)
        {
            ASSERT_TRUE(field.minimum);
            ASSERT_TRUE(field.maximum);
            EXPECT_LE(*field.minimum, *field.maximum) << field.name;
            DevelopParams at_min;
            EXPECT_TRUE(apply_develop_field_strict(at_min, field.name, *field.minimum))
                << field.name;
            DevelopParams at_max;
            EXPECT_TRUE(apply_develop_field_strict(at_max, field.name, *field.maximum))
                << field.name;
        }
    }
    EXPECT_TRUE(has_exposure);
    EXPECT_TRUE(has_canvas);
    EXPECT_TRUE(has_watermark_text);
    EXPECT_TRUE(has_color_correction);
    EXPECT_TRUE(has_color_contrast);
    EXPECT_TRUE(has_color_reconstruction);
    DevelopParams unknown;
    EXPECT_FALSE(apply_develop_field_strict(unknown, "notADevelopField", 1.0));
    const auto prefixes = develop_set_field_prefixes();
    EXPECT_GE(prefixes.size(), 2U);
}

} // namespace
} // namespace ravo
