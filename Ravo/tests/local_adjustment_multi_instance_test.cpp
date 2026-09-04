#include <gtest/gtest.h>

#include <array>
#include <variant>
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

TEST(LocalAdjustmentMultiInstanceTest, AssignExposureUpdatesFrontInstanceWhenPresent)
{
    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.exposure_ev = 0.4;
    develop.exposure_instances = {global};
    clamp_develop(develop);
    ASSERT_TRUE(apply_develop_field(develop, "exposure", -0.1));
    EXPECT_DOUBLE_EQ(develop.exposure_ev, -0.1);
    ASSERT_EQ(develop.exposure_instances.size(), 1U);
    EXPECT_DOUBLE_EQ(develop.exposure_instances.front().exposure_ev, -0.1);
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

TEST(LocalAdjustmentMultiInstanceTest, RoundTripsOrderedColorBalanceRgbInstancesWithMasks)
{
    DevelopParams develop;
    DevelopColorBalanceRgbInstance global;
    global.instance_id = "cbrgb-global";
    global.name = "Global grade";
    global.params.contrast = 0.12;
    DevelopColorBalanceRgbInstance warm;
    warm.instance_id = "cbrgb-warm";
    warm.name = "Warm face";
    warm.params.global_chroma = 0.2;
    warm.params.global_hue = 35.0;
    warm.mask_id = "mask-ellipse";
    DevelopColorBalanceRgbInstance cool;
    cool.instance_id = "cbrgb-cool";
    cool.name = "Cool sky";
    cool.params.shadows_y = -0.05;
    cool.mask_id = "mask-gradient";
    DevelopColorBalanceRgbInstance tones;
    tones.instance_id = "cbrgb-tones";
    tones.name = "Midtones";
    tones.params.vibrance = 0.15;
    tones.mask_id = "mask-parametric";
    develop.color_balance_rgb_instances = {global, warm, cool, tones};
    develop.masks = {make_ellipse("mask-ellipse"), make_gradient("mask-gradient"),
                     make_parametric("mask-parametric")};

    const AssetDescriptor asset{"asset-cbrgb", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::size_t cbrgb_ops = 0;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == "ravo.color.colorbalancergb")
        {
            ++cbrgb_ops;
            EXPECT_TRUE(operation.name.has_value());
        }
    }
    EXPECT_EQ(cbrgb_ops, 4U);
    EXPECT_EQ(recipe.value().masks.size(), 3U);

    auto json = serialize_recipe(recipe.value());
    ASSERT_TRUE(json) << json.error().message;
    auto parsed = parse_recipe_json(json.value());
    ASSERT_TRUE(parsed) << parsed.error().message;

    auto round_trip = develop_from_recipe(parsed.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    ASSERT_EQ(round_trip.value().color_balance_rgb_instances.size(), 4U);
    EXPECT_EQ(round_trip.value().color_balance_rgb_instances[0].instance_id, "cbrgb-global");
    EXPECT_EQ(round_trip.value().color_balance_rgb_instances[0].name, "Global grade");
    EXPECT_DOUBLE_EQ(round_trip.value().color_balance_rgb_instances[0].params.contrast, 0.12);
    EXPECT_EQ(round_trip.value().color_balance_rgb_instances[1].mask_id, "mask-ellipse");
    EXPECT_EQ(round_trip.value().color_balance_rgb_instances[2].mask_id, "mask-gradient");
    EXPECT_EQ(round_trip.value().color_balance_rgb_instances[3].mask_id, "mask-parametric");
    EXPECT_DOUBLE_EQ(round_trip.value().color_balance_rgb.contrast, 0.12);
    EXPECT_EQ(round_trip.value().masks.size(), 3U);
}

TEST(LocalAdjustmentMultiInstanceTest, ColorBalanceRgbBypassSkipsEvaluationButSerializes)
{
    DevelopParams develop;
    DevelopColorBalanceRgbInstance active;
    active.instance_id = "cbrgb-a";
    active.params.contrast = 0.25;
    DevelopColorBalanceRgbInstance bypassed;
    bypassed.instance_id = "cbrgb-b";
    bypassed.name = "Bypassed grade";
    bypassed.params.vibrance = 0.5;
    bypassed.bypass = true;
    develop.color_balance_rgb_instances = {active, bypassed};

    const AssetDescriptor asset{"asset-cbrgb-bypass", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::vector<OperationInstance> ops;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == "ravo.color.colorbalancergb")
        {
            ops.push_back(operation);
        }
    }
    ASSERT_EQ(ops.size(), 2U);
    EXPECT_TRUE(ops[1].bypass);
    ASSERT_TRUE(ops[1].name.has_value());
    EXPECT_EQ(*ops[1].name, "Bypassed grade");

    auto back = develop_from_recipe(recipe.value());
    ASSERT_TRUE(back) << back.error().message;
    ASSERT_EQ(back.value().color_balance_rgb_instances.size(), 2U);
    EXPECT_TRUE(back.value().color_balance_rgb_instances[1].bypass);
}

