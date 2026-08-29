#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hsl.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/split_toning.h"
#include "split_toning.h"

namespace ravo
{
namespace
{

[[nodiscard]] WorkingImage image(const std::vector<float> &rgb)
{
    WorkingImage result;
    result.width = static_cast<std::uint32_t>(rgb.size() / 3U);
    result.height = 1U;
    result.rgb = rgb;
    result.color_profile.kind = ColorProfileKind::kBuiltin;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = "linear_rec709";
    result.color_profile.has_matrix = true;
    result.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                              0.2225045F, 0.7168786F, 0.0606169F,
                                              0.0139322F, 0.0971045F, 0.7141733F};
    result.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        result.width, result.height, result.width, result.height);
    return result;
}

[[nodiscard]] std::string split_xmp(const std::string_view parameters)
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="8" darktable:operation="splittoning" darktable:enabled="1"
    darktable:modversion="1" darktable:params=")") +
           std::string(parameters) +
           R"(" darktable:multi_name="" darktable:multi_name_hand_edited="0"
    darktable:multi_priority="0" darktable:blendop_version="9"
    darktable:blendop_params="gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ="/>
  </rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
}

TEST(SplitToningRecipeTest, SchemaLegacyUpgradeAndDevelopRoundTripAreStrict)
{
    SplitToningParams params{0.34, 0.9, 0.93, 0.9, 0.35, 15.0, 0.8};
    auto parameters = split_toning_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(split_toning_from_parameters(parameters.value()).value(), params);

    OperationInstance v1{std::string(kSplitToningOperationId),
                         1,
                         "old",
                         true,
                         {{"shadows_hue", ParameterValue{0.1}},
                          {"highlights_hue", ParameterValue{0.8}},
                          {"balance", ParameterValue{0.4}},
                          {"amount", ParameterValue{0.25}}},
                         std::nullopt};
    ASSERT_TRUE(upgrade_split_toning_operation(v1));
    auto upgraded = split_toning_from_parameters(v1.parameters);
    ASSERT_TRUE(upgraded);
    EXPECT_DOUBLE_EQ(upgraded.value().shadow_saturation, 0.5);
    EXPECT_DOUBLE_EQ(upgraded.value().compress, 33.0);
    EXPECT_DOUBLE_EQ(upgraded.value().mix, 0.25);

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "splitToningEnabled", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "splitShadowSaturation", 0.8));
    ASSERT_TRUE(apply_develop_field_strict(develop, "splitCompress", 12.0));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip);
    EXPECT_TRUE(roundtrip.value().split_toning_enabled);
    EXPECT_DOUBLE_EQ(roundtrip.value().split_toning.shadow_saturation, 0.8);
}

TEST(SplitToningTest, ShadowMidtoneAndHighlightBranchesMatchScalarFormula)
{
    SplitToningParams params;
    params.shadow_hue = 0.6;
    params.shadow_saturation = 0.8;
    params.highlight_hue = 0.1;
    params.highlight_saturation = 0.7;
    params.balance = 0.5;
    params.compress = 22.0;
    const std::vector<float> source{0.2F, 0.2F, 0.2F, 0.5F, 0.5F, 0.5F, 0.8F, 0.8F, 0.8F};
    auto result = apply_split_toning(image(source), params, {});
    ASSERT_TRUE(result) << result.error().message;
    const float compression = static_cast<float>(params.compress / 110.0 / 2.0);
    for (std::size_t pixel = 0U; pixel < 3U; ++pixel)
    {
        const float lightness = source[pixel * 3U];
        float red = lightness;
        float green = lightness;
        float blue = lightness;
        float weight = 0.0F;
        if (lightness < 0.5F - compression)
        {
            hsl::hsl_to_rgb(0.6F, 0.8F, lightness, red, green, blue);
            weight = std::clamp((0.5F - compression - lightness) * 2.0F, 0.0F, 1.0F);
        }
        else if (lightness > 0.5F + compression)
        {
            hsl::hsl_to_rgb(0.1F, 0.7F, lightness, red, green, blue);
            weight = std::clamp((lightness - (0.5F + compression)) * 2.0F, 0.0F, 1.0F);
        }
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float toned = channel == 0U ? red : channel == 1U ? green : blue;
            const float expected =
                std::clamp(lightness * (1.0F - weight) + toned * weight, 0.0F, 1.0F);
            EXPECT_FLOAT_EQ(result.value().rgb[pixel * 3U + channel], expected);
        }
    }

    params.mix = 0.0;
    auto identity = apply_split_toning(image(source), params, {});
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity.value().rgb, source);
}

