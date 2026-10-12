#include "ravo/recipe/develop.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <gtest/gtest.h>

namespace ravo
{
TEST(DevelopFieldDescriptor, NestedFieldsKeepUnitsPresenceAtomicityAndRecipeRoundTrip)
{
    DevelopParams params;
    const std::array edits{std::pair{"profileGammaLinear", 0.2},
                           std::pair{"profileGammaGamma", 0.6},
                           std::pair{"profileGammaDynamicRange", 12.0},
                           std::pair{"profileGammaGreyPoint", 20.0},
                           std::pair{"profileGammaShadowsRange", -4.0},
                           std::pair{"profileGammaSecurityFactor", 5.0},
                           std::pair{"primariesAchromaticHueDegrees", 13.5},
                           std::pair{"primariesAchromaticPurity", 0.2},
                           std::pair{"primariesRedHueDegrees", -17.5},
                           std::pair{"primariesRedPurity", 1.2},
                           std::pair{"primariesGreenHueDegrees", 23.5},
                           std::pair{"primariesGreenPurity", 0.8},
                           std::pair{"primariesBlueHueDegrees", -37.5},
                           std::pair{"primariesBluePurity", 1.1}};
    for (const auto &[name, value] : edits)
    {
        SCOPED_TRACE(name);
        ASSERT_TRUE(apply_develop_field_strict(params, name, value));
        const auto before_invalid = params;
        EXPECT_FALSE(apply_develop_field(params, name, std::numeric_limits<double>::quiet_NaN()));
        EXPECT_EQ(params, before_invalid);
        EXPECT_FALSE(apply_develop_field_strict(params, name, 1000000.0));
        EXPECT_EQ(params, before_invalid);
    }
    EXPECT_FALSE(params.profile_gamma_enabled);
    EXPECT_FALSE(params.primaries.is_identity());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(params.primaries.red_hue),
              std::bit_cast<std::uint64_t>(-17.5 * std::numbers::pi / 180.0));
    const auto gamma_before_reset = params.profile_gamma.gamma;
    ASSERT_TRUE(reset_develop_field(params, "profileGammaLinear"));
    EXPECT_DOUBLE_EQ(params.profile_gamma.linear, kProfileGammaLinearDefault);
    EXPECT_DOUBLE_EQ(params.profile_gamma.gamma, gamma_before_reset);
    ASSERT_TRUE(reset_develop_field(params, "primariesRedHueDegrees"));
    EXPECT_DOUBLE_EQ(params.primaries.red_hue, 0.0);
    EXPECT_DOUBLE_EQ(params.primaries.red_purity, 1.2);
    params.profile_gamma_enabled = true;
    auto recipe =
        recipe_from_develop({"descriptor-asset", "file:///descriptor.raw", std::nullopt}, params);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized);
    auto parsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(parsed);
    auto restored = develop_from_recipe(parsed.value());
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().profile_gamma, params.profile_gamma);
    EXPECT_EQ(restored.value().primaries, params.primaries);
}

TEST(DevelopFieldDescriptor, NonfiniteClampingRetainsExistingDefaults)
{
    DevelopParams params;
    params.profile_gamma.linear = std::numeric_limits<double>::quiet_NaN();
    params.profile_gamma.gamma = std::numeric_limits<double>::infinity();
    params.profile_gamma.shadows_range = -std::numeric_limits<double>::infinity();
    params.primaries.red_hue = std::numeric_limits<double>::quiet_NaN();
    params.primaries.red_purity = std::numeric_limits<double>::quiet_NaN();
    clamp_develop(params);
    EXPECT_DOUBLE_EQ(params.profile_gamma.linear, kProfileGammaLinearDefault);
    EXPECT_DOUBLE_EQ(params.profile_gamma.gamma, kProfileGammaGammaDefault);
    EXPECT_DOUBLE_EQ(params.profile_gamma.shadows_range, kProfileGammaShadowsRangeDefault);
    EXPECT_DOUBLE_EQ(params.primaries.red_hue, 0.0);
    EXPECT_DOUBLE_EQ(params.primaries.red_purity, 1.0);
}

TEST(DevelopFieldDescriptor, ContextualRangesAllowFractionalValuesAndRejectCoupledChangesAtomically)
{
    std::size_t contextual = 0;
    const auto fields = list_develop_set_fields();
    for (const auto &field : fields)
    {
        if (!field.context_dependent)
            continue;
        ++contextual;
        EXPECT_EQ(field.kind, DevelopSetFieldKind::Number);
        ASSERT_TRUE(field.minimum);
        ASSERT_TRUE(field.maximum);
        EXPECT_LT(*field.minimum, *field.maximum);
    }
    EXPECT_EQ(contextual, 7U);
    EXPECT_NE(std::find_if(fields.begin(), fields.end(), [](const auto &field)
                           { return field.name == "rgbCurveSplit2" && field.context_dependent; }),
              fields.end());
    DevelopParams identity;
    const auto before = identity;
    EXPECT_FALSE(apply_develop_field_strict(identity, "cropX", 0.2));
    EXPECT_EQ(identity, before);
    DevelopParams crop;
    ASSERT_TRUE(apply_develop_field_strict(crop, "cropWidth", 0.5));
    ASSERT_TRUE(apply_develop_field_strict(crop, "cropHeight", 0.4));
    ASSERT_TRUE(apply_develop_field_strict(crop, "cropX", 0.2));
    ASSERT_TRUE(apply_develop_field_strict(crop, "cropY", 0.3));
    const auto before_bad_crop = crop;
    EXPECT_FALSE(apply_develop_field_strict(crop, "cropWidth", 0.9));
    EXPECT_EQ(crop, before_bad_crop);
    DevelopParams curve;
    ASSERT_TRUE(apply_develop_field_strict(curve, "rgbCurveSplit0", 0.2));
    ASSERT_TRUE(apply_develop_field_strict(curve, "rgbCurveSplit1", 0.6));
    ASSERT_TRUE(apply_develop_field_strict(curve, "rgbCurveSplit2", 0.85));
    const auto before_bad_split = curve;
    EXPECT_FALSE(apply_develop_field_strict(curve, "rgbCurveSplit1", 0.18));
    EXPECT_EQ(curve, before_bad_split);
    ASSERT_TRUE(apply_develop_field_strict(curve, "rgbCurveShadows", 0.2));
    auto recipe =
        recipe_from_develop({"context-asset", "file:///context.raw", std::nullopt}, curve);
    ASSERT_TRUE(recipe);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value().rgb_curve, curve.rgb_curve);
}
} // namespace ravo
