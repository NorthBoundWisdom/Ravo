#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <variant>
#include <string>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
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
    EXPECT_TRUE(copy.mask_id->starts_with("ravo.studio.mask.exposure."));
    const std::string clone_id = *copy.mask_id;
    // Mutating the clone must not change the source mask.
    {
        auto *ellipse = std::get_if<EllipseMask>(&develop.masks.back().payload);
        ASSERT_NE(ellipse, nullptr);
        ellipse->center_x = 0.11;
    }
    {
        const auto *source = std::get_if<EllipseMask>(&develop.masks.front().payload);
        ASSERT_NE(source, nullptr);
        EXPECT_DOUBLE_EQ(source->center_x, 0.6);
    }
    // Studio-owned clone remains editable through the mask authoring pipeline.
    load_exposure_instance_into_legacy(develop, 2);
    auto state = develop_mask_editor_state(develop, DevelopMaskTarget::kExposure);
    EXPECT_TRUE(state.editable);
    EXPECT_EQ(state.status, DevelopMaskAttachmentStatus::kEditable);
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskCenterX", 0.42));
    mirror_legacy_exposure_into_instance(develop, 2);
    const Mask *clone_after = nullptr;
    const Mask *source_after = nullptr;
    for (const auto &mask : develop.masks)
    {
        if (mask.id == clone_id)
        {
            clone_after = &mask;
        }
        if (mask.id == "mask-ellipse")
        {
            source_after = &mask;
        }
    }
    ASSERT_NE(clone_after, nullptr);
    ASSERT_NE(source_after, nullptr);
    EXPECT_DOUBLE_EQ(std::get<EllipseMask>(clone_after->payload).center_x, 0.42);
    EXPECT_DOUBLE_EQ(std::get<EllipseMask>(source_after->payload).center_x, 0.6);
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
    EXPECT_TRUE(copy.mask_id->starts_with("ravo.studio.mask.color_balance_rgb."));
    EXPECT_EQ(develop.masks.size(), 2U);
    load_color_balance_rgb_instance_into_legacy(develop, 2);
    auto state = develop_mask_editor_state(develop, DevelopMaskTarget::kColorBalanceRgb);
    EXPECT_TRUE(state.editable);
    EXPECT_EQ(state.status, DevelopMaskAttachmentStatus::kEditable);
}

TEST(LocalAdjustmentMultiInstanceTest, AuthorsLeafMasksOntoSelectedExposureInstance)
{
    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-1";
    global.exposure_ev = 0.0;
    DevelopExposureInstance local;
    local.instance_id = "exposure-2";
    local.exposure_ev = 0.4;
    develop.exposure_instances = {global, local};

    const struct
    {
        double kind = 0.0;
        const char *name = nullptr;
    } leaves[] = {
        {2.0, "linear_gradient"}, {3.0, "circle"}, {4.0, "ellipse"},
        {5.0, "parametric"},      {7.0, "path"},   {8.0, "brush"},
    };

    for (const auto &leaf : leaves)
    {
        load_exposure_instance_into_legacy(develop, 1);
        // Detach any prior leaf so each kind starts from an empty attachment.
        if (develop.exposure_mask_id.has_value())
        {
            ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskKind", 0.0))
                << leaf.name;
        }
        ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskKind", leaf.kind))
            << leaf.name;
        auto state = develop_mask_editor_state(develop, DevelopMaskTarget::kExposure);
        EXPECT_EQ(state.kind_name, leaf.name) << leaf.name;
        EXPECT_TRUE(state.editable) << leaf.name;
        EXPECT_TRUE(state.attached) << leaf.name;
        ASSERT_TRUE(develop.exposure_mask_id.has_value()) << leaf.name;
        EXPECT_TRUE(develop.exposure_mask_id->starts_with("ravo.studio.mask.exposure."))
            << leaf.name;
        mirror_legacy_exposure_into_instance(develop, 1);
        ASSERT_TRUE(develop.exposure_instances[1].mask_id.has_value()) << leaf.name;
        EXPECT_EQ(*develop.exposure_instances[1].mask_id, *develop.exposure_mask_id) << leaf.name;
        // Master instance must remain unmasked.
        EXPECT_FALSE(develop.exposure_instances[0].mask_id.has_value()) << leaf.name;
    }

    // Parametric luminance / colour-range channel remains editable on the local.
    load_exposure_instance_into_legacy(develop, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskKind", 5.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskChannel", 2.0)); // green
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskThreshold1", 0.2));
    mirror_legacy_exposure_into_instance(develop, 1);
    load_exposure_instance_into_legacy(develop, 1);
    auto parametric = develop_mask_editor_state(develop, DevelopMaskTarget::kExposure);
    EXPECT_EQ(parametric.kind_name, "parametric");
    EXPECT_EQ(parametric.channel_index, 2);
    EXPECT_DOUBLE_EQ(parametric.threshold1, 0.2);
}

