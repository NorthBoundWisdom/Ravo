#include <gtest/gtest.h>

#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/recipe/operation.h"

namespace ravo
{
TEST(LocalAdjustmentScopeTest, GlobalAndTwoLocalParameterSetsAreIndependent)
{
    DevelopParams global;
    global.exposure_ev = 1.25;
    ASSERT_TRUE(create_local_adjustment(global, "mask-one", 4));
    ASSERT_TRUE(create_local_adjustment(global, "mask-two", 2));
    auto first = local_adjustment_develop(global, "mask-one");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().exposure_ev, 0);
    first.value().exposure_ev = -0.75;
    first.value().saturation = 0.4;
    ASSERT_TRUE(set_local_adjustment_develop(global, "mask-one", first.value()));
    EXPECT_EQ(global.exposure_ev, 1.25);
    EXPECT_EQ(global.saturation, 0);
    auto second = local_adjustment_develop(global, "mask-two");
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().exposure_ev, 0);
    first = local_adjustment_develop(global, "mask-one");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().exposure_ev, -0.75);
    EXPECT_EQ(first.value().saturation, 0.4);
}

TEST(LocalAdjustmentScopeTest, RecipeRoundTripPreservesCompleteGroupAndMask)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "mask-one", 8));
    auto local = local_adjustment_develop(params, "mask-one");
    ASSERT_TRUE(local);
    local.value().exposure_ev = 1;
    local.value().highlights = -0.4;
    local.value().texture.strength = 0.5;
    ASSERT_TRUE(set_local_adjustment_develop(params, "mask-one", local.value()));
    auto recipe = recipe_from_develop({"asset", "photo.png", std::nullopt}, params);
    ASSERT_TRUE(recipe);
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry);
    ASSERT_TRUE(validate_recipe(recipe.value(), registry.value()));
    auto text = serialize_recipe(recipe.value());
    ASSERT_TRUE(text);
    auto decoded = parse_recipe_json(text.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().schema_version, 4);
    EXPECT_EQ(decoded.value().operations, recipe.value().operations);
    auto restored = develop_from_recipe(decoded.value());
    ASSERT_TRUE(restored);
    auto rebuilt = recipe_from_develop(decoded.value().asset, restored.value());
    ASSERT_TRUE(rebuilt);
    EXPECT_EQ(rebuilt.value().operations, decoded.value().operations);
}

TEST(LocalAdjustmentScopeTest, InvalidLocalEditLeavesExactPreviousState)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "mask-one", 4));
    auto local = local_adjustment_develop(params, "mask-one");
    ASSERT_TRUE(local);
    const auto before = params;
    local.value().rotate_quarters = 1;
    EXPECT_FALSE(set_local_adjustment_develop(params, "mask-one", local.value()));
    EXPECT_EQ(params, before);
    EXPECT_FALSE(delete_local_adjustment(params, "missing"));
    EXPECT_EQ(params, before);
}

TEST(LocalAdjustmentScopeTest, DuplicateOwnsIndependentGraphAndLastMaskCanBeDeleted)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "mask-one", 4));
    ASSERT_TRUE(duplicate_local_adjustment(params, "mask-one", "mask-two"));
    EXPECT_NE(params.local_adjustments[0].operation.mask_id,
              params.local_adjustments[1].operation.mask_id);
    auto second = local_adjustment_develop(params, "mask-two");
    ASSERT_TRUE(second);
    ASSERT_TRUE(apply_develop_mask_field_strict(second.value(), "localMaskCenterX", 0.2));
    ASSERT_TRUE(set_local_adjustment_develop(params, "mask-two", second.value()));
    auto first = local_adjustment_develop(params, "mask-one");
    ASSERT_TRUE(first);
    EXPECT_DOUBLE_EQ(develop_mask_editor_state(first.value(), DevelopMaskTarget::kLocal).center_x,
                     0.5);
    ASSERT_TRUE(delete_local_adjustment(params, "mask-one"));
    ASSERT_TRUE(delete_local_adjustment(params, "mask-two"));
    EXPECT_TRUE(params.local_adjustments.empty());
    EXPECT_TRUE(params.masks.empty());
}

