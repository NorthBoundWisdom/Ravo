#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "d50_lab.h"
#include "monochrome.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/monochrome.h"

namespace ravo
{
namespace
{

[[nodiscard]] WorkingImage image_from_lab(const std::array<float, 3> &lab,
                                          const std::uint32_t width = 8U,
                                          const std::uint32_t height = 8U)
{
    const auto rgb = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(lab));
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t pixel = 0U; pixel < image.rgb.size() / 3U; ++pixel)
        std::copy(rgb.begin(), rgb.end(),
                  image.rgb.begin() + static_cast<std::ptrdiff_t>(pixel * 3U));
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = "linear_rec709";
    image.color_profile.has_matrix = true;
    image.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                             0.2225045F, 0.7168786F, 0.0606169F,
                                             0.0139322F, 0.0971045F, 0.7141733F};
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    return image;
}

[[nodiscard]] std::array<float, 3> first_lab(const WorkingImage &image)
{
    return d50_lab::xyz_to_lab(
        d50_lab::linear_rec709_to_xyz({image.rgb[0], image.rgb[1], image.rgb[2]}));
}

[[nodiscard]] float source_fast_exp(const float value) noexcept
{
    constexpr std::int32_t one = 0x3f800000;
    constexpr std::int32_t exponential = 0x402df854;
    const auto bits = static_cast<std::int32_t>(static_cast<float>(one) +
                                                value * static_cast<float>(exponential - one));
    return std::bit_cast<float>(std::max(bits, std::int32_t{0}));
}

[[nodiscard]] std::string monochrome_xmp(const std::string_view parameters)
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="9" darktable:operation="monochrome" darktable:enabled="1"
    darktable:modversion="2" darktable:params=")") +
           std::string(parameters) +
           R"(" darktable:multi_name="" darktable:multi_name_hand_edited="0"
    darktable:multi_priority="0" darktable:blendop_version="9"
    darktable:blendop_params="gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ="/>
  </rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
}

TEST(MonochromeRecipeTest, SchemaAndLegacyRavoAmountUpgradeAreStrict)
{
    MonochromeParams params{32.0, 64.0, 2.3, 0.4, 0.75};
    auto parameters = monochrome_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(monochrome_from_parameters(parameters.value()).value(), params);

    OperationInstance v1{std::string(kMonochromeOperationId), 1,           "old", true,
                         {{"amount", ParameterValue{0.25}}},  std::nullopt};
    ASSERT_TRUE(upgrade_monochrome_operation(v1));
    EXPECT_EQ(v1.schema_version, kMonochromeOperationSchemaVersion);
    auto upgraded = monochrome_from_parameters(v1.parameters);
    ASSERT_TRUE(upgraded);
    EXPECT_DOUBLE_EQ(upgraded.value().mix, 0.25);

    params.size = 0.49;
    auto invalid = monochrome_to_parameters(params);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("parameter"), "size");

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "monochromeEnabled", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "monochromeFilterA", 32.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "monochromeHighlights", 0.6));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip);
    EXPECT_TRUE(roundtrip.value().monochrome_enabled);
    EXPECT_DOUBLE_EQ(roundtrip.value().monochrome.filter_a, 32.0);
}

TEST(MonochromeTest, UniformFilterMatchesFrozenScalarAndClearsChroma)
{
    MonochromeParams params;
    params.filter_a = 20.0;
    params.filter_b = 10.0;
    auto selected = apply_monochrome(image_from_lab({50.0F, 20.0F, 10.0F}), params, {});
    ASSERT_TRUE(selected) << selected.error().message;
    const auto selected_lab = first_lab(selected.value());
    EXPECT_NEAR(selected_lab[0], 50.0F, 3.0e-3F);
    EXPECT_NEAR(selected_lab[1], 0.0F, 3.0e-3F);
    EXPECT_NEAR(selected_lab[2], 0.0F, 3.0e-3F);

    params.filter_a = -128.0;
    params.filter_b = -128.0;
    params.size = 0.5;
    auto rejected_color = apply_monochrome(image_from_lab({50.0F, 20.0F, 10.0F}), params, {});
    ASSERT_TRUE(rejected_color) << rejected_color.error().message;
    const auto rejected_lab = first_lab(rejected_color.value());
    const float da = 20.0F - (-128.0F);
    const float db = 10.0F - (-128.0F);
    const float sigma2 = 2.0F * (0.5F * 128.0F) * (0.5F * 128.0F);
    const float filter = source_fast_exp(-std::clamp((da * da + db * db) / sigma2, 0.0F, 1.0F));
    EXPECT_NEAR(rejected_lab[0], 50.0F * filter, 4.0e-3F);
    EXPECT_NEAR(rejected_lab[1], 0.0F, 3.0e-3F);
    EXPECT_NEAR(rejected_lab[2], 0.0F, 3.0e-3F);

    params.mix = 0.0;
    const auto source = image_from_lab({50.0F, 20.0F, 10.0F});
    auto identity = apply_monochrome(source, params, {});
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity.value().rgb, source.rgb);
}

