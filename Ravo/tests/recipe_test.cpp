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
