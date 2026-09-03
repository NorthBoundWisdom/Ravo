#include "gpu_preview.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "image_ops.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "image_ops_internal.h"

namespace ravo
{
namespace
{

using image_ops_internal::absorbed_operation;
using image_ops_internal::make_sigmoid_curve;
using image_ops_internal::parameter;
using image_ops_internal::parameter_string;
using image_ops_internal::prepare_exposure_affine;

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

[[nodiscard]] Result<PreviewOpClass> classify_preview_operation(const OperationInstance &operation)
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

[[nodiscard]] Result<bool> recipe_has_gpu_admissible_rgb(const Recipe &recipe)
{
    for (const auto &operation : recipe.operations)
    {
        auto classified = classify_preview_operation(operation);
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
    for (const auto &operation : recipe.operations)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto classified = classify_preview_operation(operation);
        if (!classified)
        {
            return classified.error();
        }
        if (classified.value() == PreviewOpClass::Skip)
        {
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
        auto pass = make_gpu_pass(working, operation, cancellation);
        if (!pass)
        {
            return pass.error();
        }
        passes.push_back(std::move(pass).value());
    }
    return std::optional<std::vector<GpuRgbPass>>{std::move(passes)};
}

Result<LinearWorkingBuffer> apply_gpu_preview_rgb(const LinearWorkingBuffer &working,
                                                  const std::span<const GpuRgbPass> passes,
                                                  const GpuAdapter &gpu,
                                                  const CancellationToken &cancellation)
{
    LinearWorkingBuffer output;
    output.width = working.width;
    output.height = working.height;
    output.color_profile = working.color_profile;
    output.exposure_analysis = working.exposure_analysis;
    output.canonical_roi_scale = working.canonical_roi_scale;
    output.mask_attached_frame = working.mask_attached_frame;
    output.rgb.resize(working.rgb.size());
    if (passes.empty())
    {
        output.rgb = working.rgb;
        return output;
    }
    auto applied = gpu.apply_rgb_passes(working.rgb, output.rgb, passes, cancellation);
    if (!applied)
    {
        return applied.error();
    }
    for (std::size_t index = 0; index < output.rgb.size(); ++index)
    {
        if (!std::isfinite(output.rgb[index]))
        {
            return make_error(
                ErrorCode::kValidation, "GPU preview produced a non-finite sample",
                {{"reason", "gpu_pipeline_failed"}, {"sample_index", std::to_string(index)}});
        }
    }
    return output;
}

Result<LinearWorkingBuffer> apply_preview_rgb(LinearWorkingBuffer working, const Recipe &recipe,
                                              const GpuAdapter *gpu, std::string *gpu_backend,
                                              const CancellationToken &cancellation)
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
    auto has_gpu = recipe_has_gpu_admissible_rgb(recipe);
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
    while (index < remaining.operations.size())
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto classified = classify_preview_operation(remaining.operations[index]);
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
                auto batch_class = classify_preview_operation(remaining.operations[cursor]);
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
                auto pass = make_gpu_pass(working, remaining.operations[cursor], cancellation);
                if (!pass)
                {
                    return pass.error();
                }
                batch.push_back(std::move(pass).value());
                remaining.operations[cursor].enabled = false;
                ++cursor;
            }
            auto gpu_image = apply_gpu_preview_rgb(working, batch, *gpu, cancellation);
            if (!gpu_image)
            {
                return gpu_image.error();
            }
            working = std::move(gpu_image).value();
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
            auto cpu_class = classify_preview_operation(remaining.operations[cursor]);
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
        for (std::size_t op_index = index; op_index < cursor; ++op_index)
        {
            remaining.operations[op_index].enabled = false;
        }
        index = cursor;
    }
    return working;
}

} // namespace ravo