TEST(MonochromeTest, CanonicalAllMaskMatchesUnmaskedOutput)
{
    MonochromeParams params{20.0, 10.0, 2.0, 0.5, 1.0};
    auto parameters = monochrome_to_parameters(params);
    ASSERT_TRUE(parameters);
    Recipe plain;
    plain.asset = {"asset", "file:///fixture.raw", std::nullopt};
    plain.operations = {
        {"ravo.color.input", 1, "input", true, input_color_to_parameters({}), std::nullopt},
        {std::string(kMonochromeOperationId), kMonochromeOperationSchemaVersion, "mono", true,
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
    const auto source = image_from_lab({50.0F, 20.0F, 10.0F});
    auto unmasked = engine.value().render_linear_working(source, plain, {});
    auto attached = engine.value().render_linear_working(source, masked, {});
    ASSERT_TRUE(unmasked) << unmasked.error().message;
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached.value().rgb, unmasked.value().rgb);
}

struct CancelState
{
    CancellationSource *source = nullptr;
    detail::MonochromeCheckpoint target = detail::MonochromeCheckpoint::kBeforeBilateral;
};

void cancel_monochrome(void *context, const detail::MonochromeCheckpoint checkpoint,
                       const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelState *>(context);
    if (checkpoint == state.target &&
        (checkpoint == detail::MonochromeCheckpoint::kBeforeBilateral || progress == 1U))
        static_cast<void>(state.source->cancel("monochrome-checkpoint"));
}

TEST(MonochromeTest, NonfiniteAndCancellationKeepSourceImmutable)
{
    const auto input = image_from_lab({50.0F, 20.0F, 10.0F});
    CancellationSource source;
    CancelState state{&source, detail::MonochromeCheckpoint::kBeforeBilateral};
    auto cancelled = detail::apply_monochrome_controlled(input, MonochromeParams{}, source.token(),
                                                         {&state, cancel_monochrome});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, image_from_lab({50.0F, 20.0F, 10.0F}).rgb);

    auto nonfinite = input;
    nonfinite.rgb[3U] = std::numeric_limits<float>::infinity();
    auto rejected = apply_monochrome(nonfinite, MonochromeParams{}, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_monochrome_input");
    EXPECT_TRUE(std::isinf(nonfinite.rgb[3U]));
}

TEST(MonochromeLegacyXmpTest, FrozenV2SingletonMapsAndModifiedPayloadRejects)
{
    constexpr std::string_view parameters = "5acafa4259234ec1000000409a99193f";
    auto imported = import_legacy_xmp(
        {monochrome_xmp(parameters), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto found =
        std::find_if(imported.value().operations.begin(), imported.value().operations.end(),
                     [](const auto &operation) { return operation.id == kMonochromeOperationId; });
    ASSERT_NE(found, imported.value().operations.end());
    auto parsed = monochrome_from_parameters(found->parameters);
    ASSERT_TRUE(parsed);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().filter_a), 125.39521789550781F);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().filter_b), -12.88362979888916F);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().highlights), 0.6000000238418579F);
    EXPECT_DOUBLE_EQ(parsed.value().mix, 1.0);

    std::string modified(parameters);
    modified[0] = modified[0] == '5' ? '4' : '5';
    auto rejected = import_legacy_xmp(
        {monochrome_xmp(modified), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_monochrome_parameters");
}

} // namespace
} // namespace ravo