TEST(LocalAdjustmentMultiInstanceTest, AuthorsLeafMasksOntoSelectedColorBalanceRgbInstance)
{
    DevelopParams develop;
    DevelopColorBalanceRgbInstance master;
    master.instance_id = "colorbalancergb-1";
    DevelopColorBalanceRgbInstance grade;
    grade.instance_id = "colorbalancergb-2";
    develop.color_balance_rgb_instances = {master, grade};

    load_color_balance_rgb_instance_into_legacy(develop, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "colorBalanceRgbMaskKind", 8.0)); // brush
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "colorBalanceRgbMaskPointRadius", 0.08));
    mirror_legacy_color_balance_rgb_into_instance(develop, 1);
    ASSERT_TRUE(develop.color_balance_rgb_instances[1].mask_id.has_value());
    EXPECT_TRUE(develop.color_balance_rgb_instances[1].mask_id->starts_with(
        "ravo.studio.mask.color_balance_rgb."));
    EXPECT_FALSE(develop.color_balance_rgb_instances[0].mask_id.has_value());

    load_color_balance_rgb_instance_into_legacy(develop, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "colorBalanceRgbMaskKind", 7.0)); // path
    auto state = develop_mask_editor_state(develop, DevelopMaskTarget::kColorBalanceRgb);
    EXPECT_EQ(state.kind_name, "path");
    EXPECT_TRUE(state.editable);
    EXPECT_GE(state.point_count, 3);
    mirror_legacy_color_balance_rgb_into_instance(develop, 1);
}

TEST(LocalAdjustmentMultiInstanceTest, FailsClosedExternalAndSharedInstanceMasks)
{
    DevelopParams develop;
    DevelopExposureInstance a;
    a.instance_id = "exposure-1";
    a.mask_id = "mask-ellipse";
    DevelopExposureInstance b;
    b.instance_id = "exposure-2";
    b.mask_id = "mask-ellipse"; // shared external
    develop.exposure_instances = {a, b};
    develop.masks = {make_ellipse("mask-ellipse")};

    load_exposure_instance_into_legacy(develop, 0);
    auto external = develop_mask_editor_state(develop, DevelopMaskTarget::kExposure);
    EXPECT_FALSE(external.editable);
    EXPECT_EQ(external.status, DevelopMaskAttachmentStatus::kExternalReadOnly);
    auto rejected = apply_develop_mask_field_strict(develop, "exposureMaskCenterX", 0.1);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "external_read_only");

    // Studio-owned leaf shared across siblings is shared_read_only.
    DevelopParams shared;
    DevelopExposureInstance s0;
    s0.instance_id = "exposure-1";
    s0.mask_id = "ravo.studio.mask.exposure.1";
    DevelopExposureInstance s1;
    s1.instance_id = "exposure-2";
    s1.mask_id = "ravo.studio.mask.exposure.1";
    shared.exposure_instances = {s0, s1};
    Mask studio{"ravo.studio.mask.exposure.1", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    studio.payload = CircleMask{0.5, 0.5, 0.2, 0.0};
    shared.masks = {studio};
    load_exposure_instance_into_legacy(shared, 1);
    auto shared_state = develop_mask_editor_state(shared, DevelopMaskTarget::kExposure);
    EXPECT_FALSE(shared_state.editable);
    EXPECT_EQ(shared_state.status, DevelopMaskAttachmentStatus::kSharedReadOnly);
    auto shared_reject = apply_develop_mask_field_strict(shared, "exposureMaskRadius", 0.3);
    ASSERT_FALSE(shared_reject);
    EXPECT_EQ(shared_reject.error().context.at("reason"), "shared_read_only");

    // Detach is allowed and clears only the selected instance attachment.
    ASSERT_TRUE(apply_develop_mask_field_strict(shared, "exposureMaskKind", 0.0));
    mirror_legacy_exposure_into_instance(shared, 1);
    EXPECT_FALSE(shared.exposure_instances[1].mask_id.has_value());
    EXPECT_EQ(shared.exposure_instances[0].mask_id, "ravo.studio.mask.exposure.1");
    EXPECT_EQ(shared.masks.size(), 1U);
}

