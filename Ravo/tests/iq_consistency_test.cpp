#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/engine/iq_consistency.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/mask.h"
#include "ravo/recipe/sharpen.h"

namespace ravo
{
namespace
{

[[nodiscard]] LinearWorkingBuffer make_working(const std::uint32_t width,
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
        input.rgb[index] = static_cast<float>((index % 17U) + 3U) / 32.0F;
    return input;
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

[[nodiscard]] OperationInstance output_operation()
{
    return {"ravo.color.output", 1, "output", true, output_color_to_parameters(OutputColorParams{}),
            std::nullopt};
}

[[nodiscard]] Recipe make_sigmoid_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Recipe make_exposure_sigmoid_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    ExposureParams exposure;
    exposure.exposure_ev = 0.75;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Recipe make_light_sigmoid_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
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
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Result<Recipe> make_sharpen_sigmoid_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    SharpenParams sharpen;
    sharpen.radius = 1.5;
    sharpen.amount = 0.6;
    sharpen.threshold = 0.25;
    auto params = sharpen_to_parameters(sharpen);
    if (!params)
        return params.error();
    recipe.operations.push_back({std::string(kSharpenOperationId), kSharpenOperationSchemaVersion,
                                 "sharpen-1", true, params.value(), std::nullopt});
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Result<Recipe> make_admitted_develop_stack_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    ExposureParams exposure;
    exposure.exposure_ev = 0.5;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
    recipe.operations.push_back({"ravo.core.shadows",
                                 1,
                                 "shadows-1",
                                 true,
                                 {{"amount", ParameterValue{0.18}}},
                                 std::nullopt});
    SharpenParams sharpen;
    sharpen.radius = 1.0;
    sharpen.amount = 0.45;
    sharpen.threshold = 0.2;
    auto params = sharpen_to_parameters(sharpen);
    if (!params)
        return params.error();
    recipe.operations.push_back({std::string(kSharpenOperationId), kSharpenOperationSchemaVersion,
                                 "sharpen-1", true, params.value(), std::nullopt});
    recipe.operations.push_back(default_sigmoid_operation());
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Recipe make_contrast_only_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-consistency", "memory:iq-consistency", std::nullopt};
    recipe.operations.push_back({"ravo.core.contrast",
                                 1,
                                 "contrast-1",
                                 true,
                                 {{"amount", ParameterValue{0.4}}},
                                 std::nullopt});
    recipe.operations.push_back(output_operation());
    return recipe;
}

[[nodiscard]] Recipe make_masked_exposure_sigmoid_recipe()
{
    Recipe recipe = make_exposure_sigmoid_recipe();
    Mask all{"all", kCanonicalMaskSchemaVersion, MaskKind::kAll};
    all.payload = AllMask{};
    recipe.masks.push_back(std::move(all));
    recipe.operations.front().mask_id = std::string("all");
    return recipe;
}

[[nodiscard]] const std::vector<std::uint8_t> *
export_rgb8(const RenderedExportImage &image) noexcept
{
    return std::get_if<std::vector<std::uint8_t>>(&image.samples);
}

void expect_interactive_packed_within_contract(const EngineFacade &engine,
                                               const LinearWorkingBuffer &working,
                                               const Recipe &recipe)
{
    const auto cpu = engine.render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(cpu.value().gpu_backend, "cpu_reference"));

    InteractivePreviewRenderCache cache;
    const auto interactive = engine.render_interactive_linear_working(
        working, recipe, cache, CancellationToken{}, std::nullopt, true);
    ASSERT_TRUE(interactive) << interactive.error().message;
    ASSERT_EQ(interactive.value().rgb.size(), cpu.value().rgb.size());
    ASSERT_EQ(interactive.value().width, cpu.value().width);
    ASSERT_EQ(interactive.value().height, cpu.value().height);

