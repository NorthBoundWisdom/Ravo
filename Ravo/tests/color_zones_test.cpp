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

#include "color_zones.h"
#include "d50_lab.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/color_zones.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string color_zones_xmp(const std::string_view parameters)
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="10" darktable:operation="colorzones" darktable:enabled="1"
    darktable:modversion="5" darktable:params=")") +
           std::string(parameters) +
           R"(" darktable:multi_name="" darktable:multi_name_hand_edited="0"
    darktable:multi_priority="0" darktable:blendop_version="9"
    darktable:blendop_params="gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ="/>
  </rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
}

[[nodiscard]] WorkingImage image_from_lab(const std::array<float, 3> &lab,
                                          const std::uint32_t width = 1U,
                                          const std::uint32_t height = 1U)
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

[[nodiscard]] std::array<float, 3> output_lab(const WorkingImage &image)
{
    return d50_lab::xyz_to_lab(
        d50_lab::linear_rec709_to_xyz({image.rgb[0], image.rgb[1], image.rgb[2]}));
}

TEST(ColorZonesRecipeTest, SchemaSupportsEveryPartitionAndInterpolationStrictly)
{
    ColorZonesParams params;
    params.select_by = ColorZonesChannel::kChroma;
    params.curves[0].interpolation = ColorZonesInterpolation::kCubicSpline;
    params.curves[1].interpolation = ColorZonesInterpolation::kCatmullRom;
    params.curves[2].interpolation = ColorZonesInterpolation::kMonotoneHermite;
    params.curves[0].points = {{0.0, 0.5}, {0.4, 0.7}, {1.0, 0.5}};
    params.curves[1].points = {{0.0, 0.5}, {1.0, 0.5}};
    params.curves[2].points = {{0.0, 0.5}, {1.0, 0.5}};
    params.strength = 25.0;
    auto parameters = color_zones_to_parameters(params);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(color_zones_from_parameters(parameters.value()).value(), params);

    params.curves[0].points[1].x = 0.001;
    auto unordered = color_zones_to_parameters(params);
    ASSERT_FALSE(unordered);
    EXPECT_EQ(unordered.error().context.at("parameter"), "lightness_curve");

    ColorZonesParams periodic;
    periodic.curves[0].points = {{0.0, 0.5}, {0.999, 0.5}};
    auto close_wrap = color_zones_to_parameters(periodic);
    ASSERT_FALSE(close_wrap);
    EXPECT_EQ(close_wrap.error().context.at("reason"), "invalid_color_zones_parameters");

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorZonesEnabled", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorZonesBandIndex", 3.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "colorZonesChroma", 0.75));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip);
    EXPECT_TRUE(roundtrip.value().color_zones_enabled);
    EXPECT_DOUBLE_EQ(roundtrip.value().color_zones.curves[1].points[3].y, 0.75);
}

TEST(ColorZonesTest, IdentityAndConstantLabAdjustmentsMatchScalarFormula)
{
    const std::array<float, 3> source_lab{40.0F, 64.0F, 0.0F};
    auto identity_input = image_from_lab(source_lab);
    auto identity = apply_color_zones(identity_input, ColorZonesParams{}, {});
    ASSERT_TRUE(identity) << identity.error().message;
    const auto identity_lab = output_lab(identity.value());
    EXPECT_NEAR(identity_lab[0], source_lab[0], 2.0e-4F);
    EXPECT_NEAR(identity_lab[1], source_lab[1], 2.0e-4F);
    EXPECT_NEAR(identity_lab[2], source_lab[2], 2.0e-4F);

    ColorZonesParams adjusted;
    for (auto &point : adjusted.curves[0].points)
        point.y = 0.75;
    for (auto &point : adjusted.curves[1].points)
        point.y = 0.75;
    for (auto &point : adjusted.curves[2].points)
        point.y = 0.625;
    auto result = apply_color_zones(image_from_lab({50.0F, 128.0F, 0.0F}), adjusted, {});
    ASSERT_TRUE(result) << result.error().message;
    const auto lab = output_lab(result.value());
    const float lut_lightness = 49151.0F / 65536.0F;
    const float lut_hue = 40959.0F / 65536.0F;
    EXPECT_NEAR(lab[0], 50.0F * std::pow(2.0F, 4.0F * (lut_lightness - 0.5F)), 2.0e-3F);
    EXPECT_NEAR(std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]), 2.0F * lut_lightness * 128.0F,
                2.0e-3F);
    EXPECT_NEAR(std::atan2(lab[2], lab[1]), 6.28318530717958647692F * (lut_hue - 0.5F), 2.0e-4F);
}