TEST(LocalAdjustmentMultiInstanceTest, LegacySingleColorBalanceRgbUnchangedWhenInstancesEmpty)
{
    DevelopParams develop;
    develop.color_balance_rgb.contrast = 0.3;
    develop.color_balance_rgb_mask_id = "mask-gradient";
    develop.masks = {make_gradient("mask-gradient")};
    const AssetDescriptor asset{"asset-cbrgb-legacy", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    std::size_t cbrgb_ops = 0;
    for (const auto &operation : recipe.value().operations)
    {
        if (operation.id == "ravo.color.colorbalancergb")
        {
            ++cbrgb_ops;
            EXPECT_EQ(operation.instance_id, "colorbalancergb-1");
            EXPECT_FALSE(operation.bypass);
            EXPECT_FALSE(operation.name.has_value());
        }
    }
    EXPECT_EQ(cbrgb_ops, 1U);
    auto back = develop_from_recipe(recipe.value());
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back.value().color_balance_rgb_instances.size(), 1U);
    EXPECT_DOUBLE_EQ(back.value().color_balance_rgb.contrast, 0.3);
}

TEST(LocalAdjustmentMultiInstanceTest, StudioInstanceHelpersAddReorderBypassDelete)
{
    DevelopParams develop;
    develop.exposure_ev = 0.2;
    auto added = add_exposure_instance(develop);
    ASSERT_TRUE(added) << added.error().message;
    ASSERT_EQ(develop.exposure_instances.size(), 2U);
    EXPECT_DOUBLE_EQ(develop.exposure_instances.front().exposure_ev, 0.2);
    const auto second_id = develop.exposure_instances[1].instance_id;
    ASSERT_TRUE(rename_exposure_instance(develop, second_id, "Dodge"));
    ASSERT_TRUE(set_exposure_instance_bypass(develop, second_id, true));
    EXPECT_TRUE(develop.exposure_instances[1].bypass);
    ASSERT_TRUE(set_exposure_instance_enabled(develop, second_id, false));
    EXPECT_FALSE(develop.exposure_instances[1].enabled);
    ASSERT_TRUE(reorder_exposure_instance(develop, 1, 0));
    EXPECT_EQ(develop.exposure_instances.front().instance_id, second_id);
    ASSERT_TRUE(delete_exposure_instance(develop, second_id));
    ASSERT_EQ(develop.exposure_instances.size(), 1U);

    DevelopParams color;
    auto color_added = add_color_balance_rgb_instance(color);
    ASSERT_TRUE(color_added) << color_added.error().message;
    ASSERT_EQ(color.color_balance_rgb_instances.size(), 2U);
    const auto color_id = color.color_balance_rgb_instances[1].instance_id;
    ASSERT_TRUE(set_color_balance_rgb_instance_bypass(color, color_id, true));
    ASSERT_TRUE(reorder_color_balance_rgb_instance(color, 1, 0));
    EXPECT_EQ(color.color_balance_rgb_instances.front().instance_id, color_id);
    ASSERT_TRUE(delete_color_balance_rgb_instance(color, color_id));
    ASSERT_EQ(color.color_balance_rgb_instances.size(), 1U);
}