TEST(LocalAdjustmentScopeTest, RejectsNestedGroupAndRawChildren)
{
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry);
    OperationInstance group;
    group.id = kLocalAdjustmentOperationId;
    group.mask_id = "root";
    group.children.push_back(group);
    EXPECT_FALSE(validate_local_adjustment(group, registry.value()));
    group.children.front().id = "ravo.raw.demosaic";
    EXPECT_FALSE(validate_local_adjustment(group, registry.value()));
}

TEST(LocalAdjustmentScopeTest, CompositeCloneAndSelectiveCopyPreserveUnselectedMasks)
{
    DevelopParams source, destination;
    ASSERT_TRUE(create_local_adjustment(source, "source", 4));
    ASSERT_TRUE(create_local_adjustment(destination, "destination", 4));
    auto local = local_adjustment_develop(source, "source");
    ASSERT_TRUE(local);
    ASSERT_TRUE(add_local_mask_component(local.value(), 2, 1));
    ASSERT_TRUE(set_local_adjustment_develop(source, "source", local.value()));
    ASSERT_TRUE(duplicate_local_adjustment(source, "source", "source-copy"));
    ASSERT_TRUE(validate_mask_graph(source.masks));
    const auto preserved_masks = destination.masks;
    const auto preserved_locals = destination.local_adjustments;
    source.exposure_ev = 0.7;
    ASSERT_TRUE(apply_develop_selected_fields(destination, source, {"exposure"}));
    EXPECT_EQ(destination.masks, preserved_masks);
    EXPECT_EQ(destination.local_adjustments, preserved_locals);
    ASSERT_TRUE(apply_develop_selected_fields(destination, source, {"masks"}));
    EXPECT_DOUBLE_EQ(destination.exposure_ev, 0.7);
    EXPECT_EQ(destination.local_adjustments.size(), 2U);
    ASSERT_TRUE(validate_mask_graph(destination.masks));
    auto first = local_adjustment_develop(destination, "source");
    ASSERT_TRUE(first);
    EXPECT_TRUE(develop_mask_editor_state(first.value(), DevelopMaskTarget::kLocal).editable);
}

TEST(LocalAdjustmentScopeTest, SharedLocalAttachmentIsExplicitlyReadOnly)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "one", 4));
    ASSERT_TRUE(create_local_adjustment(params, "two", 4));
    params.local_adjustments[1].operation.mask_id = params.local_adjustments[0].operation.mask_id;
    auto local = local_adjustment_develop(params, "one");
    ASSERT_TRUE(local);
    EXPECT_FALSE(develop_mask_editor_state(local.value(), DevelopMaskTarget::kLocal).editable);
    EXPECT_FALSE(apply_develop_mask_field_strict(local.value(), "localMaskCenterX", 0.2));
}

TEST(LocalAdjustmentScopeTest, EditingOneFieldPreservesOtherChildIdentityAndBypass)
{
    DevelopParams params;
    ASSERT_TRUE(create_local_adjustment(params, "scope", 1));
    auto local = local_adjustment_develop(params, "scope");
    ASSERT_TRUE(local);
    local.value().shadows = 0.4;
    ASSERT_TRUE(set_local_adjustment_develop(params, "scope", local.value()));
    auto &child = params.local_adjustments[0].operation.children.back();
    ASSERT_EQ(child.id, "ravo.core.shadows");
    child.instance_id = "owned-shadow";
    child.name = "Named child";
    child.bypass = true;
    local = local_adjustment_develop(params, "scope");
    ASSERT_TRUE(local);
    local.value().exposure_ev = 0.5;
    ASSERT_TRUE(set_local_adjustment_develop(params, "scope", local.value()));
    const auto &updated = params.local_adjustments[0].operation.children.back();
    EXPECT_EQ(updated.instance_id, "owned-shadow");
    EXPECT_EQ(updated.name, "Named child");
    EXPECT_TRUE(updated.bypass);
}
} // namespace ravo
