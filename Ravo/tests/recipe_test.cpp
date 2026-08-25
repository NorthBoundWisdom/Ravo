#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

#include "test_support.h"

namespace ravo
{
namespace
{

TEST(RecipeTest, CanonicalRoundTripValidatesAgainstThePhaseOneRegistry)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;

    const auto serialized = serialize_recipe(test::valid_recipe());
    ASSERT_TRUE(serialized) << serialized.error().message;
    EXPECT_EQ(
        serialized.value(),
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[{"enabled":true,"id":"ravo.core.exposure","instance_id":"exposure-1","parameters":{"exposure_ev":1.25},"schema_version":1}],"schema_version":1})");

    const auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto valid = validate_recipe(parsed.value(), registry.value());
    EXPECT_TRUE(valid) << valid.error().message;
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
    recipe.operations.front().id = "ravo.creative.unknown";

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
    recipe.operations.front().parameters["exposure_ev"] = ParameterValue{11.0};

    const auto valid = validate_recipe(recipe, registry.value());
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().code, ErrorCode::kValidation);
    EXPECT_EQ(valid.error().context.at("parameter"), "exposure_ev");
}

TEST(RecipeTest, DevelopParamsRoundTripThroughCanonicalRecipe)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    DevelopParams params;
    params.temperature = 4800;
    params.tint = 12;
    params.exposure_ev = -0.3;
    params.contrast = 0.2;
    params.rotate_quarters = 1;
    params.crop_width = 0.8;
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto valid = validate_recipe(recipe.value(), registry.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_NEAR(restored.value().temperature, 4800.0, 1e-6);
    EXPECT_NEAR(restored.value().tint, 12.0, 1e-6);
    EXPECT_NEAR(restored.value().exposure_ev, -0.3, 1e-6);
    EXPECT_EQ(restored.value().rotate_quarters, 1);
    EXPECT_NEAR(restored.value().crop_width, 0.8, 1e-6);
    EXPECT_FALSE(restored.value().is_identity());
    EXPECT_TRUE(DevelopParams{}.is_identity());
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
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto valid = validate_recipe(recipe.value(), registry.value());
    ASSERT_TRUE(valid) << valid.error().message;
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_NEAR(restored.value().sharpen, 0.4, 1e-6);
    EXPECT_NEAR(restored.value().vignette, 0.5, 1e-6);
    EXPECT_EQ(restored.value().flip_horizontal, 1);
    EXPECT_NEAR(restored.value().gamma, 1.2, 1e-6);

    DevelopParams crop;
    ASSERT_TRUE(apply_crop_aspect(crop, "3:2"));
    EXPECT_NEAR(crop.crop_width / crop.crop_height, 1.5, 1e-6);
    EXPECT_TRUE(apply_develop_field(crop, "exposure", -0.25));
    EXPECT_NEAR(crop.exposure_ev, -0.25, 1e-6);
    EXPECT_TRUE(reset_develop_field(crop, "exposure"));
    EXPECT_NEAR(crop.exposure_ev, 0.0, 1e-6);
    EXPECT_TRUE(reset_develop_section(crop, "geometry"));
    EXPECT_NEAR(crop.crop_width, 1.0, 1e-6);

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
    lab.operations.front().parameters["working_space"] = ParameterValue{"lab"};
    const auto rejected_space = validate_recipe(lab, registry.value());
    ASSERT_FALSE(rejected_space);
    EXPECT_EQ(rejected_space.error().code, ErrorCode::kValidation);

    auto decreasing = recipe.value();
    decreasing.operations.front().parameters["points"] = ParameterValue{ParameterValue::Array{
        ParameterValue{ParameterValue::Object{{"x", ParameterValue{0.0}}, {"y", ParameterValue{0.0}}}},
        ParameterValue{ParameterValue::Object{{"x", ParameterValue{0.2}}, {"y", ParameterValue{0.4}}}},
        ParameterValue{ParameterValue::Object{{"x", ParameterValue{0.1}}, {"y", ParameterValue{0.8}}}},
        ParameterValue{ParameterValue::Object{{"x", ParameterValue{1.0}}, {"y", ParameterValue{1.0}}}},
    }};
    const auto rejected_points = validate_recipe(decreasing, registry.value());
    ASSERT_FALSE(rejected_points);
    EXPECT_EQ(rejected_points.error().code, ErrorCode::kValidation);
}

TEST(RecipeTest, RejectsNewerSchemaVersionsBeforeValidation)
{
    const auto recipe = parse_recipe_json(
        R"({"asset":{"id":"asset-1","input_uri":"file:///fixture.raw"},"masks":[],"operations":[],"schema_version":2})");

    ASSERT_FALSE(recipe);
    EXPECT_EQ(recipe.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
