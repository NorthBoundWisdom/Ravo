#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/watermark.h"
#include "watermark.h"

namespace ravo
{
namespace
{

[[nodiscard]] ProfiledOutputBuffer profiled(const std::uint32_t width, const std::uint32_t height,
                                            const float value)
{
    ProfiledOutputBuffer result;
    result.width = width;
    result.height = height;
    result.channels.assign(static_cast<std::size_t>(width) * height * 3U, value);
    result.color_profile.kind = ColorProfileKind::kBuiltin;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = "encoded-output";
    return result;
}

[[nodiscard]] LinearWorkingBuffer working(const std::uint32_t width, const std::uint32_t height,
                                          const float value)
{
    LinearWorkingBuffer result;
    result.width = width;
    result.height = height;
    result.rgb.assign(static_cast<std::size_t>(width) * height * 3U, value);
    result.color_profile.kind = ColorProfileKind::kBuiltin;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = "linear_rec709";
    result.color_profile.has_matrix = true;
    result.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                              0.2225045F, 0.7168786F, 0.0606169F,
                                              0.0139322F, 0.0971045F, 0.7141733F};
    result.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    return result;
}

[[nodiscard]] std::string watermark_xmp(const std::string_view parameters)
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="8" darktable:operation="watermark" darktable:enabled="1"
    darktable:modversion="5" darktable:params=")") +
           std::string(parameters) +
           R"(" darktable:multi_name="" darktable:multi_priority="0"
    darktable:blendop_version="9"
    darktable:blendop_params="gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ="/>
  </rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
}

TEST(WatermarkRecipeTest, SchemaExpansionDevelopAndOutputOrderAreStrict)
{
    WatermarkParams params;
    params.text = "{stem} / {asset_id}";
    params.color = {0.1, 0.2, 0.3};
    params.opacity = 0.75;
    params.scale_percent = 12.0;
    params.x_offset = -0.1;
    params.y_offset = 0.2;
    params.alignment = WatermarkAlignment::kTopCenter;
    params.rotation_degrees = 15.0;
    auto parameters = watermark_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(watermark_from_parameters(parameters.value()).value(), params);
    auto expanded = expand_watermark_text(params.text,
                                          {"asset-7", "file:///photos/IMG_0042.CR2", std::nullopt});
    ASSERT_TRUE(expanded);
    EXPECT_EQ(expanded.value(), "IMG_0042 / asset-7");

    params.text = "bad {unknown}";
    auto unknown = watermark_to_parameters(params);
    ASSERT_FALSE(unknown);
    params.text = "é";
    auto unicode = watermark_to_parameters(params);
    ASSERT_FALSE(unicode);
    EXPECT_EQ(unicode.error().context.at("reason"), "unsupported_watermark_character");

    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.frame_present = true;
    develop.frame_enabled = true;
    develop.watermark_present = true;
    develop.watermark_enabled = true;
    develop.watermark.text = "RAVO";
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_GE(recipe.value().operations.size(), 4U);
    EXPECT_EQ(recipe.value().operations[recipe.value().operations.size() - 3U].id,
              kOutputDitherOperationId);
    EXPECT_EQ(recipe.value().operations[recipe.value().operations.size() - 2U].id,
              kFrameOperationId);
    EXPECT_EQ(recipe.value().operations.back().id, kWatermarkOperationId);
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip);
    EXPECT_TRUE(roundtrip.value().watermark_enabled);
    EXPECT_EQ(roundtrip.value().watermark.text, "RAVO");

    auto invalid = recipe.value();
    std::swap(invalid.operations[invalid.operations.size() - 2U], invalid.operations.back());
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto rejected = engine.value().validate(invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_output_dither_order");
}

