#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "engine_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using namespace engine_test_support;

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

TEST(EngineFacadeTest, PhaseOneControlsChangeSyntheticRaster)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto base_raster = gradient_raster();
    Recipe identity;
    identity.asset = {"raster", "memory:raster", std::nullopt};
    declare_input(identity);
    RenderRequest identity_request;
    identity_request.asset = identity.asset;
    identity_request.recipe = identity;
    auto base = engine.value().render_to_image(identity_request, &base_raster);
    ASSERT_TRUE(base) << base.error().message;
    const auto base_mean = mean_luma(base.value());

    auto exposed = render_op(engine.value(), base_raster,
                             {"ravo.core.exposure",
                              1,
                              "exposure-1",
                              true,
                              {{"exposure_ev", ParameterValue{1.0}}},
                              std::nullopt});
    ASSERT_TRUE(exposed) << exposed.error().message;
    EXPECT_GT(mean_luma(exposed.value()), base_mean);

    auto shadowed = render_op(engine.value(), base_raster,
                              {"ravo.core.shadows",
                               1,
                               "shadows-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(shadowed) << shadowed.error().message;
    EXPECT_GT(mean_luma(shadowed.value()), base_mean);

    auto highlighted = render_op(engine.value(), base_raster,
                                 {"ravo.core.highlights",
                                  1,
                                  "highlights-1",
                                  true,
                                  {{"amount", ParameterValue{-0.8}}},
                                  std::nullopt});
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_LT(mean_luma(highlighted.value()), base_mean);

    auto contrast = render_op(engine.value(), base_raster,
                              {"ravo.core.contrast",
                               1,
                               "contrast-1",
                               true,
                               {{"amount", ParameterValue{0.6}}},
                               std::nullopt});
    ASSERT_TRUE(contrast) << contrast.error().message;

    auto whites = render_op(engine.value(), base_raster,
                            {"ravo.core.whites",
                             1,
                             "whites-1",
                             true,
                             {{"amount", ParameterValue{-0.5}}},
                             std::nullopt});
    ASSERT_TRUE(whites) << whites.error().message;

    auto blacks = render_op(
        engine.value(), base_raster,
        {"ravo.core.blacks", 1, "blacks-1", true, {{"amount", ParameterValue{0.4}}}, std::nullopt});
    ASSERT_TRUE(blacks) << blacks.error().message;

    auto vibrance = render_op(engine.value(), base_raster,
                              {"ravo.color.vibrance",
                               1,
                               "vibrance-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(vibrance) << vibrance.error().message;

    TemperatureParams manual_wb;
    manual_wb.mode = std::string(kTemperatureModeManual);
    manual_wb.coefficients = std::array<double, kTemperatureChannelCount>{1.3, 0.9, 0.7, 1.0};
    auto wb = render_op(engine.value(), base_raster, temperature_operation(manual_wb));
    ASSERT_TRUE(wb) << wb.error().message;
    const auto mid = (8U * 16U + 8U) * 3U;
    EXPECT_NE(wb.value().rgb[mid], base.value().rgb[mid]);
    EXPECT_NE(wb.value().rgb[mid + 2U], base.value().rgb[mid + 2U]);

    auto gamma = render_op(
        engine.value(), base_raster,
        {"ravo.core.gamma", 1, "gamma-1", true, {{"gamma", ParameterValue{1.8}}}, std::nullopt});
    ASSERT_TRUE(gamma) << gamma.error().message;

    auto velvia = render_op(engine.value(), base_raster,
                            {"ravo.color.velvia",
                             1,
                             "velvia-1",
                             true,
                             {{"amount", ParameterValue{0.8}}, {"bias", ParameterValue{1.0}}},
                             std::nullopt});
    ASSERT_TRUE(velvia) << velvia.error().message;

    auto balance = render_op(engine.value(), base_raster,
                             color_balance_rgb_operation(test::color_balance_0093_params()));
    ASSERT_TRUE(balance) << balance.error().message;

    auto contrast_color = render_op(engine.value(), base_raster,
                                    {"ravo.color.colorcontrast",
                                     1,
                                     "cc-1",
                                     true,
                                     {{"amount", ParameterValue{0.7}}},
                                     std::nullopt});
    ASSERT_TRUE(contrast_color) << contrast_color.error().message;

    auto mono = render_op(engine.value(), solid_raster(8, 8, 220, 20, 20),
                          {"ravo.color.monochrome",
                           1,
                           "mono-1",
                           true,
                           {{"amount", ParameterValue{1.0}}},
                           std::nullopt});
    ASSERT_TRUE(mono) << mono.error().message;
    EXPECT_NEAR(mono.value().rgb[0], mono.value().rgb[1], 8);
    EXPECT_NEAR(mono.value().rgb[1], mono.value().rgb[2], 8);

    auto split = render_op(engine.value(), base_raster,
                           {"ravo.color.splittoning",
                            1,
                            "split-1",
                            true,
                            {{"shadows_hue", ParameterValue{0.6}},
                             {"highlights_hue", ParameterValue{0.1}},
                             {"balance", ParameterValue{0.5}},
                             {"amount", ParameterValue{0.8}}},
                            std::nullopt});
    ASSERT_TRUE(split) << split.error().message;

    auto crop = render_op(engine.value(), base_raster,
                          {"ravo.geometry.crop",
                           1,
                           "crop-1",
                           true,
                           {{"x", ParameterValue{0.25}},
                            {"y", ParameterValue{0.25}},
                            {"width", ParameterValue{0.5}},
                            {"height", ParameterValue{0.5}}},
                           std::nullopt});
    ASSERT_TRUE(crop) << crop.error().message;
    EXPECT_EQ(crop.value().width, 8U);
    EXPECT_EQ(crop.value().height, 8U);

    RasterBuffer sided;
    sided.width = 4;
    sided.height = 2;
    sided.srgb = {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0,
                  255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0};
    auto flipped = render_op(engine.value(), sided,
                             {"ravo.geometry.flip",
                              1,
                              "flip-1",
                              true,
                              {{"horizontal", ParameterValue{std::int64_t{1}}},
                               {"vertical", ParameterValue{std::int64_t{0}}}},
                              std::nullopt});
    ASSERT_TRUE(flipped) << flipped.error().message;
    EXPECT_EQ(flipped.value().rgb[0], 0);
    EXPECT_EQ(flipped.value().rgb[1], 255);

    RasterBuffer unique;
    unique.width = 3;
    unique.height = 2;
    unique.srgb = {10, 20, 30, 40, 50, 60, 70, 80, 90, 11, 21, 31, 41, 51, 61, 71, 81, 91};
    declare_srgb(unique);
    const auto sample = [](const RenderedImage &image, const std::uint32_t x, const std::uint32_t y)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
        return std::array<std::uint8_t, 3>{image.rgb[index], image.rgb[index + 1U],
                                           image.rgb[index + 2U]};
    };
    for (std::int32_t orientation = 0; orientation <= 7; ++orientation)
    {
        SCOPED_TRACE(orientation);
        auto geometry = leftover_flip_orientation_to_geometry(orientation);
        ASSERT_TRUE(geometry) << geometry.error().message;
        Recipe recipe;
        recipe.asset = {"raster", "memory:raster", std::nullopt};
        declare_input(recipe);
        if (geometry.value().rotate_quarters != 0)
        {
            recipe.operations.push_back(
                {"ravo.geometry.rotate",
                 1,
                 "rotate-1",
                 true,
                 {{"quarters", ParameterValue{geometry.value().rotate_quarters}}},
                 std::nullopt});
        }
        if (geometry.value().flip_horizontal != 0 || geometry.value().flip_vertical != 0)
        {
            recipe.operations.push_back(
                {"ravo.geometry.flip",
                 1,
                 "flip-1",
                 true,
                 {{"horizontal", ParameterValue{geometry.value().flip_horizontal}},
                  {"vertical", ParameterValue{geometry.value().flip_vertical}}},
                 std::nullopt});
        }
        RenderRequest request;
        request.asset = recipe.asset;
        request.recipe = recipe;
        auto rendered = engine.value().render_to_image(request, &unique);
        ASSERT_TRUE(rendered) << rendered.error().message;
        if (orientation == 0)
        {
            EXPECT_EQ(rendered.value().width, 3U);
            EXPECT_EQ(rendered.value().height, 2U);
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 2, 1), (std::array<std::uint8_t, 3>{71, 81, 91}));
        }
        else if (orientation == 5)
        {
            EXPECT_EQ(rendered.value().width, 2U);
            EXPECT_EQ(rendered.value().height, 3U);
            EXPECT_EQ(sample(rendered.value(), 1, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{11, 21, 31}));
        }
        else if (orientation == 4)
        {
            EXPECT_EQ(rendered.value().width, 2U);
            EXPECT_EQ(rendered.value().height, 3U);
            EXPECT_EQ(sample(rendered.value(), 0, 0), (std::array<std::uint8_t, 3>{10, 20, 30}));
            EXPECT_EQ(sample(rendered.value(), 1, 0), (std::array<std::uint8_t, 3>{11, 21, 31}));
        }
    }

    auto vignette = render_op(engine.value(), solid_raster(32, 32, 200, 200, 200),
                              {"ravo.effect.vignette",
                               1,
                               "vig-1",
                               true,
                               {{"amount", ParameterValue{1.0}},
                                {"midpoint", ParameterValue{0.3}},
                                {"falloff", ParameterValue{0.4}}},
                               std::nullopt});
    ASSERT_TRUE(vignette) << vignette.error().message;
    const auto center = (16U * 32U + 16U) * 3U;
    const auto corner = 0U;
    EXPECT_GT(vignette.value().rgb[center], vignette.value().rgb[corner]);

    auto grain_a = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    auto grain_b = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    ASSERT_TRUE(grain_a) << grain_a.error().message;
    ASSERT_TRUE(grain_b) << grain_b.error().message;
    EXPECT_EQ(grain_a.value().rgb, grain_b.value().rgb);
    EXPECT_NE(grain_a.value().rgb, solid_raster(12, 12, 120, 120, 120).srgb);

    auto sharpen = render_op(engine.value(), base_raster,
                             {"ravo.detail.sharpen",
                              1,
                              "sharp-1",
                              true,
                              {{"amount", ParameterValue{1.2}},
                               {"radius", ParameterValue{1.0}},
                               {"threshold", ParameterValue{0.0}}},
                              std::nullopt});
    ASSERT_TRUE(sharpen) << sharpen.error().message;

    auto clarity = render_op(engine.value(), base_raster,
                             {"ravo.detail.clarity",
                              1,
                              "clarity-1",
                              true,
                              {{"amount", ParameterValue{0.6}}},
                              std::nullopt});
    ASSERT_TRUE(clarity) << clarity.error().message;

    auto bloom = render_op(
        engine.value(), base_raster,
        {"ravo.effect.bloom", 1, "bloom-1", true, {{"amount", ParameterValue{0.7}}}, std::nullopt});
    ASSERT_TRUE(bloom) << bloom.error().message;

    auto soften = render_op(engine.value(), base_raster,
                            {"ravo.effect.soften",
                             1,
                             "soften-1",
                             true,
                             {{"amount", ParameterValue{0.7}}},
                             std::nullopt});
    ASSERT_TRUE(soften) << soften.error().message;

    auto dehaze = render_op(engine.value(), base_raster,
                            {"ravo.effect.dehaze",
                             1,
                             "dehaze-1",
                             true,
                             {{"amount", ParameterValue{0.5}}},
                             std::nullopt});
    ASSERT_FALSE(dehaze);
    EXPECT_EQ(dehaze.error().context.at("reason"), "dehaze_raster_source_unsupported");

    auto straightened = render_op(engine.value(), solid_raster(16, 16, 200, 20, 20),
                                  {"ravo.geometry.straighten",
                                   1,
                                   "straighten-1",
                                   true,
                                   {{"degrees", ParameterValue{15.0}}},
                                   std::nullopt});
    ASSERT_TRUE(straightened) << straightened.error().message;
    EXPECT_EQ(straightened.value().width, 16U);
    EXPECT_EQ(straightened.value().height, 16U);
    EXPECT_LT(straightened.value().rgb[0], 40);
    const auto straighten_mid = (8U * 16U + 8U) * 3U;
    EXPECT_GT(straightened.value().rgb[straighten_mid], 80);
    bool saw_antialiased_edge = false;
    for (std::size_t index = 0; index + 2 < straightened.value().rgb.size(); index += 3)
    {
        const auto red = straightened.value().rgb[index];
        if (red > 20 && red < 160)
        {
            saw_antialiased_edge = true;
            break;
        }
    }
    EXPECT_TRUE(saw_antialiased_edge);

    auto graduated = render_op(engine.value(), solid_raster(32, 32, 180, 180, 180),
                               {"ravo.effect.graduatednd",
                                1,
                                "grad-1",
                                true,
                                {{"density_ev", ParameterValue{1.5}},
                                 {"hardness", ParameterValue{0.8}},
                                 {"rotation_deg", ParameterValue{0.0}},
                                 {"offset", ParameterValue{0.0}}},
                                std::nullopt});
    ASSERT_TRUE(graduated) << graduated.error().message;
    const auto top = (2U * 32U + 16U) * 3U;
    const auto bottom = (30U * 32U + 16U) * 3U;
    EXPECT_LT(graduated.value().rgb[top], graduated.value().rgb[bottom]);

    const auto dark_raster = solid_raster(16, 16, 12, 12, 12);
    auto dark_base = engine.value().render_to_image(identity_request, &dark_raster);
    ASSERT_TRUE(dark_base) << dark_base.error().message;
    auto toneeq = render_op(engine.value(), dark_raster,
                            {"ravo.core.toneequal",
                             1,
                             "toneeq-1",
                             true,
                             {{"blacks", ParameterValue{1.2}},
                              {"shadows", ParameterValue{0.0}},
                              {"midtones", ParameterValue{0.0}},
                              {"highlights", ParameterValue{0.0}},
                              {"whites", ParameterValue{0.0}}},
                             std::nullopt});
    ASSERT_TRUE(toneeq) << toneeq.error().message;
    EXPECT_GT(mean_luma(toneeq.value()), mean_luma(dark_base.value()));

    ParameterValue::Array sat_bands(8, ParameterValue{0.0});
    sat_bands[0] = ParameterValue{0.8};
    auto coloreq =
        render_op(engine.value(), solid_raster(8, 8, 220, 30, 30),
                  {"ravo.color.colorequal",
                   1,
                   "ceq-1",
                   true,
                   {{"hue_shift", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}},
                    {"saturation", ParameterValue{sat_bands}},
                    {"lightness", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}}},
                   std::nullopt});
    ASSERT_TRUE(coloreq) << coloreq.error().message;

    auto lens = render_op(engine.value(), solid_raster(24, 24, 200, 80, 40),
                          {"ravo.geometry.lens",
                           1,
                           "lens-1",
                           true,
                           {{"mode", ParameterValue{"manual"}},
                            {"k1", ParameterValue{-0.4}},
                            {"k2", ParameterValue{0.1}},
                            {"tca_r", ParameterValue{1.02}},
                            {"tca_b", ParameterValue{0.98}},
                            {"vignetting", ParameterValue{0.6}}},
                           std::nullopt});
    ASSERT_TRUE(lens) << lens.error().message;
    EXPECT_NE(lens.value().rgb, solid_raster(24, 24, 200, 80, 40).srgb);

    auto missing_lens = render_op(engine.value(), solid_raster(8, 8, 120, 120, 120),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-1",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"Missing"}},
                                    {"camera_model", ParameterValue{"Camera"}},
                                    {"lens", ParameterValue{"Unknown"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_FALSE(missing_lens);
    EXPECT_EQ(missing_lens.error().code, ErrorCode::kNotFound);

    auto matched_lens = render_op(engine.value(), solid_raster(16, 16, 180, 180, 180),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-2",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"RavoTest"}},
                                    {"camera_model", ParameterValue{"RavoSensor"}},
                                    {"lens", ParameterValue{"FixtureLens"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_TRUE(matched_lens) << matched_lens.error().message;

    RasterBuffer noisy = solid_raster(32, 32, 120, 120, 120);
    for (std::size_t index = 0; index < noisy.srgb.size(); ++index)
    {
        const auto delta = static_cast<int>((index * 37U) % 41U) - 20;
        noisy.srgb[index] = static_cast<std::uint8_t>(std::clamp(120 + delta, 0, 255));
    }
    auto denoised = render_op(engine.value(), noisy,
                              {"ravo.detail.denoiseprofile",
                               1,
                               "denoise-1",
                               true,
                               {{"strength", ParameterValue{0.8}},
                                {"chroma", ParameterValue{1.0}},
                                {"radius", ParameterValue{1.5}}},
                               std::nullopt});
    ASSERT_TRUE(denoised) << denoised.error().message;
    auto denoised_again = render_op(engine.value(), noisy,
                                    {"ravo.detail.denoiseprofile",
                                     1,
                                     "denoise-1",
                                     true,
                                     {{"strength", ParameterValue{0.8}},
                                      {"chroma", ParameterValue{1.0}},
                                      {"radius", ParameterValue{1.5}}},
                                     std::nullopt});
    ASSERT_TRUE(denoised_again) << denoised_again.error().message;
    EXPECT_EQ(denoised_again.value().rgb, denoised.value().rgb);

    auto raw_on_raster = render_op(engine.value(), solid_raster(8, 8, 10, 10, 10),
                                   {"ravo.raw.highlights",
                                    1,
                                    "raw-hl-1",
                                    true,
                                    {{"mode", ParameterValue{"inpaint"}},
                                     {"amount", ParameterValue{1.0}},
                                     {"clip", ParameterValue{0.98}}},
                                    std::nullopt});
    ASSERT_FALSE(raw_on_raster);
    EXPECT_EQ(raw_on_raster.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
