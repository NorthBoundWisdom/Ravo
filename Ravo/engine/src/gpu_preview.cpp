#include "gpu_preview.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "image_ops.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/rapidraw_tone.h"
#include "ravo/recipe/rapidraw_tone_controls.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/split_toning.h"
#include "ravo/recipe/velvia.h"
#include "image_ops_internal.h"

namespace ravo
{
namespace
{

using image_ops_internal::absorbed_operation;
using image_ops_internal::light_control_rank;
using image_ops_internal::make_sigmoid_curve;
using image_ops_internal::parameter;
using image_ops_internal::parameter_string;
using image_ops_internal::prepare_exposure_affine;

constexpr int kGpuSharpenMaxRadius = 12;

enum class PreviewOpClass : std::uint8_t
{
    Skip = 0,
    Gpu = 1,
    Cpu = 2,
};

[[nodiscard]] bool near(const double value, const double expected) noexcept
{
    return std::abs(value - expected) <= 1.0e-12;
}

[[nodiscard]] bool identity_geometry(const OperationInstance &operation)
{
    if (operation.id == "ravo.geometry.crop")
    {
        return near(parameter(operation, "x", 0.0), 0.0) &&
               near(parameter(operation, "y", 0.0), 0.0) &&
               near(parameter(operation, "width", 1.0), 1.0) &&
               near(parameter(operation, "height", 1.0), 1.0);
    }
    if (operation.id == "ravo.geometry.rotate")
    {
        return static_cast<int>(parameter(operation, "quarters", 0.0)) % 4 == 0;
    }
    if (operation.id == "ravo.geometry.flip")
    {
        return parameter(operation, "horizontal", 0.0) == 0.0 &&
               parameter(operation, "vertical", 0.0) == 0.0;
    }
    if (operation.id == "ravo.geometry.straighten")
    {
        return near(parameter(operation, "degrees", 0.0), 0.0);
    }
    return false;
}

[[nodiscard]] bool linear_rec709_working(const LinearWorkingBuffer &working) noexcept
{
    return working.color_profile.model == ColorModel::kRgb &&
           working.color_profile.identifier == kInputProfileLinearRec709;
}

[[nodiscard]] Result<PreviewOpClass> classify_preview_operation(const LinearWorkingBuffer &working,
                                                                const OperationInstance &operation)
{
    if (!operation.enabled || absorbed_operation(operation.id) || identity_geometry(operation))
    {
        return PreviewOpClass::Skip;
    }
    if (operation.mask_id.has_value())
    {
        return PreviewOpClass::Cpu;
    }
    if (operation.id == kExposureOperationId)
    {
        auto params = exposure_from_parameters(operation.parameters);
        if (!params)
        {
            return params.error();
        }
        if (params.value().is_identity())
        {
            return PreviewOpClass::Skip;
        }
        return PreviewOpClass::Gpu;
    }
    if (light_control_rank(operation.id) >= 0)
    {
        if (near(parameter(operation, "amount", 0.0), 0.0))
        {
            return PreviewOpClass::Skip;
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == "ravo.core.contrast")
    {
        if (near(parameter(operation, "amount", 0.0), 0.0))
        {
            return PreviewOpClass::Skip;
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == "ravo.core.gamma")
    {
        if (near(parameter(operation, "gamma", 1.0), 1.0))
        {
            return PreviewOpClass::Skip;
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == "ravo.color.vibrance" || operation.id == "ravo.color.saturation")
    {
        if (near(parameter(operation, "amount", 0.0), 0.0))
        {
            return PreviewOpClass::Skip;
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == kVelviaOperationId)
    {
        if (!linear_rec709_working(working))
            return PreviewOpClass::Cpu;
        if (near(parameter(operation, "strength", 0.0), 0.0))
            return PreviewOpClass::Skip;
        return PreviewOpClass::Gpu;
    }
    if (operation.id == kSplitToningOperationId)
    {
        if (!linear_rec709_working(working))
            return PreviewOpClass::Cpu;
        if (near(parameter(operation, "mix", 0.0), 0.0))
            return PreviewOpClass::Skip;
        return PreviewOpClass::Gpu;
    }
    if (operation.id == kSharpenOperationId)
    {
        if (linear_rec709_working(working))
        {
            return PreviewOpClass::Gpu;
        }
        return PreviewOpClass::Cpu;
    }
    if (operation.id == "ravo.display.sigmoid")
    {
        const auto working_space = parameter_string(operation, "working_space",
                                                    std::string(kSigmoidWorkingSpaceLinearSrgb));
        const auto color_processing = parameter_string(
            operation, "color_processing", std::string(kSigmoidColorProcessingPerChannel));
        if (working_space != kSigmoidWorkingSpaceLinearSrgb)
        {
            return PreviewOpClass::Cpu;
        }
        if (color_processing != kSigmoidColorProcessingPerChannel &&
            color_processing != kSigmoidColorProcessingRgbRatio)
        {
            return PreviewOpClass::Cpu;
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == kRapidRawBasicToneOperationId)
    {
        auto validated = validate_rapidraw_basic_tone_parameters(operation.parameters);
        if (!validated)
        {
            return validated.error();
        }
        return PreviewOpClass::Gpu;
    }
    if (operation.id == kRapidRawToneControlsOperationId)
    {
        auto parsed = rapidraw_tone_controls_from_parameters(operation.parameters);
        if (!parsed)
            return parsed.error();
        return parsed.value().is_identity() ? PreviewOpClass::Skip : PreviewOpClass::Gpu;
    }
    return PreviewOpClass::Cpu;
}

[[nodiscard]] Result<GpuRgbPass> make_gpu_pass(const LinearWorkingBuffer &working,
                                               const OperationInstance &operation,
                                               const CancellationToken &cancellation)
{
    if (operation.id == kExposureOperationId)
    {
        auto params = exposure_from_parameters(operation.parameters);
        if (!params)
        {
            return params.error();
        }
        auto affine = prepare_exposure_affine(working, params.value(), cancellation);
        if (!affine)
        {
            return affine.error();
        }
        const auto scale = static_cast<float>(affine.value().scale);
        const auto black = static_cast<float>(affine.value().black);
        if (!std::isfinite(scale) || !std::isfinite(black))
        {
            return make_error(ErrorCode::kValidation, "GPU exposure affine is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kAffine;
        pass.affine.scale = scale;
        pass.affine.black = black;
        return pass;
    }
    const int light_rank = light_control_rank(operation.id);
    if (light_rank >= 0)
    {
        const double amount = parameter(operation, "amount", 0.0);
        const float ev = static_cast<float>(amount) * (light_rank == 0 || light_rank == 2 ?
                                                           (amount >= 0.0 ? 0.9F : 1.8F) :
                                                           (amount >= 0.0 ? 2.0F : 2.9F));
        if (!std::isfinite(ev))
        {
            return make_error(ErrorCode::kValidation, "GPU light-control amount is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kLightControls;
        if (light_rank == 0)
        {
            pass.light.highlight_ev = ev;
        }
        else if (light_rank == 1)
        {
            pass.light.shadow_ev = ev;
        }
        else if (light_rank == 2)
        {
            pass.light.white_ev = ev;
        }
        else
        {
            pass.light.black_ev = ev;
        }
        return pass;
    }
    if (operation.id == "ravo.core.contrast")
    {
        const float amount = static_cast<float>(parameter(operation, "amount", 0.0));
        if (!std::isfinite(amount))
        {
            return make_error(ErrorCode::kValidation, "GPU contrast amount is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kContrast;
        pass.contrast.amount = amount;
        return pass;
    }
    if (operation.id == "ravo.core.gamma")
    {
        const double gamma = parameter(operation, "gamma", 1.0);
        if (!std::isfinite(gamma) || gamma <= 0.0)
        {
            return make_error(ErrorCode::kValidation, "GPU gamma is not finite/positive",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kGamma;
        pass.gamma.exponent = static_cast<float>(1.0 / std::max(0.2, gamma));
        return pass;
    }
    if (operation.id == "ravo.color.vibrance" || operation.id == "ravo.color.saturation")
    {
        const float amount = static_cast<float>(parameter(operation, "amount", 0.0));
        if (!std::isfinite(amount))
        {
            return make_error(ErrorCode::kValidation, "GPU vibrance/saturation is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kVibranceSaturation;
        if (operation.id == "ravo.color.vibrance")
            pass.vibrance_saturation.vibrance_amount = amount / 1.4F;
        else
            pass.vibrance_saturation.saturation_amount = amount;
        return pass;
    }
    if (operation.id == kVelviaOperationId)
    {
        const float strength = static_cast<float>(parameter(operation, "strength", 0.0) / 100.0);
        const float bias = static_cast<float>(parameter(operation, "bias", 1.0));
        if (!std::isfinite(strength) || !std::isfinite(bias))
        {
            return make_error(ErrorCode::kValidation, "GPU velvia is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kVelvia;
        pass.velvia.strength = strength;
        pass.velvia.bias = bias;
        return pass;
    }
    if (operation.id == kSplitToningOperationId)
    {
        const float shadow_hue = static_cast<float>(parameter(operation, "shadow_hue", 0.0));
        const float shadow_saturation =
            static_cast<float>(parameter(operation, "shadow_saturation", 0.5));
        const float highlight_hue = static_cast<float>(parameter(operation, "highlight_hue", 0.2));
        const float highlight_saturation =
            static_cast<float>(parameter(operation, "highlight_saturation", 0.5));
        const float balance = static_cast<float>(parameter(operation, "balance", 0.5));
        const float compression =
            static_cast<float>(parameter(operation, "compress", 33.0) / 110.0 / 2.0);
        const float mix = static_cast<float>(parameter(operation, "mix", 1.0));
        if (!std::isfinite(shadow_hue) || !std::isfinite(shadow_saturation) ||
            !std::isfinite(highlight_hue) || !std::isfinite(highlight_saturation) ||
            !std::isfinite(balance) || !std::isfinite(compression) || !std::isfinite(mix))
        {
            return make_error(ErrorCode::kValidation, "GPU split toning is not finite",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kSplitToning;
        pass.split_toning.shadow_hue = shadow_hue;
        pass.split_toning.shadow_saturation = shadow_saturation;
        pass.split_toning.highlight_hue = highlight_hue;
        pass.split_toning.highlight_saturation = highlight_saturation;
        pass.split_toning.balance = balance;
        pass.split_toning.compression = compression;
        pass.split_toning.mix = mix;
        return pass;
    }
    if (operation.id == kSharpenOperationId)
    {
        OperationInstance canonical = operation;
        auto upgraded = upgrade_sharpen_operation(canonical);
        if (!upgraded)
        {
            return upgraded.error();
        }
        auto params = sharpen_from_parameters(canonical.parameters);
        if (!params)
        {
            return params.error();
        }
        const float radius = 2.5F * static_cast<float>(params.value().radius);
        const float amount = static_cast<float>(params.value().amount);
        const float threshold = static_cast<float>(params.value().threshold);
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kSharpen;
        pass.sharpen.width = working.width;
        pass.sharpen.height = working.height;
        pass.sharpen.amount = amount;
        pass.sharpen.threshold = threshold;
        if (radius == 0.0F)
        {
            return pass;
        }
        if (!working.canonical_roi_scale.valid())
        {
            return make_error(ErrorCode::kValidation,
                              "Sharpen requires a canonical ROI scale for a positive radius",
                              {{"reason", "invalid_sharpen_roi_scale"}});
        }
        const float scaled_radius = radius * working.canonical_roi_scale.value();
        const int taps = std::min(kGpuSharpenMaxRadius, static_cast<int>(std::ceil(scaled_radius)));
        if (taps == 0 || working.width < static_cast<std::uint32_t>(2 * taps + 1) ||
            working.height < static_cast<std::uint32_t>(2 * taps + 1))
        {
            return pass;
        }
        const float sigma2 = (1.0F / (2.5F * 2.5F)) * scaled_radius * scaled_radius;
        if (!std::isfinite(sigma2) || sigma2 <= 0.0F)
        {
            return make_error(ErrorCode::kValidation, "Sharpen Gaussian sigma is invalid",
                              {{"reason", "invalid_sharpen_sigma"}});
        }
        float weight = 0.0F;
        for (int offset = -taps; offset <= taps; ++offset)
        {
            const float value = std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma2));
            pass.sharpen.kernel[static_cast<std::size_t>(offset + taps)] = value;
            weight += value;
        }
        if (!std::isfinite(weight) || weight <= 0.0F)
        {
            return make_error(ErrorCode::kValidation, "Sharpen Gaussian weight is invalid",
                              {{"reason", "invalid_sharpen_kernel"}});
        }
        for (int index = 0; index < 2 * taps + 1; ++index)
        {
            pass.sharpen.kernel[static_cast<std::size_t>(index)] /= weight;
        }
        pass.sharpen.radius = static_cast<std::uint32_t>(taps);
        return pass;
    }
    if (operation.id == kRapidRawBasicToneOperationId)
    {
        auto validated = validate_rapidraw_basic_tone_parameters(operation.parameters);
        if (!validated)
        {
            return validated.error();
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kRapidRawBasicTone;
        return pass;
    }
    if (operation.id == kRapidRawToneControlsOperationId)
    {
        auto parsed = rapidraw_tone_controls_from_parameters(operation.parameters);
        if (!parsed)
            return parsed.error();
        if ((parsed.value().shadows != 0.0 || parsed.value().blacks != 0.0) &&
            !working.canonical_roi_scale.valid())
        {
            return make_error(ErrorCode::kValidation,
                              "RapidRAW shadows and blacks require canonical ROI scale",
                              {{"reason", "invalid_rapidraw_tone_roi_scale"}});
        }
        GpuRgbPass pass;
        pass.kind = GpuRgbPass::Kind::kRapidRawToneControls;
        pass.rapidraw_tone.width = working.width;
        pass.rapidraw_tone.height = working.height;
        pass.rapidraw_tone.radius =
            working.canonical_roi_scale.valid() ?
                static_cast<std::uint32_t>(std::max(
                    1, static_cast<int>(std::ceil(3.5F * working.canonical_roi_scale.value())))) :
                1U;
        pass.rapidraw_tone.ev_shift = static_cast<float>(parsed.value().ev_shift);
        pass.rapidraw_tone.exposure = static_cast<float>(parsed.value().exposure);
        pass.rapidraw_tone.contrast = static_cast<float>(parsed.value().contrast);
        pass.rapidraw_tone.highlights = static_cast<float>(parsed.value().highlights);
        pass.rapidraw_tone.shadows = static_cast<float>(parsed.value().shadows);
        pass.rapidraw_tone.whites = static_cast<float>(parsed.value().whites);
        pass.rapidraw_tone.blacks = static_cast<float>(parsed.value().blacks);
        return pass;
    }
    if (operation.id != "ravo.display.sigmoid")
    {
        return make_error(ErrorCode::kValidation, "GPU preview pass is not admitted",
                          {{"operation_id", operation.id}, {"reason", "gpu_pipeline_failed"}});
    }
    const auto color_processing = parameter_string(operation, "color_processing",
                                                   std::string(kSigmoidColorProcessingPerChannel));
    auto curve = make_sigmoid_curve(operation);
    if (!curve)
    {
        return curve.error();
    }
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kSigmoid;
    pass.sigmoid.mode = color_processing == kSigmoidColorProcessingRgbRatio ? 1U : 0U;
    pass.sigmoid.white_target = static_cast<float>(curve.value().white_target);
    pass.sigmoid.black_target = static_cast<float>(curve.value().black_target);
    pass.sigmoid.paper_exposure = static_cast<float>(curve.value().paper_exposure);
    pass.sigmoid.film_fog = static_cast<float>(curve.value().film_fog);
    pass.sigmoid.film_power = static_cast<float>(curve.value().film_power);
    pass.sigmoid.paper_power = static_cast<float>(curve.value().paper_power);
    pass.sigmoid.hue_preservation = static_cast<float>(curve.value().hue_preservation);
    return pass;
}

[[nodiscard]] Result<GpuRgbPass> make_vibrance_saturation_pass(const float vibrance_amount,
                                                               const float saturation_amount)
{
    if (!std::isfinite(vibrance_amount) || !std::isfinite(saturation_amount))
    {
        return make_error(ErrorCode::kValidation, "GPU vibrance/saturation is not finite",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    GpuRgbPass pass;
    pass.kind = GpuRgbPass::Kind::kVibranceSaturation;
    pass.vibrance_saturation.vibrance_amount = vibrance_amount;
    pass.vibrance_saturation.saturation_amount = saturation_amount;
    return pass;
}

// Match CPU image_recipe_ops fusion: adjacent vibrance+saturation become one apply.
[[nodiscard]] Result<std::pair<GpuRgbPass, std::size_t>>
make_gpu_pass_consuming(const LinearWorkingBuffer &working, const Recipe &recipe,
                        const std::size_t index, const CancellationToken &cancellation)
{
    if (index >= recipe.operations.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "GPU preview pass index is out of range",
                          {{"reason", "gpu_pipeline_failed"}});
    }
    const auto &operation = recipe.operations[index];
    if (operation.id == "ravo.color.vibrance" || operation.id == "ravo.color.saturation")
    {
        float vibrance = 0.0F;
        float saturation = 0.0F;
        std::size_t consumed = 1U;
        if (operation.id == "ravo.color.vibrance")
            vibrance = static_cast<float>(parameter(operation, "amount", 0.0)) / 1.4F;
        else
            saturation = static_cast<float>(parameter(operation, "amount", 0.0));
        const auto next_index = index + 1U;
        if (next_index < recipe.operations.size())
        {
            const auto &next = recipe.operations[next_index];
            if (next.enabled)
            {
                if (operation.id == "ravo.color.vibrance" && next.id == "ravo.color.saturation")
                {
                    saturation = static_cast<float>(parameter(next, "amount", 0.0));
                    consumed = 2U;
                }
                else if (operation.id == "ravo.color.saturation" &&
                         next.id == "ravo.color.vibrance")
                {
                    vibrance = static_cast<float>(parameter(next, "amount", 0.0)) / 1.4F;
                    consumed = 2U;
                }
            }
        }
        auto pass = make_vibrance_saturation_pass(vibrance, saturation);
        if (!pass)
            return pass.error();
        return std::pair<GpuRgbPass, std::size_t>{std::move(pass).value(), consumed};
    }
    auto pass = make_gpu_pass(working, operation, cancellation);
    if (!pass)
        return pass.error();
    return std::pair<GpuRgbPass, std::size_t>{std::move(pass).value(), 1U};
}

[[nodiscard]] Result<bool> recipe_has_gpu_admissible_rgb(const LinearWorkingBuffer &working,
                                                         const Recipe &recipe)
{
    for (const auto &operation : recipe.operations)
    {
        auto classified = classify_preview_operation(working, operation);
        if (!classified)
        {
            return classified.error();
        }
        if (classified.value() == PreviewOpClass::Gpu)
        {
            return true;
        }
    }
    return false;
}

} // namespace

Result<std::optional<std::vector<GpuRgbPass>>>
gpu_preview_rgb_passes(const LinearWorkingBuffer &working, const Recipe &recipe,
                       const CancellationToken &cancellation)
{
    std::vector<GpuRgbPass> passes;
    for (std::size_t index = 0U; index < recipe.operations.size();)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto classified = classify_preview_operation(working, recipe.operations[index]);
        if (!classified)
        {
            return classified.error();
        }
        if (classified.value() == PreviewOpClass::Skip)
        {
            ++index;
            continue;
        }
        if (classified.value() == PreviewOpClass::Cpu)
        {
            if (passes.empty())
            {
                return std::optional<std::vector<GpuRgbPass>>{};
            }
            break;
        }
        auto consumed = make_gpu_pass_consuming(working, recipe, index, cancellation);
        if (!consumed)
        {
            return consumed.error();
        }
        passes.push_back(std::move(consumed.value().first));
        index += consumed.value().second;
    }
    return std::optional<std::vector<GpuRgbPass>>{std::move(passes)};
}

Result<LinearWorkingBuffer> apply_gpu_preview_rgb(const LinearWorkingBuffer &working,
                                                  const std::span<const GpuRgbPass> passes,
                                                  const GpuAdapter &gpu,
                                                  const CancellationToken &cancellation,
                                                  GpuRgbApplyOptions options)
{
    LinearWorkingBuffer output;
    output.width = working.width;
    output.height = working.height;
    output.color_profile = working.color_profile;
    output.exposure_analysis = working.exposure_analysis;
    output.canonical_roi_scale = working.canonical_roi_scale;
    output.mask_attached_frame = working.mask_attached_frame;
    if (options.download)
    {
        output.rgb.resize(working.rgb.size());
    }
    if (passes.empty())
    {
        if (options.download)
        {
            output.rgb = working.rgb;
        }
        return output;
    }
    options.width = working.width;
    options.height = working.height;
    auto applied = gpu.apply_rgb_passes(working.rgb, output.rgb, passes, options, cancellation);
    if (!applied)
    {
        return applied.error();
    }
    if (options.download)
    {
        for (std::size_t index = 0; index < output.rgb.size(); ++index)
        {
            if (!std::isfinite(output.rgb[index]))
            {
                return make_error(
                    ErrorCode::kValidation, "GPU preview produced a non-finite sample",
                    {{"reason", "gpu_pipeline_failed"}, {"sample_index", std::to_string(index)}});
            }
        }
    }
    return output;
}

Result<LinearWorkingBuffer>
apply_preview_rgb(LinearWorkingBuffer working, const Recipe &recipe, const GpuAdapter *gpu,
                  std::string *gpu_backend, const CancellationToken &cancellation,
                  const bool need_cpu_pixels, const std::uint32_t display_slot,
                  const bool prefer_retained_source, const GpuDisplayPublishCrop publish_crop)
{
    if (gpu_backend != nullptr)
    {
        gpu_backend->clear();
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (gpu == nullptr)
    {
        return apply_recipe_ops(std::move(working), recipe, cancellation);
    }
    auto has_gpu = recipe_has_gpu_admissible_rgb(working, recipe);
    if (!has_gpu)
    {
        return has_gpu.error();
    }
    if (!has_gpu.value())
    {
        return apply_recipe_ops(std::move(working), recipe, cancellation);
    }

    Recipe remaining = recipe;
    std::size_t index = 0U;
    bool allow_retained = prefer_retained_source;
    while (index < remaining.operations.size())
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto classified = classify_preview_operation(working, remaining.operations[index]);
        if (!classified)
        {
            return classified.error();
        }
        if (classified.value() == PreviewOpClass::Skip)
        {
            ++index;
            continue;
        }
        if (classified.value() == PreviewOpClass::Gpu)
        {
            std::vector<GpuRgbPass> batch;
            std::size_t cursor = index;
            while (cursor < remaining.operations.size())
            {
                auto batch_class =
                    classify_preview_operation(working, remaining.operations[cursor]);
                if (!batch_class)
                {
                    return batch_class.error();
                }
                if (batch_class.value() == PreviewOpClass::Skip)
                {
                    ++cursor;
                    continue;
                }
                if (batch_class.value() != PreviewOpClass::Gpu)
                {
                    break;
                }
                auto consumed = make_gpu_pass_consuming(working, remaining, cursor, cancellation);
                if (!consumed)
                {
                    return consumed.error();
                }
                batch.push_back(std::move(consumed.value().first));
                const std::size_t span = consumed.value().second;
                for (std::size_t offset = 0U; offset < span; ++offset)
                    remaining.operations[cursor + offset].enabled = false;
                cursor += span;
            }
            bool cpu_after = false;
            for (std::size_t look = cursor; look < remaining.operations.size(); ++look)
            {
                auto later = classify_preview_operation(working, remaining.operations[look]);
                if (!later)
                {
                    return later.error();
                }
                if (later.value() == PreviewOpClass::Cpu)
                {
                    cpu_after = true;
                    break;
                }
            }
            GpuRgbApplyOptions gpu_options;
#if defined(__APPLE__)
            const bool can_publish = true;
#else
            const bool can_publish = false;
#endif
            // Metal can publish an IOSurface for QML even when the caller also
            // downloads CPU RGB for live identity, comparison, and tests.
            gpu_options.publish_display = !cpu_after && can_publish;
            gpu_options.download = need_cpu_pixels || cpu_after || !gpu_options.publish_display;
            gpu_options.from_retained_source =
                allow_retained && gpu->has_retained_source(working.width, working.height);
            gpu_options.width = working.width;
            gpu_options.height = working.height;
            gpu_options.display_slot = display_slot;
            gpu_options.publish_crop_x = publish_crop.x;
            gpu_options.publish_crop_y = publish_crop.y;
            gpu_options.publish_crop_w = publish_crop.width;
            gpu_options.publish_crop_h = publish_crop.height;
            auto gpu_image = apply_gpu_preview_rgb(working, batch, *gpu, cancellation, gpu_options);
            if (!gpu_image)
            {
                return gpu_image.error();
            }
            working = std::move(gpu_image).value();
            allow_retained = false;
            if (gpu_backend != nullptr)
            {
                *gpu_backend = std::string(gpu->backend_id());
            }
            index = cursor;
            continue;
        }

        std::size_t cursor = index;
        while (cursor < remaining.operations.size())
        {
            auto cpu_class = classify_preview_operation(working, remaining.operations[cursor]);
            if (!cpu_class)
            {
                return cpu_class.error();
            }
            if (cpu_class.value() == PreviewOpClass::Skip)
            {
                ++cursor;
                continue;
            }
            if (cpu_class.value() == PreviewOpClass::Gpu)
            {
                break;
            }
            ++cursor;
        }
        Recipe cpu_recipe = remaining;
        for (std::size_t op_index = 0U; op_index < cpu_recipe.operations.size(); ++op_index)
        {
            if (op_index < index || op_index >= cursor)
            {
                cpu_recipe.operations[op_index].enabled = false;
            }
        }
        auto cpu_image = apply_recipe_ops(std::move(working), cpu_recipe, cancellation);
        if (!cpu_image)
        {
            return cpu_image.error();
        }
        working = std::move(cpu_image).value();
        allow_retained = false;
        for (std::size_t op_index = index; op_index < cursor; ++op_index)
        {
            remaining.operations[op_index].enabled = false;
        }
        index = cursor;
    }
    return working;
}

} // namespace ravo