TEST(LocalAdjustmentMultiInstanceTest, HistorySummaryCoversInstanceVectorAndMasks)
{
    DevelopParams before;
    DevelopExposureInstance master;
    master.instance_id = "exposure-1";
    master.name = "Master";
    before.exposure_instances = {master};

    DevelopParams after = before;
    DevelopExposureInstance local;
    local.instance_id = "exposure-2";
    local.name = "Dodge";
    local.exposure_ev = 0.35;
    local.mask_id = "ravo.studio.mask.exposure.2";
    after.exposure_instances.push_back(local);
    Mask mask{"ravo.studio.mask.exposure.2", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    mask.payload = CircleMask{0.4, 0.5, 0.15, 0.0};
    after.masks = {mask};

    const auto changes = develop_change_summary(before, after);
    bool saw_instances = false;
    bool saw_masks = false;
    for (const auto &change : changes)
    {
        if (change.field == "exposureInstances")
            saw_instances = true;
        if (change.field == "masks")
            saw_masks = true;
    }
    EXPECT_TRUE(saw_instances);
    EXPECT_TRUE(saw_masks);

    DevelopParams cbr_before;
    DevelopColorBalanceRgbInstance c0;
    c0.instance_id = "colorbalancergb-1";
    cbr_before.color_balance_rgb_instances = {c0};
    DevelopParams cbr_after = cbr_before;
    ASSERT_TRUE(add_color_balance_rgb_instance(cbr_after));
    const auto cbr_changes = develop_change_summary(cbr_before, cbr_after);
    bool saw_cbr = false;
    for (const auto &change : cbr_changes)
    {
        if (change.field == "colorBalanceRgbInstances")
            saw_cbr = true;
    }
    EXPECT_TRUE(saw_cbr);
}

TEST(LocalAdjustmentMultiInstanceTest, StructuralOpsRoundTripThroughRecipeHistoryJson)
{
    DevelopParams develop;
    // First add seeds Master then appends Instance 2.
    ASSERT_TRUE(add_exposure_instance(develop));
    ASSERT_EQ(develop.exposure_instances.size(), 2U);
    develop.exposure_instances[1].name = "Local";
    develop.exposure_instances[1].exposure_ev = 0.5;
    load_exposure_instance_into_legacy(develop, 1);
    ASSERT_TRUE(apply_develop_mask_field_strict(develop, "exposureMaskKind", 3.0)); // circle
    mirror_legacy_exposure_into_instance(develop, 1);
    ASSERT_TRUE(develop.exposure_instances[1].mask_id.has_value());
    const auto masked_id = *develop.exposure_instances[1].mask_id;

    ASSERT_TRUE(
        set_exposure_instance_bypass(develop, develop.exposure_instances[0].instance_id, true));
    ASSERT_TRUE(reorder_exposure_instance(develop, 0, 1));
    ASSERT_EQ(develop.exposure_instances.size(), 2U);

    const AssetDescriptor asset{"asset-history", "file:///fixture.raw", std::nullopt};
    auto recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto json = serialize_recipe(recipe.value());
    ASSERT_TRUE(json) << json.error().message;
    auto parsed = parse_recipe_json(json.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().exposure_instances.size(), 2U);
    EXPECT_TRUE(restored.value().exposure_instances[0].bypass ||
                restored.value().exposure_instances[1].bypass);
    bool found_mask = false;
    for (const auto &instance : restored.value().exposure_instances)
    {
        if (instance.mask_id && *instance.mask_id == masked_id)
            found_mask = true;
    }
    EXPECT_TRUE(found_mask);
    EXPECT_FALSE(restored.value().masks.empty());

    // Duplicate a masked instance; clone keeps an independent mask through history.
    DevelopParams dup = restored.value();
    std::string source_id;
    for (const auto &instance : dup.exposure_instances)
    {
        if (instance.mask_id && *instance.mask_id == masked_id)
        {
            source_id = instance.instance_id;
            break;
        }
    }
    ASSERT_FALSE(source_id.empty());
    ASSERT_TRUE(duplicate_exposure_instance(dup, source_id));
    ASSERT_EQ(dup.exposure_instances.size(), 3U);
    ASSERT_TRUE(dup.exposure_instances.back().mask_id.has_value());
    EXPECT_NE(*dup.exposure_instances.back().mask_id, masked_id);
    const auto cloned_mask = *dup.exposure_instances.back().mask_id;
    auto dup_recipe = recipe_from_develop(asset, dup);
    ASSERT_TRUE(dup_recipe) << dup_recipe.error().message;
    auto dup_json = serialize_recipe(dup_recipe.value());
    ASSERT_TRUE(dup_json) << dup_json.error().message;
    auto dup_parsed = parse_recipe_json(dup_json.value());
    ASSERT_TRUE(dup_parsed) << dup_parsed.error().message;
    auto dup_restored = develop_from_recipe(dup_parsed.value());
    ASSERT_TRUE(dup_restored) << dup_restored.error().message;
    ASSERT_EQ(dup_restored.value().exposure_instances.size(), 3U);
    bool found_clone = false;
    for (const auto &instance : dup_restored.value().exposure_instances)
    {
        if (instance.mask_id && *instance.mask_id == cloned_mask)
            found_clone = true;
    }
    EXPECT_TRUE(found_clone);
}

TEST(LocalAdjustmentMultiInstanceTest, SelectiveCopyCarriesMultiInstanceExposureMasks)
{
    DevelopParams source;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.name = "Global";
    global.exposure_ev = 0.2;
    DevelopExposureInstance local;
    local.instance_id = "exposure-local";
    local.name = "Dodge";
    local.exposure_ev = 0.45;
    local.mask_id = "mask-ellipse";
    source.exposure_instances = {global, local};
    source.masks = {make_ellipse("mask-ellipse")};
    source.saturation = 0.1;

    DevelopParams destination;
    destination.saturation = -0.2;
    destination.contrast = 0.15;
    Mask keep{"keep-mask", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    keep.payload = CircleMask{0.5, 0.5, 0.1, 0.05};
    destination.masks.push_back(keep);

    auto applied = apply_develop_selected_fields(destination, source, {"exposure"});
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(destination.exposure_instances.size(), 2U);
    EXPECT_EQ(destination.exposure_instances[1].mask_id, "mask-ellipse");
    EXPECT_NEAR(destination.saturation, -0.2, 1e-9);
    EXPECT_NEAR(destination.contrast, 0.15, 1e-9);
    EXPECT_EQ(destination.masks.size(), 2U);
    EXPECT_NE(
        std::find(destination.masks.begin(), destination.masks.end(), make_ellipse("mask-ellipse")),
        destination.masks.end());
    EXPECT_NE(std::find(destination.masks.begin(), destination.masks.end(), keep),
              destination.masks.end());
}

TEST(LocalAdjustmentMultiInstanceTest, SelectiveCopyCarriesMultiInstanceColorBalanceRgbMasks)
{
    DevelopParams source;
    DevelopColorBalanceRgbInstance master;
    master.instance_id = "cbr-master";
    master.name = "Master";
    master.params.contrast = 0.1;
    DevelopColorBalanceRgbInstance grade;
    grade.instance_id = "cbr-grade";
    grade.name = "Warm face";
    grade.params.vibrance = 0.2;
    grade.mask_id = "mask-gradient";
    source.color_balance_rgb_instances = {master, grade};
    source.masks = {make_gradient("mask-gradient")};
    source.exposure_ev = 0.3;

    DevelopParams destination;
    destination.exposure_ev = -0.1;
    auto applied = apply_develop_selected_fields(destination, source, {"colorBalanceRgb"});
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(destination.color_balance_rgb_instances.size(), 2U);
    EXPECT_EQ(destination.color_balance_rgb_instances[1].mask_id, "mask-gradient");
    EXPECT_NEAR(destination.exposure_ev, -0.1, 1e-9);
    ASSERT_EQ(destination.masks.size(), 1U);
    EXPECT_EQ(destination.masks.front().id, "mask-gradient");
}

TEST(LocalAdjustmentMultiInstanceTest, SelectiveCopyFailClosedOnOrphanInstanceMask)
{
    DevelopParams source;
    DevelopExposureInstance local;
    local.instance_id = "exposure-orphan";
    local.exposure_ev = 0.5;
    local.mask_id = "missing-mask";
    source.exposure_instances = {local};
    // Intentionally no masks entry for missing-mask.

    DevelopParams destination;
    auto applied = apply_develop_selected_fields(destination, source, {"exposure"});
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().context.at("reason"), "missing_develop_selection_mask");
    EXPECT_EQ(applied.error().context.at("mask_id"), "missing-mask");
    EXPECT_TRUE(destination.exposure_instances.empty());
}

TEST(LocalAdjustmentMultiInstanceTest, Cor01SoleInstanceDeleteRequiresMatchingId)
{
    DevelopParams params;
    ASSERT_EQ(ensure_exposure_instances(params), 1U);
    params.exposure_instances.front().exposure_ev = 1.25;
    const auto sole_id = params.exposure_instances.front().instance_id;

    auto wrong = delete_exposure_instance(params, "exposure-stale");
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().context.at("reason"), "delete_exposure_instance_id_mismatch");
    ASSERT_EQ(params.exposure_instances.size(), 1U);
    EXPECT_DOUBLE_EQ(params.exposure_instances.front().exposure_ev, 1.25);

    ASSERT_TRUE(delete_exposure_instance(params, sole_id));
    EXPECT_TRUE(params.exposure_instances.empty());
    EXPECT_DOUBLE_EQ(params.exposure_ev, 1.25);
}

