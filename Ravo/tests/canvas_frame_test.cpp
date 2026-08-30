#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "canvas_frame.h"
#include "mask_evaluator.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

[[nodiscard]] WorkingImage working_image(const std::uint32_t width, const std::uint32_t height,
                                         const std::array<float, 3> color)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t pixel = 0U; pixel < image.rgb.size() / 3U; ++pixel)
    {
        image.rgb[pixel * 3U] = color[0];
        image.rgb[pixel * 3U + 1U] = color[1];
        image.rgb[pixel * 3U + 2U] = color[2];
    }
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

[[nodiscard]] std::array<float, 3> pixel(const std::vector<float> &rgb, const std::uint32_t width,
                                         const std::uint32_t x, const std::uint32_t y)
{
    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3U;
    return {rgb[offset], rgb[offset + 1U], rgb[offset + 2U]};
}

[[nodiscard]] std::string geometry_xmp(const std::string_view operation,
                                       const std::string_view version,
                                       const std::string_view parameters,
                                       const std::string_view blend_version,
                                       const std::string_view blend)
{
    return std::string(R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="11" darktable:operation=")") +
           std::string(operation) + R"(" darktable:enabled="1" darktable:modversion=")" +
           std::string(version) + R"(" darktable:params=")" + std::string(parameters) +
           R"(" darktable:multi_name="" darktable:multi_name_hand_edited="0"
    darktable:multi_priority="0" darktable:blendop_version=")" +
           std::string(blend_version) + R"(" darktable:blendop_params=")" + std::string(blend) +
           R"("/></rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
}

