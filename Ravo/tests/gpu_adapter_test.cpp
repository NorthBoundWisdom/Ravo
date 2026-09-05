#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

#include "gpu_adapter.h"
#include "gpu_preview.h"
#include "image_ops.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/velvia.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/rapidraw_tone.h"
#include "ravo/recipe/rapidraw_tone_controls.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool gpu_available(const EngineFacade &engine) noexcept
{
    return engine.gpu_backend() != "unavailable";
}

[[nodiscard]] bool gpu_host_unavailable(const TaskError &error)
{
    const auto reason = error.context.find("reason");
    if (reason == error.context.end())
    {
        return false;
    }
    if (reason->second == "gpu_unavailable")
    {
        return error.code == ErrorCode::kUnsupported;
    }
    return reason->second == "gpu_pipeline_failed" && error.code == ErrorCode::kIo;
}

[[nodiscard]] LinearWorkingBuffer make_preview_working(const std::uint32_t width,
                                                       const std::uint32_t height)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input;
    input.width = width;
    input.height = height;
    input.color_profile = profile;
    input.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    input.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
    {
        input.rgb[index] = static_cast<float>((index % 23U) + 1U) / 32.0F;
    }
    return input;
}

[[nodiscard]] Result<RenderedImage> render_interactive(const EngineFacade &engine,
                                                       const LinearWorkingBuffer &working,
                                                       const Recipe &recipe)
{
    InteractivePreviewRenderCache cache;
    return engine.render_interactive_linear_working(working, recipe, cache, CancellationToken{});
}

TEST(EngineFacadeTest, GpuAdapterDoesNotFailCpuCreate)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    EXPECT_FALSE(engine.value().gpu_backend().empty());
    EXPECT_FALSE(engine.value().operations().empty());
}

TEST(EngineFacadeTest, GpuCopyRgbHonorsCancellation)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("gpu_test_cancel"));
    std::vector<float> input{1.0F, 2.0F, 3.0F};
    std::vector<float> output(3U, 0.0F);
    const auto copied = engine.value().gpu_copy_rgb(input, output, cancellation.token());
    ASSERT_FALSE(copied);
    EXPECT_EQ(copied.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, GpuCopyRgbMatchesHostAvailability)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    std::vector<float> input{0.0F, -1.5F, 2.25F, 1.0F};
    std::vector<float> output(input.size(), 99.0F);
    const auto copied = engine.value().gpu_copy_rgb(input, output, CancellationToken{});
    if (!gpu_available(engine.value()))
    {
        ASSERT_FALSE(copied);
        EXPECT_TRUE(gpu_host_unavailable(copied.error())) << copied.error().message;
        EXPECT_EQ(output[0], 99.0F);
        return;
    }
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(output, input);
}