TEST(WatermarkTest, FiveBySevenGlyphAlphaAndBlendAreExact)
{
    WatermarkParams params;
    params.text = "A";
    params.color = {1.0, 0.0, 0.0};
    params.opacity = 0.5;
    params.scale_percent = 50.0;
    params.alignment = WatermarkAlignment::kTopLeft;
    auto result = apply_watermark(profiled(12U, 10U, 0.0F), params,
                                  {"asset", "file:///fixture.raw", std::nullopt}, {});
    ASSERT_TRUE(result) << result.error().message;
    constexpr std::array<std::uint8_t, 7> rows{14, 17, 17, 31, 17, 17, 17};
    for (std::uint32_t y = 0U; y < 10U; ++y)
    {
        for (std::uint32_t x = 0U; x < 12U; ++x)
        {
            const bool marked = x < 5U && y < 7U && (rows[y] & (1U << (4U - x))) != 0U;
            const std::size_t index = (static_cast<std::size_t>(y) * 12U + x) * 3U;
            EXPECT_FLOAT_EQ(result.value().channels[index], marked ? 0.5F : 0.0F);
            EXPECT_FLOAT_EQ(result.value().channels[index + 1U], 0.0F);
            EXPECT_FLOAT_EQ(result.value().channels[index + 2U], 0.0F);
        }
    }
}

TEST(WatermarkTest, EngineAppliesWatermarkAfterDitherAndFrame)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kPosterize2, -100.0};
    develop.frame_present = true;
    develop.frame_enabled = true;
    develop.frame.size = 0.5;
    develop.frame.border_color = {0.0, 0.0, 0.0};
    develop.watermark_present = true;
    develop.watermark_enabled = true;
    develop.watermark.text = "A";
    develop.watermark.color = {1.0, 1.0, 1.0};
    develop.watermark.opacity = 1.0;
    develop.watermark.scale_percent = 50.0;
    develop.watermark.alignment = WatermarkAlignment::kTopLeft;
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe);
    auto rendered = engine.value().render_linear_working_export(
        working(2U, 2U, 0.25F), recipe.value(), RenderSampleKind::kRgbFloat, {});
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 4U);
    const auto &samples = std::get<std::vector<float>>(rendered.value().samples);
    const std::size_t top_mark = 1U * 3U;
    EXPECT_EQ(
        (std::array<float, 3>{samples[top_mark], samples[top_mark + 1U], samples[top_mark + 2U]}),
        (std::array<float, 3>{1.0F, 1.0F, 1.0F}));
    EXPECT_EQ((std::array<float, 3>{samples[0], samples[1], samples[2]}),
              (std::array<float, 3>{0.0F, 0.0F, 0.0F}));
}

struct CancelState
{
    CancellationSource *source = nullptr;
    detail::WatermarkCheckpoint target = detail::WatermarkCheckpoint::kProcessRow;
};

void cancel_watermark(void *context, const detail::WatermarkCheckpoint checkpoint,
                      const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelState *>(context);
    if (checkpoint == state.target &&
        (checkpoint == detail::WatermarkCheckpoint::kBeforePublication || progress == 1U))
        static_cast<void>(state.source->cancel("watermark-checkpoint"));
}

TEST(WatermarkTest, NonfiniteAndCancellationFailWithoutMutatingTheSource)
{
    auto input = profiled(12U, 10U, 0.25F);
    const auto original = input.channels;
    CancellationSource source;
    CancelState state{&source, detail::WatermarkCheckpoint::kProcessRow};
    auto cancelled = detail::apply_watermark_controlled(
        input, WatermarkParams{}, {"asset", "file:///fixture.raw", std::nullopt}, source.token(),
        {&state, cancel_watermark});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.channels, original);

    input.channels[7U] = std::numeric_limits<float>::infinity();
    auto nonfinite = apply_watermark(input, WatermarkParams{},
                                     {"asset", "file:///fixture.raw", std::nullopt}, {});
    ASSERT_FALSE(nonfinite);
    EXPECT_EQ(nonfinite.error().context.at("reason"), "nonfinite_watermark_input");
    EXPECT_TRUE(std::isinf(input.channels[7U]));
}

TEST(WatermarkLegacyXmpTest, MissingPromoSvgRecordRejectsInsteadOfBecomingANoop)
{
    constexpr std::string_view parameters =
        "gz13eJxjZPjgyMDg4cwABRwMCFBQlJ+br1dcls4wCkYBZcAlNSsxrFQhODGvWMHQgHT9APv/CeY=";
    auto imported = import_legacy_xmp(
        {watermark_xmp(parameters), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_watermark_resource");
}

} // namespace
} // namespace ravo
