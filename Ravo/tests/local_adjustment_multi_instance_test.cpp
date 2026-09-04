#include <gtest/gtest.h>

#include <array>
#include <string>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/mask.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

[[nodiscard]] Mask make_gradient(std::string id)
{
    Mask mask{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kLinearGradient};
    mask.payload = LinearGradientMask{0.25, 0.5, 90.0, 0.2};
    return mask;
}

[[nodiscard]] Mask make_ellipse(std::string id)
{
    Mask mask{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    mask.payload = EllipseMask{0.6, 0.4, 0.2, 0.15, 15.0, 0.05};
    return mask;
}

[[nodiscard]] Mask make_parametric(std::string id)
{
    Mask mask{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kParametric};
    mask.payload = ParametricMask{ParametricMaskSource::kInput, ParametricMaskChannel::kLuminance,
                                  std::array<double, 4>{0.1, 0.25, 0.75, 0.9}};
    return mask;
}

TEST(LocalAdjustmentMultiInstanceTest, RoundTripsOrderedExposureInstancesWithMasks)
{
    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.name = "Global";
    global.exposure_ev = 0.25;
    DevelopExposureInstance dodge;
    dodge.instance_id = "exposure-dodge";
    dodge.name = "Dodge face";
    dodge.exposure_ev = 0.4;
    dodge.mask_id = "mask-ellipse";
    DevelopExposureInstance burn;
    burn.instance_id = "exposure-burn";
    burn.name = "Burn sky";
    burn.exposure_ev = -0.35;
    burn.mask_id = "mask-gradient";
    DevelopExposureInstance parametric;
    parametric.instance_id = "exposure-tones";
    parametric.name = "Midtones";
    parametric.exposure_ev = 0.15;
    parametric.mask_id = "mask-parametric";
    develop.exposure_instances = {global, dodge, burn, parametric};
    develop.masks = {make_ellipse("mask-ellipse"), make_gradient("mask-gradient"),
                     make_parametric("mask-parametric")};

    const AssetDescriptor asset{"asset-local", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::size_t exposure_ops = 0;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == kExposureOperationId)
        {
            ++exposure_ops;
            EXPECT_TRUE(operation.name.has_value());
        }
    }
    EXPECT_EQ(exposure_ops, 4U);
    EXPECT_EQ(recipe.value().masks.size(), 3U);

    auto json = serialize_recipe(recipe.value());
    ASSERT_TRUE(json) << json.error().message;
    auto parsed = parse_recipe_json(json.value());
    ASSERT_TRUE(parsed) << parsed.error().message;

    auto round_trip = develop_from_recipe(parsed.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    ASSERT_EQ(round_trip.value().exposure_instances.size(), 4U);
    EXPECT_EQ(round_trip.value().exposure_instances[0].instance_id, "exposure-global");
    EXPECT_EQ(round_trip.value().exposure_instances[0].name, "Global");
    EXPECT_DOUBLE_EQ(round_trip.value().exposure_instances[0].exposure_ev, 0.25);
    EXPECT_EQ(round_trip.value().exposure_instances[1].mask_id, "mask-ellipse");
    EXPECT_EQ(round_trip.value().exposure_instances[2].mask_id, "mask-gradient");
    EXPECT_EQ(round_trip.value().exposure_instances[3].mask_id, "mask-parametric");
    EXPECT_DOUBLE_EQ(round_trip.value().exposure_ev, 0.25);
    EXPECT_EQ(round_trip.value().masks.size(), 3U);
}

TEST(LocalAdjustmentMultiInstanceTest, BypassSkipsEvaluationButSerializes)
{
    DevelopParams develop;
    DevelopExposureInstance active;
    active.instance_id = "exposure-a";
    active.exposure_ev = 1.0;
    DevelopExposureInstance bypassed;
    bypassed.instance_id = "exposure-b";
    bypassed.name = "Bypassed";
    bypassed.exposure_ev = -1.0;
    bypassed.bypass = true;
    develop.exposure_instances = {active, bypassed};

    const AssetDescriptor asset{"asset-bypass", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::vector<OperationInstance> exposures;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == kExposureOperationId)
        {
            exposures.push_back(operation);
        }
    }
    ASSERT_EQ(exposures.size(), 2U);
    EXPECT_TRUE(exposures[1].bypass);
    ASSERT_TRUE(exposures[1].name.has_value());
    EXPECT_EQ(*exposures[1].name, "Bypassed");

    auto back = develop_from_recipe(recipe.value());
    ASSERT_TRUE(back) << back.error().message;
    ASSERT_EQ(back.value().exposure_instances.size(), 2U);
    EXPECT_TRUE(back.value().exposure_instances[1].bypass);
}

TEST(LocalAdjustmentMultiInstanceTest, LegacySingleExposureUnchangedWhenInstancesEmpty)
{
    DevelopParams develop;
    develop.exposure_ev = 0.5;
    develop.exposure_mask_id = "mask-gradient";
    develop.masks = {make_gradient("mask-gradient")};
    const AssetDescriptor asset{"asset-legacy", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::size_t exposure_ops = 0;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == kExposureOperationId)
        {
            ++exposure_ops;
            EXPECT_EQ(operation.instance_id, "exposure-1");
            EXPECT_FALSE(operation.bypass);
            EXPECT_FALSE(operation.name.has_value());
        }
    }
    EXPECT_EQ(exposure_ops, 1U);
    auto back = develop_from_recipe(recipe.value());
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back.value().exposure_instances.size(), 1U);
    EXPECT_DOUBLE_EQ(back.value().exposure_ev, 0.5);
}

} // namespace
} // namespace ravo