TEST(CanvasFrameRecipeTest, SchemasDevelopAndOrderAreStrict)
{
    CanvasParams canvas{5.0, 10.0, 15.000000953674316, 20.0, CanvasColor::kBlue};
    auto canvas_parameters = canvas_to_parameters(canvas);
    ASSERT_TRUE(canvas_parameters) << canvas_parameters.error().message;
    EXPECT_EQ(canvas_from_parameters(canvas_parameters.value()).value(), canvas);
    FrameParams frame;
    frame.border_color = {0.1, 0.2, 0.3};
    frame.aspect = 16.0 / 9.0;
    frame.orientation = FrameOrientation::kLandscape;
    frame.size = 0.1;
    frame.position_h = 0.64;
    frame.position_v = 0.625;
    frame.frame_size = 0.05;
    frame.frame_offset = 1.0;
    frame.frame_color = {0.75, 0.1, 0.15};
    auto frame_parameters = frame_to_parameters(frame);
    ASSERT_TRUE(frame_parameters) << frame_parameters.error().message;
    EXPECT_EQ(frame_from_parameters(frame_parameters.value()).value(), frame);

    DevelopParams develop;
    develop.canvas_present = true;
    develop.canvas_enabled = true;
    develop.canvas = canvas;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kPosterize4, -100.0};
    develop.frame_present = true;
    develop.frame_enabled = true;
    develop.frame = frame;
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    ASSERT_GE(recipe.value().operations.size(), 4U);
    EXPECT_EQ(recipe.value().operations[recipe.value().operations.size() - 3U].id,
              "ravo.color.output");
    EXPECT_EQ(recipe.value().operations[recipe.value().operations.size() - 2U].id,
              kOutputDitherOperationId);
    EXPECT_EQ(recipe.value().operations.back().id, kFrameOperationId);
    auto restored = develop_from_recipe(recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().canvas, canvas);
    EXPECT_EQ(restored.value().frame, frame);

    DevelopParams composed = develop;
    composed.crop_width = 0.8;
    composed.perspective_vertical = 0.15;
    auto composed_recipe =
        recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, composed);
    ASSERT_TRUE(composed_recipe);
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto composed_ok = engine.value().validate(composed_recipe.value());
    ASSERT_TRUE(composed_ok) << composed_ok.error().message;

    Mask after_geometry_mask{"late", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    after_geometry_mask.payload = AllMask{};
    composed_recipe.value().masks.push_back(std::move(after_geometry_mask));
    const auto output = std::find_if(
        composed_recipe.value().operations.begin(), composed_recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == "ravo.color.output"; });
    ASSERT_NE(output, composed_recipe.value().operations.end());
    composed_recipe.value().operations.insert(
        output,
        OperationInstance{"ravo.effect.graduatednd",
                          1,
                          "late-mask-consumer",
                          true,
                          {{"density_ev", ParameterValue{0.5}},
                           {"hardness", ParameterValue{0.5}},
                           {"rotation_deg", ParameterValue{0.0}},
                           {"offset", ParameterValue{0.0}}},
                          std::string("late")});
    auto late_mask = engine.value().validate(composed_recipe.value());
    ASSERT_FALSE(late_mask);
    EXPECT_EQ(late_mask.error().context.at("reason"),
              "canvas_geometry_later_mask_unsupported");

    DevelopParams invalid = develop;
    invalid.rotate_quarters = 1;
    auto invalid_recipe =
        recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, invalid);
    ASSERT_TRUE(invalid_recipe);
    auto rejected = engine.value().validate(invalid_recipe.value());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "canvas_later_geometry_unsupported");

    DevelopParams disabled = invalid;
    disabled.canvas_enabled = false;
    auto disabled_recipe =
        recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, disabled);
    ASSERT_TRUE(disabled_recipe) << disabled_recipe.error().message;
    auto disabled_ok = engine.value().validate(disabled_recipe.value());
    ASSERT_TRUE(disabled_ok) << disabled_ok.error().message;
    auto disabled_restored = develop_from_recipe(disabled_recipe.value());
    ASSERT_TRUE(disabled_restored) << disabled_restored.error().message;
    EXPECT_TRUE(disabled_restored.value().canvas_present);
    EXPECT_FALSE(disabled_restored.value().canvas_enabled);

    DevelopParams toggle;
    ASSERT_TRUE(apply_develop_field(toggle, "canvasEnabled", 1.0));
    EXPECT_TRUE(toggle.canvas_present);
    EXPECT_TRUE(toggle.canvas_enabled);
    ASSERT_TRUE(apply_develop_field(toggle, "canvasEnabled", 0.0));
    EXPECT_FALSE(toggle.canvas_present);
    EXPECT_FALSE(toggle.canvas_enabled);
    auto cleared_recipe =
        recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, toggle);
    ASSERT_TRUE(cleared_recipe) << cleared_recipe.error().message;
    EXPECT_TRUE(std::none_of(
        cleared_recipe.value().operations.begin(), cleared_recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == kCanvasOperationId; }));
    auto cleared = develop_from_recipe(cleared_recipe.value());
    ASSERT_TRUE(cleared) << cleared.error().message;
    EXPECT_FALSE(cleared.value().canvas_present);
    EXPECT_FALSE(cleared.value().canvas_enabled);

    frame.aspect = std::numeric_limits<double>::denorm_min();
    auto underflowed_aspect = frame_to_parameters(frame);
    ASSERT_FALSE(underflowed_aspect);
    EXPECT_EQ(underflowed_aspect.error().context.at("parameter"), "aspect");
    constexpr auto oversized = static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 3) + 1U;
    auto oversized_canvas = compute_canvas_layout(oversized, 1U, CanvasParams{});
    ASSERT_FALSE(oversized_canvas);
    EXPECT_EQ(oversized_canvas.error().context.at("reason"), "invalid_canvas_dimensions");
    auto oversized_frame = compute_frame_layout(oversized, 1U, FrameParams{});
    ASSERT_FALSE(oversized_frame);
    EXPECT_EQ(oversized_frame.error().context.at("reason"), "invalid_frame_dimensions");
}