TEST(LocalAdjustmentMultiInstanceTest, Cor01SoleDisabledCollapseResetsIdentity)
{
    DevelopParams params;
    ASSERT_EQ(ensure_exposure_instances(params), 1U);
    params.exposure_instances.front().exposure_ev = 2.0;
    params.exposure_instances.front().enabled = false;
    const auto sole_id = params.exposure_instances.front().instance_id;
    ASSERT_TRUE(delete_exposure_instance(params, sole_id));
    EXPECT_TRUE(params.exposure_instances.empty());
    EXPECT_DOUBLE_EQ(params.exposure_ev, 0.0);

    DevelopParams color;
    ASSERT_EQ(ensure_color_balance_rgb_instances(color), 1U);
    color.color_balance_rgb_instances.front().params.global_y = 0.4;
    color.color_balance_rgb_instances.front().bypass = true;
    const auto color_id = color.color_balance_rgb_instances.front().instance_id;
    ASSERT_TRUE(delete_color_balance_rgb_instance(color, color_id));
    EXPECT_TRUE(color.color_balance_rgb_instances.empty());
    EXPECT_DOUBLE_EQ(color.color_balance_rgb.global_y, 0.0);
}

TEST(LocalAdjustmentMultiInstanceTest, Cor01MaskCloneRollsBackOnCycleOrMissingChild)
{
    DevelopParams params;
    Mask leaf = make_gradient("ravo.studio.mask.exposure.1");
    Mask group{"ravo.studio.mask.exposure.2", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    MaskGroup payload;
    MaskGroupChild child;
    child.mask_id = "missing-child";
    payload.children.push_back(child);
    group.payload = payload;
    params.masks.push_back(leaf);
    params.masks.push_back(group);
    const auto before = params.masks.size();

    DevelopExposureInstance instance;
    instance.instance_id = "exposure-1";
    instance.mask_id = group.id;
    params.exposure_instances.push_back(instance);

    auto missing = duplicate_exposure_instance(params, "exposure-1");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "duplicate_instance_mask_missing");
    EXPECT_EQ(params.masks.size(), before);
    EXPECT_EQ(params.exposure_instances.size(), 1U);

    // Cycle: A -> B -> A
    params.masks.clear();
    Mask a{"ravo.studio.mask.exposure.10", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    MaskGroup ga;
    MaskGroupChild ca;
    ca.mask_id = "ravo.studio.mask.exposure.11";
    ga.children.push_back(ca);
    a.payload = ga;
    Mask b{"ravo.studio.mask.exposure.11", kCanonicalMaskSchemaVersion, MaskKind::kGroup};
    MaskGroup gb;
    MaskGroupChild cb;
    cb.mask_id = "ravo.studio.mask.exposure.10";
    gb.children.push_back(cb);
    b.payload = gb;
    params.masks.push_back(a);
    params.masks.push_back(b);
    params.exposure_instances.front().mask_id = a.id;
    const auto cycle_before = params.masks.size();
    auto cycled = duplicate_exposure_instance(params, "exposure-1");
    ASSERT_FALSE(cycled);
    EXPECT_EQ(cycled.error().context.at("reason"), "duplicate_instance_mask_cycle");
    EXPECT_EQ(params.masks.size(), cycle_before);
}

} // namespace

} // namespace ravo