    if (interactive.value().gpu_backend.empty())
    {
        EXPECT_TRUE(rgb8_buffers_equal(interactive.value().rgb, cpu.value().rgb));
    }
    else
    {
        EXPECT_EQ(interactive.value().gpu_backend, engine.gpu_backend());
        EXPECT_FALSE(is_cpu_gold_backend(interactive.value().gpu_backend));
        const int max_delta = max_packed_rgb8_abs_delta(interactive.value().rgb, cpu.value().rgb);
        EXPECT_GE(max_delta, 0);
        EXPECT_LE(max_delta, kIqGpuCpuPackedRgb8AbsDelta) << "max_packed_abs_delta=" << max_delta;
        EXPECT_TRUE(packed_rgb8_within_abs_delta(interactive.value().rgb, cpu.value().rgb,
                                                 kIqGpuCpuPackedRgb8AbsDelta));
    }

    const auto persist = engine.render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(persist.value().gpu_backend, "persist_after_gpu"));
    EXPECT_TRUE(rgb8_buffers_equal(persist.value().rgb, cpu.value().rgb));
}

} // namespace

TEST(IqConsistencyTest, PersistPreviewStaysOnCpuGoldAndIsBitExactOnRerender)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 6);
    const auto recipe = make_sigmoid_recipe();

    const auto first = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(first.value().gpu_backend, "persist_preview"));
    EXPECT_TRUE(is_cpu_gold_backend(first.value().gpu_backend));
    ASSERT_FALSE(first.value().rgb.empty());

    const auto second = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(second.value().gpu_backend, "persist_preview_reopen"));
    EXPECT_EQ(first.value().width, second.value().width);
    EXPECT_EQ(first.value().height, second.value().height);
    EXPECT_TRUE(rgb8_buffers_equal(first.value().rgb, second.value().rgb));
}

TEST(IqConsistencyTest, ExportStaysOnCpuGoldAndMatchesAcrossCalls)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 6);
    const auto recipe = make_sigmoid_recipe();

    const auto first = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    const auto *first_rgb = export_rgb8(first.value());
    ASSERT_NE(first_rgb, nullptr);
    ASSERT_FALSE(first_rgb->empty());

    const auto second = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    const auto *second_rgb = export_rgb8(second.value());
    ASSERT_NE(second_rgb, nullptr);
    EXPECT_EQ(first.value().width, second.value().width);
    EXPECT_EQ(first.value().height, second.value().height);
    EXPECT_EQ(*first_rgb, *second_rgb);
}

TEST(IqConsistencyTest, InteractiveGpuMatchesCpuWithinDocumentedToleranceWhenAvailable)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(4, 4);
    const auto recipe = make_sigmoid_recipe();

    const auto cpu = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(cpu.value().gpu_backend, "cpu_reference"));

    InteractivePreviewRenderCache cache;
    const auto interactive = engine.value().render_interactive_linear_working(
        working, recipe, cache, CancellationToken{}, std::nullopt, true);
    ASSERT_TRUE(interactive) << interactive.error().message;
    ASSERT_EQ(interactive.value().rgb.size(), cpu.value().rgb.size());

    if (!interactive.value().gpu_backend.empty())
    {
        EXPECT_EQ(interactive.value().gpu_backend, engine.value().gpu_backend());
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index < cpu.value().rgb.size(); ++index)
        {
            const int delta = static_cast<int>(interactive.value().rgb[index]) -
                              static_cast<int>(cpu.value().rgb[index]);
            if (std::abs(delta) > 1)
                ++mismatches;
        }
        EXPECT_LE(mismatches, cpu.value().rgb.size() / 8U);
    }
    else
    {
        EXPECT_TRUE(rgb8_buffers_equal(interactive.value().rgb, cpu.value().rgb));
    }
}

TEST(IqConsistencyTest, RequireCpuGoldRejectsNamedGpuBackend)
{
    auto ok = require_cpu_gold_backend({}, "persist");
    ASSERT_TRUE(ok);
    auto bad = require_cpu_gold_backend("metal", "persist");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().context.at("reason"), "iq_cpu_gold_backend_required");
}