TEST(CanvasFrameTest, CanvasPixelsAndMaskAttachedFrameMatchFrozenPlacement)
{
    CanvasParams params{25.0, 50.0, 50.0, 100.0, CanvasColor::kBlue};
    auto layout = compute_canvas_layout(4U, 2U, params);
    ASSERT_TRUE(layout) << layout.error().message;
    EXPECT_EQ(layout.value().output_width, 7U);
    EXPECT_EQ(layout.value().output_height, 5U);
    EXPECT_EQ(layout.value().image_x, 1U);
    EXPECT_EQ(layout.value().image_y, 1U);
    auto expanded = apply_canvas(working_image(4U, 2U, {1.0F, 0.0F, 0.0F}), params, {});
    ASSERT_TRUE(expanded) << expanded.error().message;
    ASSERT_TRUE(expanded.value().mask_attached_frame);
    EXPECT_EQ(*expanded.value().mask_attached_frame, (AttachedPixelFrame{1U, 1U, 4U, 2U}));
    EXPECT_EQ(pixel(expanded.value().rgb, 7U, 0U, 0U), (std::array<float, 3>{0.0F, 0.0F, 1.0F}));
    EXPECT_EQ(pixel(expanded.value().rgb, 7U, 1U, 1U), (std::array<float, 3>{1.0F, 0.0F, 0.0F}));
    EXPECT_EQ(pixel(expanded.value().rgb, 7U, 5U, 2U), (std::array<float, 3>{0.0F, 0.0F, 1.0F}));

    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    const std::uint32_t stride = expanded.value().width * 3U;
    MaskEvaluationRequest request{.full_width = expanded.value().width,
                                  .full_height = expanded.value().height,
                                  .roi_width = expanded.value().width,
                                  .roi_height = expanded.value().height,
                                  .input = MaskRgbPlaneView{expanded.value().rgb, stride},
                                  .operation_output = std::nullopt,
                                  .attached_frame = expanded.value().mask_attached_frame,
                                  .cancellation = {}};
    auto alpha = evaluate_canonical_mask({all}, "all", request);
    ASSERT_TRUE(alpha) << alpha.error().message;
    for (std::uint32_t y = 0U; y < alpha.value().height; ++y)
    {
        for (std::uint32_t x = 0U; x < alpha.value().width; ++x)
        {
            const bool inside = x >= 1U && x < 5U && y >= 1U && y < 3U;
            EXPECT_EQ(alpha.value().alpha[static_cast<std::size_t>(y) * alpha.value().width + x],
                      inside ? 1.0F : 0.0F);
        }
    }
    Mask circle{"circle", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    circle.payload = CircleMask{0.5, 0.5, 0.4, 0.0};
    auto circle_alpha = evaluate_canonical_mask({circle}, "circle", request);
    ASSERT_TRUE(circle_alpha) << circle_alpha.error().message;
    EXPECT_EQ(circle_alpha.value().alpha[static_cast<std::size_t>(1U) * 7U + 2U], 1.0F);
    EXPECT_EQ(circle_alpha.value().alpha[static_cast<std::size_t>(1U) * 7U], 0.0F);
}

TEST(CanvasFrameTest, PerspectiveAndCropTransformCanvasAttachedOverlayWithPixels)
{
    DevelopParams develop;
    develop.canvas_present = true;
    develop.canvas_enabled = true;
    develop.canvas = CanvasParams{30.0, 30.0, 30.0, 30.0, CanvasColor::kBlue};
    develop.straighten_degrees = 4.0;
    develop.perspective_vertical = 0.25;
    develop.perspective_horizontal = -0.15;
    develop.perspective_shear = 0.08;
    develop.perspective_constrain_crop = false;
    develop.perspective_interpolation_index = 0;
    develop.crop_x = 0.05;
    develop.crop_y = 0.05;
    develop.crop_width = 0.9;
    develop.crop_height = 0.9;
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    Mask all{"photo", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    recipe.value().masks.push_back(std::move(all));

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto rendered = engine.value().render_linear_working(
        working_image(48U, 32U, {1.0F, 0.0F, 0.0F}), recipe.value(), {}, "photo");
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().mask_alpha.size(),
              static_cast<std::size_t>(rendered.value().width) * rendered.value().height);

    std::size_t photo_pixels = 0U;
    std::size_t background_pixels = 0U;
    for (std::size_t index = 0U; index < rendered.value().mask_alpha.size(); ++index)
    {
        const float alpha = rendered.value().mask_alpha[index];
        ASSERT_GE(alpha, 0.0F);
        ASSERT_LE(alpha, 1.0F);
        const auto red = rendered.value().rgb[index * 3U];
        const auto blue = rendered.value().rgb[index * 3U + 2U];
        if (alpha >= 0.9F)
        {
            ++photo_pixels;
            EXPECT_GT(red, blue);
        }
        else if (alpha <= 0.1F)
        {
            ++background_pixels;
            EXPECT_LE(red, blue);
        }
    }
    EXPECT_GT(photo_pixels, 100U);
    EXPECT_GT(background_pixels, 100U);
}

TEST(CanvasFrameTest, ConstantFrameCopiesImageAndDrawsLineWithFrozenIntegerLayout)
{
    ProfiledOutputBuffer input;
    input.width = 2U;
    input.height = 2U;
    input.channels = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F};
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "encoded-output";
    FrameParams params;
    params.size = 0.5;
    params.border_color = {1.0, 1.0, 1.0};
    auto layout = compute_frame_layout(2U, 2U, params);
    ASSERT_TRUE(layout) << layout.error().message;
    EXPECT_EQ(layout.value(), (FrameLayout{4U, 4U, 1U, 1U}));
    auto framed = apply_frame(input, params, {});
    ASSERT_TRUE(framed) << framed.error().message;
    EXPECT_EQ(pixel(framed.value().channels, 4U, 0U, 0U), (std::array<float, 3>{1.0F, 1.0F, 1.0F}));
    EXPECT_EQ(pixel(framed.value().channels, 4U, 1U, 1U), (std::array<float, 3>{1.0F, 0.0F, 0.0F}));
    EXPECT_EQ(pixel(framed.value().channels, 4U, 2U, 2U), (std::array<float, 3>{1.0F, 1.0F, 0.0F}));

    params.frame_size = 1.0;
    params.frame_offset = 0.0;
    params.frame_color = {0.0, 0.0, 0.0};
    auto lined = apply_frame(input, params, {});
    ASSERT_TRUE(lined) << lined.error().message;
    constexpr std::array<float, 3> black{0.0F, 0.0F, 0.0F};
    constexpr std::array<float, 3> white{1.0F, 1.0F, 1.0F};
    constexpr std::array<float, 3> red{1.0F, 0.0F, 0.0F};
    constexpr std::array<std::array<float, 3>, 16> expected{
        black, black, black, white, black, red,   black, white,
        black, black, black, white, white, white, white, white,
    };
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        EXPECT_EQ(pixel(lined.value().channels, lined.value().width,
                        static_cast<std::uint32_t>(index % lined.value().width),
                        static_cast<std::uint32_t>(index / lined.value().width)),
                  expected[index]);
    }
}