TEST(EngineFacadeTest, GpuCopyRgbRejectsSizeMismatchWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    std::vector<float> input{1.0F, 2.0F};
    std::vector<float> output{0.0F};
    const auto copied = engine.value().gpu_copy_rgb(input, output, CancellationToken{});
    ASSERT_FALSE(copied);
    if (!gpu_available(engine.value()))
    {
        EXPECT_TRUE(gpu_host_unavailable(copied.error())) << copied.error().message;
        return;
    }
    EXPECT_EQ(copied.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(copied.error().context.at("reason"), "gpu_copy_size_mismatch");
}

TEST(EngineFacadeTest, GpuApplyExposureMatchesCpuGoldWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = "working-fixture";
    profile.has_matrix = true;
    profile.camera_input = true;
    profile.icc_bytes = {1U, 2U, 3U};
    const LinearWorkingBuffer input{2,  1, {-0.5F, 0.0F, 0.25F, 0.5F, 1.0F, 2.0F}, profile, {},
                                    {}, {}};
    ExposureParams params;
    params.black = -0.25;
    params.exposure_ev = 1.0;
    const auto gpu = engine.value().gpu_apply_exposure(input, params, CancellationToken{});
    if (!gpu_available(engine.value()))
    {
        ASSERT_FALSE(gpu);
        EXPECT_TRUE(gpu_host_unavailable(gpu.error())) << gpu.error().message;
        return;
    }
    const auto cpu = apply_exposure(input, params, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(gpu) << gpu.error().message;
    ASSERT_EQ(gpu.value().rgb.size(), cpu.value().rgb.size());
    EXPECT_EQ(gpu.value().width, input.width);
    EXPECT_EQ(gpu.value().height, input.height);
    EXPECT_EQ(gpu.value().color_profile, input.color_profile);
    EXPECT_NE(gpu.value().rgb.data(), input.rgb.data());
    for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(gpu.value().rgb[index], cpu.value().rgb[index], 1.0e-5) << index;
    }
    EXPECT_EQ(input.rgb[0], -0.5F);
}

TEST(EngineFacadeTest, GpuApplyExposureHonorsCancellation)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    LinearWorkingBuffer input{1, 1, {0.25F, 0.5F, 0.75F}, {}, {}, {}, {}};
    input.color_profile.kind = ColorProfileKind::kBuiltin;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = "linear-rec709";
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("gpu_exposure_cancel"));
    const auto gpu =
        engine.value().gpu_apply_exposure(input, ExposureParams{}, cancellation.token());
    ASSERT_FALSE(gpu);
    EXPECT_EQ(gpu.error().code, ErrorCode::kCancelled);
}

TEST(GpuAdapterTest, TryCreateReportsTheSameBackendAsTheFacade)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto created = GpuAdapter::try_create();
    if (!gpu_available(engine.value()))
    {
        ASSERT_FALSE(created);
        EXPECT_TRUE(gpu_host_unavailable(created.error())) << created.error().message;
        return;
    }
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value()->backend_id(), engine.value().gpu_backend());
    EXPECT_NE(created.value()->backend_id(), "unavailable");
    std::vector<float> input{4.0F, 5.0F};
    std::vector<float> output(2U, 0.0F);
    const auto copied = created.value()->copy_rgb(input, output, CancellationToken{});
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(output, input);
}

[[nodiscard]] OperationInstance default_sigmoid_operation()
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-1",
            true,
            {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
             {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
             {"middle_grey_contrast", ParameterValue{kSigmoidContrastDefault}},
             {"contrast_skewness", ParameterValue{kSigmoidSkewDefault}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{kSigmoidHuePreservationDefault}}},
            std::nullopt};
}

TEST(EngineFacadeTest, GpuPreviewRgbMatchesCpuSigmoidAndExposureGold)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    if (!gpu_available(engine.value()))
    {
        GTEST_SKIP() << "GPU adapter is unavailable";
    }
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    LinearWorkingBuffer input{
        2,
        2,
        {0.02F, 0.04F, 0.08F, 0.16F, 0.24F, 0.32F, 0.48F, 0.64F, 0.80F, 1.10F, 0.90F, 0.70F},
        profile,
        {},
        {},
        {}};
    Recipe recipe;
    ExposureParams exposure;
    exposure.exposure_ev = 0.75;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
    recipe.operations.push_back(default_sigmoid_operation());
    auto passes = gpu_preview_rgb_passes(input, recipe, CancellationToken{});
    ASSERT_TRUE(passes) << passes.error().message;
    ASSERT_TRUE(passes.value().has_value());
    ASSERT_EQ(passes.value()->size(), 2U);
    auto gpu = GpuAdapter::try_create();
    ASSERT_TRUE(gpu) << gpu.error().message;
    auto gpu_image =
        apply_gpu_preview_rgb(input, *passes.value(), *gpu.value(), CancellationToken{});
    ASSERT_TRUE(gpu_image) << gpu_image.error().message;
    auto cpu_image = apply_recipe_ops(input, recipe, CancellationToken{});
    ASSERT_TRUE(cpu_image) << cpu_image.error().message;
    ASSERT_EQ(gpu_image.value().rgb.size(), cpu_image.value().rgb.size());
    for (std::size_t index = 0; index < cpu_image.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(gpu_image.value().rgb[index], cpu_image.value().rgb[index], 2.0e-3) << index;
    }
}