TEST(IqConsistencyTest, PersistPreviewMatchesExportRgb8AndIccIdentity)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 6);
    const auto recipe = make_sigmoid_recipe();

    const auto preview = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(preview.value().gpu_backend, "persist_preview"));

    const auto exported = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(exported) << exported.error().message;
    const auto *export_rgb = export_rgb8(exported.value());
    ASSERT_NE(export_rgb, nullptr);
    EXPECT_EQ(preview.value().width, exported.value().width);
    EXPECT_EQ(preview.value().height, exported.value().height);
    EXPECT_TRUE(rgb8_buffers_equal(preview.value().rgb, *export_rgb));
    EXPECT_EQ(preview.value().color_profile.identifier, exported.value().color_profile.identifier);
    EXPECT_EQ(preview.value().color_profile.icc_bytes, exported.value().color_profile.icc_bytes);
    EXPECT_FALSE(exported.value().color_profile.identifier.empty());
}

TEST(IqConsistencyTest, OverlappingPackedRoiAgreesWithFullFrameCrop)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(16, 12);
    const auto recipe = make_sigmoid_recipe();

    const auto full = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(full) << full.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(full.value().gpu_backend, "full_frame_cpu_gold"));

    const std::uint32_t x = 4;
    const std::uint32_t y = 3;
    const std::uint32_t w = 6;
    const std::uint32_t h = 4;
    auto cropped =
        crop_packed_rgb8(full.value().rgb, full.value().width, full.value().height, x, y, w, h);
    ASSERT_TRUE(cropped) << cropped.error().message;

    const auto reopen = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(reopen) << reopen.error().message;
    auto cropped_reopen = crop_packed_rgb8(reopen.value().rgb, reopen.value().width,
                                           reopen.value().height, x, y, w, h);
    ASSERT_TRUE(cropped_reopen) << cropped_reopen.error().message;
    EXPECT_TRUE(rgb8_buffers_equal(cropped.value(), cropped_reopen.value()));

    auto exported = engine.value().render_linear_working_export(
        working, recipe, RenderSampleKind::kRgb8, CancellationToken{});
    ASSERT_TRUE(exported) << exported.error().message;
    const auto *export_rgb = export_rgb8(exported.value());
    ASSERT_NE(export_rgb, nullptr);
    auto cropped_export =
        crop_packed_rgb8(*export_rgb, exported.value().width, exported.value().height, x, y, w, h);
    ASSERT_TRUE(cropped_export) << cropped_export.error().message;
    EXPECT_TRUE(rgb8_buffers_equal(cropped.value(), cropped_export.value()));
}

TEST(IqConsistencyTest, InteractiveGpuResidualStaysWithinDocumentedPackedDelta)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(8, 8),
                                              make_sigmoid_recipe());
}

TEST(IqConsistencyTest, InteractiveGpuExposurePackedDeltaWithinContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(8, 8),
                                              make_exposure_sigmoid_recipe());
}

TEST(IqConsistencyTest, InteractiveGpuLightControlsPackedDeltaWithinContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(8, 8),
                                              make_light_sigmoid_recipe());
}

TEST(IqConsistencyTest, InteractiveGpuSharpenPackedDeltaWithinContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto recipe = make_sharpen_sigmoid_recipe();
    ASSERT_TRUE(recipe) << recipe.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(16, 16), recipe.value());
}

TEST(IqConsistencyTest, InteractiveGpuAdmittedDevelopStackPackedDeltaWithinContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto recipe = make_admitted_develop_stack_recipe();
    ASSERT_TRUE(recipe) << recipe.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(16, 16), recipe.value());
}

TEST(IqConsistencyTest, InteractiveGpuContrastPackedDeltaWithinContract)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    expect_interactive_packed_within_contract(engine.value(), make_working(8, 8),
                                              make_contrast_only_recipe());
}

