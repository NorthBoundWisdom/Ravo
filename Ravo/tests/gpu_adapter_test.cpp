#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gpu_adapter.h"
#include "gpu_preview.h"
#include "image_ops.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool gpu_available(const EngineFacade &engine) noexcept
{
    return engine.gpu_backend() != "unavailable";
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
        EXPECT_EQ(copied.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(copied.error().context.at("reason"), "gpu_unavailable");
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
        EXPECT_EQ(copied.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(copied.error().context.at("reason"), "gpu_unavailable");
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
        EXPECT_EQ(gpu.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(gpu.error().context.at("reason"), "gpu_unavailable");
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
        EXPECT_EQ(created.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(created.error().context.at("reason"), "gpu_unavailable");
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
    const auto rendered = engine.value().render_linear_working(input, recipe, CancellationToken{});
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

TEST(EngineFacadeTest, GpuPreviewLeavesContrastOnlyRecipesOnCpu)
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
    const auto rendered = engine.value().render_linear_working(input, recipe, CancellationToken{});
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_TRUE(rendered.value().gpu_backend.empty());
}

TEST(EngineFacadeTest, GpuPreviewAppliesSigmoidOnGpuAfterCpuOps)
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
    const auto rendered = engine.value().render_linear_working(input, recipe, CancellationToken{});
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
    auto recipe = recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt},
                                      develop_raw_import_baseline());
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
    const auto rendered =
        engine.value().render_linear_working(input, recipe.value(), CancellationToken{});
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
    auto recipe = recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt},
                                      develop_raw_import_baseline());
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto passes = gpu_preview_rgb_passes(input, recipe.value(), CancellationToken{});
    ASSERT_TRUE(passes) << passes.error().message;
    ASSERT_TRUE(passes.value().has_value());
    bool has_sharpen = false;
    bool has_sigmoid = false;
    for (const auto &pass : *passes.value())
    {
        has_sharpen = has_sharpen || pass.kind == GpuRgbPass::Kind::kSharpen;
        has_sigmoid = has_sigmoid || pass.kind == GpuRgbPass::Kind::kSigmoid;
    }
    EXPECT_TRUE(has_sharpen);
    EXPECT_TRUE(has_sigmoid);
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
    develop.exposure_ev = 0.847;
    develop.shadows = 0.214;
    auto recipe =
        recipe_from_develop({"gpu-preview", "memory:gpu-preview", std::nullopt}, develop);
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
        EXPECT_EQ(passes.value()->at(0).kind, GpuRgbPass::Kind::kAffine);
        EXPECT_EQ(passes.value()->at(1).kind, GpuRgbPass::Kind::kLightControls);
        EXPECT_EQ(passes.value()->at(2).kind, GpuRgbPass::Kind::kSharpen);
    }
    else
    {
        EXPECT_TRUE(gpu_backend.empty());
    }
}

} // namespace
} // namespace ravo