TEST(EngineFacadeTest, GpuPreviewRenderReportsBackendForAdmissibleRecipe)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.18F, 0.18F, 0.18F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    ASSERT_EQ(persist.value().rgb.size(), 3U);
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().rgb.size(), 3U);
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesContrastOnlyRecipesOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.18F, 0.18F, 0.18F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    recipe.operations.push_back({"ravo.core.contrast",
                                 1,
                                 "contrast-1",
                                 true,
                                 {{"amount", ParameterValue{0.4}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesGammaOnlyRecipesOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.18F, 0.18F, 0.18F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    recipe.operations.push_back(
        {"ravo.core.gamma", 1, "gamma-1", true, {{"gamma", ParameterValue{1.25}}}, std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesVibranceSaturationOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.22F, 0.14F, 0.10F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    recipe.operations.push_back({"ravo.color.vibrance",
                                 1,
                                 "vibrance-1",
                                 true,
                                 {{"amount", ParameterValue{0.5}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.color.saturation",
                                 1,
                                 "saturation-1",
                                 true,
                                 {{"amount", ParameterValue{0.15}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesVelviaOnlyRecipesOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.20F, 0.12F, 0.08F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    VelviaParams params;
    params.strength = 35.0;
    params.bias = 0.8;
    auto encoded = velvia_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    recipe.operations.push_back({std::string(kVelviaOperationId), kVelviaOperationSchemaVersion,
                                 "velvia-1", true, encoded.value(), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesSplitToningOnlyRecipesOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input{1, 1, {0.16F, 0.14F, 0.22F}, profile, {}, {}, {}};
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    SplitToningParams params;
    params.shadow_hue = 0.6;
    params.shadow_saturation = 0.4;
    params.highlight_hue = 0.1;
    params.highlight_saturation = 0.35;
    params.balance = 0.5;
    params.compress = 30.0;
    params.mix = 0.9;
    auto encoded = split_toning_to_parameters(params);
    ASSERT_TRUE(encoded) << encoded.error().message;
    recipe.operations.push_back({std::string(kSplitToningOperationId),
                                 kSplitToningOperationSchemaVersion, "split-1", true,
                                 encoded.value(), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewAppliesContrastThenSigmoidOnGpuWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto input = make_preview_working(4, 4);
    Recipe recipe;
    recipe.asset = {"gpu-preview", "memory:gpu-preview", std::nullopt};
    recipe.operations.push_back({"ravo.core.contrast",
                                 1,
                                 "contrast-1",
                                 true,
                                 {{"amount", ParameterValue{0.4}}},
                                 std::nullopt});
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    auto gpu = GpuAdapter::try_create();
    std::string gpu_backend;
    const auto mixed = apply_preview_rgb(input, recipe, gpu ? gpu.value().get() : nullptr,
                                         &gpu_backend, CancellationToken{});
    ASSERT_TRUE(mixed) << mixed.error().message;
    const auto cpu = apply_recipe_ops(input, recipe, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_EQ(mixed.value().rgb.size(), cpu.value().rgb.size());
    for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(mixed.value().rgb[index], cpu.value().rgb[index], 2.0e-3) << index;
    }
    const auto persist = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe);
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_FALSE(gpu_backend.empty());
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
        EXPECT_TRUE(gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewDefaultRawBaselineReportsBackend)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto input = make_preview_working(32, 32);
    auto develop = develop_raw_import_baseline();
    develop.raw_highlights = 0.0;
    auto recipe = recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    EXPECT_NE(std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == kRapidRawBasicToneOperationId; }),
              recipe.value().operations.end());
    EXPECT_NE(std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == kRapidRawToneControlsOperationId; }),
              recipe.value().operations.end());
    auto gpu = GpuAdapter::try_create();
    std::string gpu_backend;
    const auto mixed = apply_preview_rgb(input, recipe.value(), gpu ? gpu.value().get() : nullptr,
                                         &gpu_backend, CancellationToken{});
    ASSERT_TRUE(mixed) << mixed.error().message;
    const auto cpu = apply_recipe_ops(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_EQ(mixed.value().rgb.size(), cpu.value().rgb.size());
    for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(mixed.value().rgb[index], cpu.value().rgb[index], 2.0e-3) << index;
    }
    const auto persist =
        engine.value().render_linear_working(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    EXPECT_TRUE(persist.value().gpu_backend.empty());
    const auto rendered = render_interactive(engine.value(), input, recipe.value());
    ASSERT_TRUE(rendered) << rendered.error().message;
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(rendered.value().gpu_backend, engine.value().gpu_backend());
        EXPECT_NE(rendered.value().gpu_backend, "unavailable");
        EXPECT_EQ(gpu_backend, engine.value().gpu_backend());
    }
    else
    {
        EXPECT_TRUE(rendered.value().gpu_backend.empty());
        EXPECT_TRUE(gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuPreviewLightControlsMatchCpuGoldWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    if (!gpu_available(engine.value()))
    {
        GTEST_SKIP() << "GPU adapter is unavailable";
    }
    const auto input = make_preview_working(8, 8);
    Recipe recipe;
    recipe.operations.push_back({"ravo.core.shadows",
                                 1,
                                 "shadows-1",
                                 true,
                                 {{"amount", ParameterValue{0.214}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.core.highlights",
                                 1,
                                 "highlights-1",
                                 true,
                                 {{"amount", ParameterValue{-0.35}}},
                                 std::nullopt});
    auto passes = gpu_preview_rgb_passes(input, recipe, CancellationToken{});
    ASSERT_TRUE(passes) << passes.error().message;
    ASSERT_TRUE(passes.value().has_value());
    ASSERT_EQ(passes.value()->size(), 2U);
    EXPECT_EQ(passes.value()->at(0).kind, GpuRgbPass::Kind::kLightControls);
    EXPECT_EQ(passes.value()->at(1).kind, GpuRgbPass::Kind::kLightControls);
    auto gpu = GpuAdapter::try_create();
    ASSERT_TRUE(gpu) << gpu.error().message;
    auto gpu_image =
        apply_gpu_preview_rgb(input, *passes.value(), *gpu.value(), CancellationToken{});
    ASSERT_TRUE(gpu_image) << gpu_image.error().message;
    auto cpu_image = apply_recipe_ops(input, recipe, CancellationToken{});
    ASSERT_TRUE(cpu_image) << cpu_image.error().message;
    ASSERT_EQ(gpu_image.value().rgb.size(), cpu_image.value().rgb.size());
    for (std::size_t index = 0; index < cpu_image.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(gpu_image.value().rgb[index], cpu_image.value().rgb[index], 2.0e-3) << index;
    }
}

TEST(EngineFacadeTest, GpuPreviewSharpenMatchesCpuGoldWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    if (!gpu_available(engine.value()))
    {
        GTEST_SKIP() << "GPU adapter is unavailable";
    }
    const auto input = make_preview_working(32, 32);
    auto develop = develop_raw_import_baseline();
    develop.raw_highlights = 0.0;
    auto recipe = recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto passes = gpu_preview_rgb_passes(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(passes) << passes.error().message;
    ASSERT_TRUE(passes.value().has_value());
    bool has_sharpen = false;
    bool has_rapidraw_basic_tone = false;
    for (const auto &pass : *passes.value())
    {
        has_sharpen = has_sharpen || pass.kind == GpuRgbPass::Kind::kSharpen;
        has_rapidraw_basic_tone =
            has_rapidraw_basic_tone || pass.kind == GpuRgbPass::Kind::kRapidRawBasicTone;
    }
    EXPECT_TRUE(has_sharpen);
    EXPECT_TRUE(has_rapidraw_basic_tone);
    auto gpu = GpuAdapter::try_create();
    ASSERT_TRUE(gpu) << gpu.error().message;
    auto gpu_image =
        apply_gpu_preview_rgb(input, *passes.value(), *gpu.value(), CancellationToken{});
    ASSERT_TRUE(gpu_image) << gpu_image.error().message;
    auto cpu_image = apply_recipe_ops(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(cpu_image) << cpu_image.error().message;
    ASSERT_EQ(gpu_image.value().rgb.size(), cpu_image.value().rgb.size());
    for (std::size_t index = 0; index < cpu_image.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(gpu_image.value().rgb[index], cpu_image.value().rgb[index], 2.0e-3) << index;
    }
}

TEST(EngineFacadeTest, GpuPreviewRgbStackKeepsShadowsSharpenAndSigmoidOnGpu)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto input = make_preview_working(32, 32);
    DevelopParams develop = develop_raw_import_baseline();
    develop.raw_highlights = 0.0;
    develop.rapidraw_ev_shift = 0.32;
    develop.rapidraw_exposure = 0.47;
    develop.rapidraw_contrast = 23.0;
    develop.rapidraw_highlights = -31.0;
    develop.rapidraw_shadows = 28.0;
    develop.rapidraw_whites = 12.0;
    develop.rapidraw_blacks = -9.0;
    auto recipe = recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto gpu = GpuAdapter::try_create();
    std::string gpu_backend;
    const auto mixed = apply_preview_rgb(input, recipe.value(), gpu ? gpu.value().get() : nullptr,
                                         &gpu_backend, CancellationToken{});
    ASSERT_TRUE(mixed) << mixed.error().message;
    const auto cpu = apply_recipe_ops(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_EQ(mixed.value().rgb.size(), cpu.value().rgb.size());
    for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
    {
        EXPECT_NEAR(mixed.value().rgb[index], cpu.value().rgb[index], 2.0e-3) << index;
    }
    if (gpu_available(engine.value()))
    {
        EXPECT_EQ(gpu_backend, engine.value().gpu_backend());
        auto passes = gpu_preview_rgb_passes(input, recipe.value(), CancellationToken{});
        ASSERT_TRUE(passes) << passes.error().message;
        ASSERT_TRUE(passes.value().has_value());
        ASSERT_GE(passes.value()->size(), 3U);
        EXPECT_EQ(passes.value()->at(0).kind, GpuRgbPass::Kind::kSharpen);
        EXPECT_EQ(passes.value()->at(1).kind, GpuRgbPass::Kind::kRapidRawToneControls);
        EXPECT_EQ(passes.value()->back().kind, GpuRgbPass::Kind::kRapidRawBasicTone);
    }
    else
    {
        EXPECT_TRUE(gpu_backend.empty());
    }
}

TEST(EngineFacadeTest, GpuRetainedSourceMatchesUploadedPassesWhenAvailable)
{
    auto gpu = GpuAdapter::try_create();
    if (!gpu)
    {
        GTEST_SKIP() << "GPU adapter is unavailable";
    }
    const auto input = make_preview_working(8, 8);
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kAffine;
    pass.affine.scale = 1.25F;
    pass.affine.black = 0.01F;
    std::vector<float> uploaded(input.rgb.size(), 0.0F);
    std::vector<float> retained(input.rgb.size(), 0.0F);
    GpuRgbApplyOptions upload_options;
    upload_options.download = true;
    upload_options.width = input.width;
    upload_options.height = input.height;
    auto first =
        gpu.value()->apply_rgb_passes(input.rgb, uploaded, std::span<const GpuRgbPass>(&pass, 1U),
                                      upload_options, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    auto stored = gpu.value()->retain_source_rgb(input.rgb, input.width, input.height,
                                                 CancellationToken{}, "test-retained");
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_TRUE(gpu.value()->has_retained_source(input.width, input.height));
    EXPECT_EQ(gpu.value()->retained_source_key(), "test-retained");
    GpuRgbApplyOptions retained_options;
    retained_options.from_retained_source = true;
    retained_options.download = true;
    retained_options.width = input.width;
    retained_options.height = input.height;
    auto second =
        gpu.value()->apply_rgb_passes(input.rgb, retained, std::span<const GpuRgbPass>(&pass, 1U),
                                      retained_options, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(uploaded.size(), retained.size());
    for (std::size_t index = 0; index < uploaded.size(); ++index)
    {
        EXPECT_NEAR(uploaded[index], retained[index], 2.0e-3F) << index;
    }
}

TEST(EngineFacadeTest, GpuGrowOnlySmallerUploadDoesNotOverreadWhenAvailable)
{
    auto gpu = GpuAdapter::try_create();
    if (!gpu)
    {
        GTEST_SKIP() << "GPU adapter is unavailable";
    }
    const auto large = make_preview_working(64, 64);
    const auto small = make_preview_working(8, 8);
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kAffine;
    pass.affine.scale = 1.5F;
    pass.affine.black = 0.0F;
    std::vector<float> large_out(large.rgb.size(), 0.0F);
    std::vector<float> small_out(small.rgb.size(), 0.0F);
    GpuRgbApplyOptions large_options;
    large_options.download = true;
    large_options.width = large.width;
    large_options.height = large.height;
    auto first =
        gpu.value()->apply_rgb_passes(large.rgb, large_out, std::span<const GpuRgbPass>(&pass, 1U),
                                      large_options, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    GpuRgbApplyOptions small_options;
    small_options.download = true;
    small_options.width = small.width;
    small_options.height = small.height;
    auto second =
        gpu.value()->apply_rgb_passes(small.rgb, small_out, std::span<const GpuRgbPass>(&pass, 1U),
                                      small_options, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(small_out.size(), small.rgb.size());
    for (std::size_t index = 0; index < small.rgb.size(); ++index)
    {
        EXPECT_NEAR(small_out[index], small.rgb[index] * 1.5F, 2.0e-3F) << index;
    }
}

TEST(EngineFacadeTest, GpuInteractiveSkipDownloadPublishesDisplayWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto input = make_preview_working(8, 8);
    Recipe recipe;
    recipe.asset = {"gpu-display", "memory:gpu-display", std::nullopt};
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    InteractivePreviewRenderCache cache;
    const auto rendered = engine.value().render_interactive_linear_working(
        input, recipe, cache, CancellationToken{}, std::nullopt, false);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 8U);
    EXPECT_EQ(rendered.value().height, 8U);
    if (engine.value().gpu_backend() == "metal")
    {
        EXPECT_TRUE(rendered.value().rgb.empty());
        EXPECT_GT(rendered.value().gpu_display_generation, 0U);
        const auto frame = engine.value().gpu_display_frame();
        EXPECT_EQ(frame.generation, rendered.value().gpu_display_generation);
        EXPECT_NE(frame.native_surface, 0U);
        EXPECT_EQ(frame.width, 8U);
        EXPECT_EQ(frame.height, 8U);

        const auto odd = make_preview_working(5, 4);
        InteractivePreviewRenderCache odd_cache;
        const auto odd_rendered = engine.value().render_interactive_linear_working(
            odd, recipe, odd_cache, CancellationToken{}, std::nullopt, false);
        ASSERT_TRUE(odd_rendered) << odd_rendered.error().message;
        EXPECT_TRUE(odd_rendered.value().rgb.empty());
        EXPECT_EQ(odd_rendered.value().width, 5U);
        EXPECT_EQ(odd_rendered.value().height, 4U);
        const auto odd_frame = engine.value().gpu_display_frame();
        EXPECT_EQ(odd_frame.width, 5U);
        EXPECT_EQ(odd_frame.height, 4U);
        EXPECT_NE(odd_frame.native_surface, 0U);
    }
    else
    {
        EXPECT_FALSE(rendered.value().rgb.empty());
        EXPECT_EQ(rendered.value().gpu_display_generation, 0U);
    }
}

} // namespace
} // namespace ravo