TEST(IqConsistencyTest, InteractiveMaskedExposureStaysCpuGoldBitExact)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto working = make_working(8, 8);
    const auto recipe = make_masked_exposure_sigmoid_recipe();

    const auto cpu = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(cpu) << cpu.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(cpu.value().gpu_backend, "cpu_reference"));

    InteractivePreviewRenderCache cache;
    const auto interactive = engine.value().render_interactive_linear_working(
        working, recipe, cache, CancellationToken{}, std::nullopt, true);
    ASSERT_TRUE(interactive) << interactive.error().message;
    // Masked exposure is CPU on the interactive path; sigmoid after a CPU prefix
    // may still report GPU. Persist remains CPU gold either way.
    ASSERT_EQ(interactive.value().rgb.size(), cpu.value().rgb.size());
    if (interactive.value().gpu_backend.empty())
    {
        EXPECT_TRUE(rgb8_buffers_equal(interactive.value().rgb, cpu.value().rgb));
    }
    else
    {
        EXPECT_TRUE(packed_rgb8_within_abs_delta(interactive.value().rgb, cpu.value().rgb,
                                                 kIqGpuCpuPackedRgb8AbsDelta));
    }
    const auto persist = engine.value().render_linear_working(working, recipe, CancellationToken{});
    ASSERT_TRUE(persist) << persist.error().message;
    ASSERT_TRUE(require_cpu_gold_backend(persist.value().gpu_backend, "persist_after_masked"));
    EXPECT_TRUE(rgb8_buffers_equal(persist.value().rgb, cpu.value().rgb));
}

TEST(IqConsistencyTest, AdmittedInteractiveStagesAreDocumented)
{
    EXPECT_EQ(kIqConsistencySchemaVersion, 4);
    EXPECT_FALSE(std::string(kIqGpuInteractiveNonAdmittedPolicy).empty());
    EXPECT_NE(std::string(kIqConsistencyGpuLiveResidual).find("admitted_interactive"),
              std::string::npos);
    EXPECT_NE(std::string(kIqRawRoiVersusExportResidual).find("full_export_crop_size_matched"),
              std::string::npos);
    EXPECT_NE(std::string(kIqRawRoiVersusExportResidual).find("rcd_tile_aligned"),
              std::string::npos);
    EXPECT_NE(std::string(kIqRawRoiVersusExportResidual).find("gpu_native_roi_apron_owned_surface"),
              std::string::npos);
    EXPECT_NE(std::string(kIqRawRoiVersusExportResidual)
                  .find("scaled_export_same_scale_settled_preview_crop_packed_bit_exact"),
              std::string::npos);
    EXPECT_NE(std::string(kIqRawRoiVersusExportResidual)
                  .find("cross_scale_roi_vs_scaled_export_explicit_non_compare"),
              std::string::npos);
    bool saw_exposure = false;
    bool saw_contrast = false;
    bool saw_sigmoid = false;
    bool saw_sharpen = false;
    bool saw_rapidraw_controls = false;
    bool saw_rapidraw_display = false;
    for (const auto stage : kIqGpuAdmittedInteractiveStages)
    {
        if (stage == "ravo.core.exposure")
            saw_exposure = true;
        if (stage == "ravo.core.contrast")
            saw_contrast = true;
        if (stage == "ravo.display.sigmoid")
            saw_sigmoid = true;
        if (stage == "ravo.detail.sharpen")
            saw_sharpen = true;
        if (stage == "ravo.core.rapidraw-tone-controls")
            saw_rapidraw_controls = true;
        if (stage == "ravo.display.rapidraw-basic")
            saw_rapidraw_display = true;
    }
    EXPECT_TRUE(saw_exposure);
    EXPECT_TRUE(saw_contrast);
    EXPECT_TRUE(saw_sigmoid);
    EXPECT_TRUE(saw_sharpen);
    EXPECT_TRUE(saw_rapidraw_controls);
    EXPECT_TRUE(saw_rapidraw_display);
}

} // namespace ravo