TEST(LocalAdjustmentMultiInstanceTest, SelectedInstanceEditBufferSurvivesClamp)
{
    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.exposure_ev = 0.1;
    DevelopExposureInstance local;
    local.instance_id = "exposure-local";
    local.exposure_ev = 0.5;
    develop.exposure_instances = {global, local};
    load_exposure_instance_into_legacy(develop, 1);
    EXPECT_DOUBLE_EQ(develop.exposure_ev, 0.5);
    develop.exposure_ev = -0.25;
    mirror_legacy_exposure_into_instance(develop, 1);
    clamp_develop(develop);
    load_exposure_instance_into_legacy(develop, 1);
    EXPECT_DOUBLE_EQ(develop.exposure_ev, -0.25);
    EXPECT_DOUBLE_EQ(develop.exposure_instances[0].exposure_ev, 0.1);
    EXPECT_DOUBLE_EQ(develop.exposure_instances[1].exposure_ev, -0.25);
}

TEST(LocalAdjustmentMultiInstanceTest, DuplicatesExposureInstanceWithIndependentMask)
{
    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-1";
    global.name = "Master";
    global.exposure_ev = 0.1;
    DevelopExposureInstance local;
    local.instance_id = "exposure-2";
    local.name = "Dodge";
    local.exposure_ev = 0.55;
    local.mask_id = "mask-ellipse";
    develop.exposure_instances = {global, local};
    develop.masks = {make_ellipse("mask-ellipse")};

    auto duplicated = duplicate_exposure_instance(develop, "exposure-2");
    ASSERT_TRUE(duplicated) << duplicated.error().message;
    ASSERT_EQ(develop.exposure_instances.size(), 3U);
    const auto &copy = develop.exposure_instances[2];
    EXPECT_EQ(copy.instance_id, duplicated.value());
    EXPECT_EQ(copy.name, "Dodge copy");
    EXPECT_DOUBLE_EQ(copy.exposure_ev, 0.55);
    ASSERT_TRUE(copy.mask_id.has_value());
    EXPECT_NE(*copy.mask_id, "mask-ellipse");
    ASSERT_EQ(develop.masks.size(), 2U);
    const Mask *cloned = nullptr;
    for (const auto &mask : develop.masks)
    {
        if (mask.id == *copy.mask_id)
        {
            cloned = &mask;
        }
    }
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->kind, MaskKind::kEllipse);
    // Mutating the clone must not change the source mask.
    auto *ellipse = std::get_if<EllipseMask>(&develop.masks.back().payload);
    ASSERT_NE(ellipse, nullptr);
    ellipse->center_x = 0.11;
    const auto *source = std::get_if<EllipseMask>(&develop.masks.front().payload);
    ASSERT_NE(source, nullptr);
    EXPECT_DOUBLE_EQ(source->center_x, 0.6);
}

TEST(LocalAdjustmentMultiInstanceTest, DuplicatesColorBalanceRgbInstanceWithMask)
{
    DevelopParams develop;
    DevelopColorBalanceRgbInstance master;
    master.instance_id = "colorbalancergb-1";
    master.name = "Master";
    master.params.contrast = 0.2;
    DevelopColorBalanceRgbInstance grade;
    grade.instance_id = "colorbalancergb-2";
    grade.name = "Grade";
    grade.params.contrast = 0.45;
    grade.mask_id = "mask-gradient";
    develop.color_balance_rgb_instances = {master, grade};
    develop.masks = {make_gradient("mask-gradient")};

    auto duplicated = duplicate_color_balance_rgb_instance(develop, "colorbalancergb-2");
    ASSERT_TRUE(duplicated) << duplicated.error().message;
    ASSERT_EQ(develop.color_balance_rgb_instances.size(), 3U);
    const auto &copy = develop.color_balance_rgb_instances[2];
    EXPECT_EQ(copy.name, "Grade copy");
    EXPECT_DOUBLE_EQ(copy.params.contrast, 0.45);
    ASSERT_TRUE(copy.mask_id.has_value());
    EXPECT_NE(*copy.mask_id, "mask-gradient");
    EXPECT_EQ(develop.masks.size(), 2U);
}

} // namespace
} // namespace ravo