TEST(CanvasFrameTest, EngineDrawsFrameAfterDither)
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
    develop.frame.border_color = {0.25, 0.25, 0.25};
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe);
    auto working = working_image(2U, 2U, {0.25F, 0.25F, 0.25F});
    auto rendered = engine.value().render_linear_working_export(working, recipe.value(),
                                                                RenderSampleKind::kRgbFloat, {});
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 4U);
    EXPECT_EQ(rendered.value().height, 4U);
    const auto &samples = std::get<std::vector<float>>(rendered.value().samples);
    EXPECT_EQ(pixel(samples, 4U, 0U, 0U), (std::array<float, 3>{0.25F, 0.25F, 0.25F}));
    EXPECT_EQ(pixel(samples, 4U, 1U, 1U), (std::array<float, 3>{1.0F, 1.0F, 1.0F}));
}

struct CancelGeometry
{
    CancellationSource *source = nullptr;
    detail::CanvasFrameCheckpoint target = detail::CanvasFrameCheckpoint::kBeforePublication;
};

void cancel_geometry(void *context, const detail::CanvasFrameCheckpoint checkpoint,
                     const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<CancelGeometry *>(context);
    if (checkpoint == state.target &&
        (checkpoint == detail::CanvasFrameCheckpoint::kBeforePublication || progress == 1U))
        static_cast<void>(state.source->cancel("canvas-frame-checkpoint"));
}