TEST(SplitToningTest, CanonicalAllMaskMatchesUnmaskedOutput)
{
    SplitToningParams params{0.6, 0.8, 0.1, 0.7, 0.5, 22.0, 1.0};
    auto parameters = split_toning_to_parameters(params);
    ASSERT_TRUE(parameters);
    Recipe plain;
    plain.asset = {"asset", "file:///fixture.raw", std::nullopt};
    plain.operations = {
        {"ravo.color.input", 1, "input", true, input_color_to_parameters({}), std::nullopt},
        {std::string(kSplitToningOperationId), kSplitToningOperationSchemaVersion, "split", true,
         parameters.value(), std::nullopt},
        {"ravo.color.output", 1, "output", true, output_color_to_parameters({}), std::nullopt},
    };
    Recipe masked = plain;
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    masked.masks.push_back(std::move(all));
    masked.operations[1].mask_id = "all";
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto source = image({0.2F, 0.2F, 0.2F, 0.8F, 0.8F, 0.8F});
    auto unmasked = engine.value().render_linear_working(source, plain, {});
    auto attached = engine.value().render_linear_working(source, masked, {});
    ASSERT_TRUE(unmasked) << unmasked.error().message;
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached.value().rgb, unmasked.value().rgb);
}

struct CancelState
{
    CancellationSource *source = nullptr;
};

void cancel_split(void *context, const detail::SplitToningCheckpoint checkpoint,
                  const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelState *>(context);
    if (checkpoint == detail::SplitToningCheckpoint::kProcessRow && progress == 1U)
        static_cast<void>(state.source->cancel("split-toning-checkpoint"));
}

TEST(SplitToningTest, NonfiniteAndCancellationKeepSourceImmutable)
{
    auto source_image = image(std::vector<float>(8U * 4U * 3U, 0.25F));
    const auto original = source_image.rgb;
    source_image.height = 4U;
    source_image.width = 8U;
    CancellationSource source;
    CancelState state{&source};
    auto cancelled = detail::apply_split_toning_controlled(source_image, SplitToningParams{},
                                                           source.token(), {&state, cancel_split});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(source_image.rgb, original);

    source_image.rgb[2U] = std::numeric_limits<float>::quiet_NaN();
    auto rejected = apply_split_toning(source_image, SplitToningParams{}, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_split_toning_input");
}

TEST(SplitToningLegacyXmpTest, FrozenV1SingletonMapsAndModifiedPayloadRejects)
{
    constexpr std::string_view parameters = "7b14ae3e6666663f7b146e3f6666663f3433b33e01007041";
    auto imported =
        import_legacy_xmp({split_xmp(parameters), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto found =
        std::find_if(imported.value().operations.begin(), imported.value().operations.end(),
                     [](const auto &operation) { return operation.id == kSplitToningOperationId; });
    ASSERT_NE(found, imported.value().operations.end());
    auto parsed = split_toning_from_parameters(found->parameters);
    ASSERT_TRUE(parsed);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().shadow_hue), 0.3400000035762787F);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().highlight_hue), 0.9300000071525574F);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().compress), 15.000000953674316F);
    EXPECT_DOUBLE_EQ(parsed.value().mix, 1.0);

    std::string modified(parameters);
    modified[0] = modified[0] == '7' ? '6' : '7';
    auto rejected =
        import_legacy_xmp({split_xmp(modified), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_split_toning_parameters");
}

} // namespace
} // namespace ravo