TEST(ColorZonesTest, EverySplineTypeBuildsPeriodicAndNonperiodicLuts)
{
    for (const auto interpolation :
         {ColorZonesInterpolation::kCubicSpline, ColorZonesInterpolation::kCatmullRom,
          ColorZonesInterpolation::kMonotoneHermite})
    {
        for (const auto selection : {ColorZonesChannel::kLightness, ColorZonesChannel::kHue})
        {
            ColorZonesParams params;
            params.select_by = selection;
            for (auto &curve : params.curves)
            {
                curve.points = {{0.125, 0.5}, {0.5, 0.6}, {0.875, 0.5}};
                curve.interpolation = interpolation;
            }
            auto result = apply_color_zones(image_from_lab({37.5F, 64.0F, 16.0F}), params, {});
            ASSERT_TRUE(result) << color_zones_interpolation_name(interpolation) << ": "
                                << result.error().message;
            EXPECT_TRUE(std::all_of(result.value().rgb.begin(), result.value().rgb.end(),
                                    [](const float value) { return std::isfinite(value); }));
        }
    }
}

TEST(ColorZonesTest, AllCanonicalMaskMatchesUnmaskedOutput)
{
    ColorZonesParams params;
    for (auto &point : params.curves[1].points)
        point.y = 0.75;
    auto parameters = color_zones_to_parameters(params);
    ASSERT_TRUE(parameters);
    Recipe unmasked;
    unmasked.asset = {"asset", "file:///fixture.raw", std::nullopt};
    unmasked.operations = {
        {"ravo.color.input", 1, "input", true, input_color_to_parameters({}), std::nullopt},
        {std::string(kColorZonesOperationId), kColorZonesOperationSchemaVersion, "zones", true,
         parameters.value(), std::nullopt},
        {"ravo.color.output", 1, "output", true, output_color_to_parameters({}), std::nullopt},
    };
    Recipe masked = unmasked;
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    masked.masks.push_back(std::move(all));
    masked.operations[1].mask_id = "all";
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto source = image_from_lab({50.0F, 40.0F, 20.0F}, 3U, 2U);
    auto plain = engine.value().render_linear_working(source, unmasked, {});
    auto attached = engine.value().render_linear_working(source, masked, {});
    ASSERT_TRUE(plain) << plain.error().message;
    ASSERT_TRUE(attached) << attached.error().message;
    EXPECT_EQ(attached.value().rgb, plain.value().rgb);
}

struct CancelState
{
    CancellationSource *source = nullptr;
    detail::ColorZonesCheckpoint target = detail::ColorZonesCheckpoint::kProcessRow;
};

void cancel_zones(void *context, const detail::ColorZonesCheckpoint checkpoint,
                  const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelState *>(context);
    if (checkpoint == state.target &&
        (checkpoint == detail::ColorZonesCheckpoint::kBeforePublication || progress == 1U))
        static_cast<void>(state.source->cancel("color-zones-checkpoint"));
}

TEST(ColorZonesTest, NonfiniteAndCancellationKeepTheSourceImmutable)
{
    auto input = image_from_lab({50.0F, 20.0F, 10.0F}, 8U, 4U);
    const auto original = input.rgb;
    CancellationSource source;
    CancelState state{&source, detail::ColorZonesCheckpoint::kProcessRow};
    auto cancelled = detail::apply_color_zones_controlled(input, ColorZonesParams{}, source.token(),
                                                          {&state, cancel_zones});
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(input.rgb, original);

    input.rgb[5U] = std::numeric_limits<float>::quiet_NaN();
    auto rejected = apply_color_zones(input, ColorZonesParams{}, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "nonfinite_color_zones_input");
    EXPECT_TRUE(std::isnan(input.rgb[5U]));

    CancellationSource lut_source;
    CancelState lut_state{&lut_source, detail::ColorZonesCheckpoint::kBuildLut};
    auto lut_cancelled = detail::apply_color_zones_controlled(
        image_from_lab({50.0F, 20.0F, 10.0F}, 2U, 2U), ColorZonesParams{}, lut_source.token(),
        {&lut_state, cancel_zones});
    ASSERT_FALSE(lut_cancelled);
    EXPECT_EQ(lut_cancelled.error().code, ErrorCode::kCancelled);
}

TEST(ColorZonesLegacyXmpTest, FrozenV5SingletonMapsAndModifiedPayloadRejects)
{
    constexpr std::string_view parameters =
        "gz08eJxjYgCBBjsgYf9stZ596FsdewYGB3sQn2GQAK3z/2z1FVLtYO4cbO6DuSts/Sk7p8wFdoPNfcxImBEJwwCIDQAtfA+o";
    auto imported = import_legacy_xmp(
        {color_zones_xmp(parameters), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto found =
        std::find_if(imported.value().operations.begin(), imported.value().operations.end(),
                     [](const auto &operation) { return operation.id == kColorZonesOperationId; });
    ASSERT_NE(found, imported.value().operations.end());
    auto parsed = color_zones_from_parameters(found->parameters);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().select_by, ColorZonesChannel::kHue);
    EXPECT_EQ(parsed.value().curves[0].points.size(), 3U);
    EXPECT_FLOAT_EQ(static_cast<float>(parsed.value().curves[0].points[1].x), 0.6823104619979858F);
    EXPECT_EQ(parsed.value().curves[0].interpolation, ColorZonesInterpolation::kCatmullRom);

    std::string modified(parameters);
    modified.back() = modified.back() == 'A' ? 'B' : 'A';
    auto rejected = import_legacy_xmp(
        {color_zones_xmp(modified), {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_color_zones_parameters");
}

} // namespace
} // namespace ravo