TEST(CanvasFrameTest, MidRowAndPrePublicationCancellationKeepInputsImmutable)
{
    const auto canvas_input = working_image(8U, 4U, {0.2F, 0.4F, 0.6F});
    CancellationSource canvas_cancel;
    CancelGeometry canvas_state{&canvas_cancel, detail::CanvasFrameCheckpoint::kCanvasRow};
    auto canvas =
        detail::apply_canvas_controlled(canvas_input, {25.0, 25.0, 25.0, 25.0, CanvasColor::kBlack},
                                        canvas_cancel.token(), {&canvas_state, cancel_geometry});
    ASSERT_FALSE(canvas);
    EXPECT_EQ(canvas.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(canvas_input.rgb, working_image(8U, 4U, {0.2F, 0.4F, 0.6F}).rgb);

    ProfiledOutputBuffer frame_input;
    frame_input.width = 8U;
    frame_input.height = 4U;
    frame_input.channels.assign(8U * 4U * 3U, 0.5F);
    frame_input.color_profile.model = ColorModel::kRgb;
    const auto original = frame_input.channels;
    CancellationSource frame_cancel;
    CancelGeometry frame_state{&frame_cancel, detail::CanvasFrameCheckpoint::kBeforePublication};
    FrameParams params;
    params.size = 0.25;
    auto frame = detail::apply_frame_controlled(frame_input, params, frame_cancel.token(),
                                                {&frame_state, cancel_geometry});
    ASSERT_FALSE(frame);
    EXPECT_EQ(frame.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(frame_input.channels, original);
}

TEST(CanvasFrameTest, NonfiniteInputFailsBeforePublicationAndPreservesSource)
{
    auto canvas_input = working_image(2U, 2U, {0.2F, 0.4F, 0.6F});
    canvas_input.rgb[5U] = std::numeric_limits<float>::quiet_NaN();
    const auto original_canvas = canvas_input.rgb;
    auto canvas = apply_canvas(canvas_input, {10.0, 0.0, 0.0, 0.0, CanvasColor::kWhite}, {});
    ASSERT_FALSE(canvas);
    EXPECT_EQ(canvas.error().code, ErrorCode::kValidation);
    EXPECT_EQ(canvas.error().context.at("reason"), "nonfinite_canvas_input");
    EXPECT_EQ(canvas.error().context.at("sample_index"), "5");
    EXPECT_TRUE(std::isnan(canvas_input.rgb[5U]));
    EXPECT_EQ(canvas_input.rgb.size(), original_canvas.size());

    ProfiledOutputBuffer frame_input;
    frame_input.width = 2U;
    frame_input.height = 2U;
    frame_input.channels.assign(12U, 0.5F);
    frame_input.channels[7U] = std::numeric_limits<float>::infinity();
    frame_input.color_profile.model = ColorModel::kRgb;
    const auto original_frame = frame_input.channels;
    auto frame = apply_frame(frame_input, FrameParams{}, {});
    ASSERT_FALSE(frame);
    EXPECT_EQ(frame.error().code, ErrorCode::kValidation);
    EXPECT_EQ(frame.error().context.at("reason"), "nonfinite_frame_input");
    EXPECT_EQ(frame.error().context.at("sample_index"), "7");
    EXPECT_EQ(frame_input.channels, original_frame);
}

TEST(CanvasFrameLegacyXmpTest, FrozenCanvasAndThreeFrameRecordsMapStrictly)
{
    constexpr std::string_view default_blend = "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
    constexpr std::string_view canvas_blend =
        "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
    constexpr std::string_view frame_blend = "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
    constexpr std::string_view canvas = "0000a04000002041010070410000a04102000000";
    constexpr std::string_view frame_v3 =
        "gz02eJxjYGiwZ0Dg/cn5ecUliXklCkn5RSmpRQwwwHV9sQ2QsjfUN2JYzcLAEMXGwMDNChSZwsQAE2/wvS8GUz9z5k77s2fO2ILkGJAAIxADAKSCFWo=";
    constexpr std::string_view frame_v4_100 =
        "gz03eJzbucPC7vAhJ/u/f/7YMTAssGfAAhiB+OyZM7YMDA1Y5XGLgwGKHCOUBgC7mgxh";
    constexpr std::string_view frame_v4_line =
        "gz02eJx7+OCB7eNHj+xu3oiwt+x7bM+ABTAB8dkzZ2y5ritjlWdgUMAqfvaMjy0DQ4P9wQMO9g+B9ixcoGDHCJUDAFchF5o=";
    auto imported_canvas =
        import_legacy_xmp({geometry_xmp("enlargecanvas", "1", canvas, "13", canvas_blend),
                           {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported_canvas) << imported_canvas.error().message;
    const auto canvas_operation = std::find_if(
        imported_canvas.value().operations.begin(), imported_canvas.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == kCanvasOperationId; });
    ASSERT_NE(canvas_operation, imported_canvas.value().operations.end());
    auto canvas_params = canvas_from_parameters(canvas_operation->parameters);
    ASSERT_TRUE(canvas_params);
    EXPECT_EQ(canvas_params.value(),
              (CanvasParams{5.0, 10.0, 15.000000953674316, 20.0, CanvasColor::kBlue}));

    const std::array frame_cases{
        std::tuple{std::string_view("3"), frame_v3, std::string_view("9"), default_blend, -1.0,
                   FrameOrientation::kAuto},
        std::tuple{std::string_view("4"), frame_v4_100, std::string_view("13"), frame_blend, 1.25,
                   FrameOrientation::kPortrait},
        std::tuple{std::string_view("4"), frame_v4_line, std::string_view("13"), frame_blend,
                   1.7777777910232544, FrameOrientation::kLandscape},
    };
    for (const auto &[version, parameters, blend_version, blend, aspect, orientation] : frame_cases)
    {
        auto imported =
            import_legacy_xmp({geometry_xmp("borders", version, parameters, blend_version, blend),
                               {"asset", "file:///fixture.raw", std::nullopt}});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.back().id, kFrameOperationId);
        auto parsed = frame_from_parameters(imported.value().operations.back().parameters);
        ASSERT_TRUE(parsed);
        EXPECT_DOUBLE_EQ(parsed.value().aspect, aspect);
        EXPECT_EQ(parsed.value().orientation, orientation);
    }

    std::string modified(canvas);
    modified.replace(0, 2, "01");
    auto rejected =
        import_legacy_xmp({geometry_xmp("enlargecanvas", "1", modified, "13", canvas_blend),
                           {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_canvas_parameters");
}

} // namespace
} // namespace ravo
