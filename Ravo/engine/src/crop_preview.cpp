#include "ravo/engine/crop_preview.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <variant>

#include "canvas_frame.h"
#include "perspective_transform.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
Result<CropPreviewPlan> plan_crop_preview(const Recipe &recipe, std::uint32_t width,
                                          std::uint32_t height,
                                          const CancellationToken &cancellation)
try
{
    if (width < 2 || height < 2)
        return make_error(ErrorCode::kValidation, "Crop preview dimensions are invalid");
    CropPreviewPlan plan;
    plan.recipe = recipe;
    strip_crop_operations(plan.recipe);
    // Include the pixel-centre rounding margin at the maximum rotated extent.
    double extent =
        std::ceil(std::hypot(static_cast<double>(width), static_cast<double>(height))) + 1.0;
    for (auto &operation : plan.recipe.operations)
    {
        if (!operation.enabled || operation.bypass)
            continue;
        if (operation.id == kCanvasOperationId)
        {
            auto params = canvas_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto layout = compute_canvas_layout(width, height, params.value());
            if (!layout)
                return layout.error();
            width = layout.value().output_width;
            height = layout.value().output_height;
            extent =
                std::ceil(std::hypot(static_cast<double>(width), static_cast<double>(height))) +
                1.0;
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            const auto found = operation.parameters.find("quarters");
            if (found == operation.parameters.end())
                return make_error(ErrorCode::kValidation, "Crop preview rotation is missing");
            const auto *quarters = std::get_if<std::int64_t>(&found->second.value);
            if (!quarters)
                return make_error(ErrorCode::kValidation, "Crop preview rotation is invalid");
            if (*quarters % 2 != 0)
                std::swap(width, height);
        }
        else if (operation.id == kPerspectiveOperationId)
        {
            auto params = perspective_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto layout = compute_perspective_layout(width, height, params.value());
            if (!layout)
                return layout.error();
            plan.x = static_cast<double>(layout.value().output_left) / layout.value().full_width;
            plan.y = static_cast<double>(layout.value().output_top) / layout.value().full_height;
            plan.width =
                static_cast<double>(layout.value().output_width) / layout.value().full_width;
            plan.height =
                static_cast<double>(layout.value().output_height) / layout.value().full_height;
            auto coverage =
                perspective_source_coverage(width, height, layout.value(), cancellation);
            if (!coverage)
                return coverage.error();
            plan.source_coverage = std::move(coverage).value();
            plan.coverage_width = layout.value().full_width;
            plan.coverage_height = layout.value().full_height;
            width = layout.value().full_width;
            height = layout.value().full_height;
            params.value().constrain_crop = false;
            auto parameters = perspective_to_parameters(params.value());
            if (!parameters)
                return parameters.error();
            operation.parameters = std::move(parameters).value();
        }
        // Decorative output borders do not form part of the crop source.
        else if (operation.id == kFrameOperationId)
            operation.enabled = false;
    }
    plan.width_scale = static_cast<double>(width) / extent;
    plan.height_scale = static_cast<double>(height) / extent;
    return plan;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Crop preview planning allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> fill_crop_preview_exterior(RenderedImage &image, const CropPreviewPlan &plan,
                                        const CancellationToken &cancellation)
{
    if (plan.source_coverage.empty())
        return cancellation.check();
    const auto pixels = static_cast<std::size_t>(image.width) * image.height;
    if (image.width != plan.coverage_width || image.height != plan.coverage_height ||
        plan.source_coverage.size() != pixels || image.rgb.size() != pixels * 3U)
        return make_error(ErrorCode::kValidation, "Crop coverage does not match preview pixels");
    for (std::uint32_t row = 0; row < image.height; ++row)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (std::uint32_t column = 0; column < image.width; ++column)
        {
            const auto pixel = static_cast<std::size_t>(row) * image.width + column;
            if (plan.source_coverage[pixel] == 0)
                std::fill_n(image.rgb.data() + pixel * 3U, 3, std::uint8_t{118});
        }
    }
    // The published CPU bytes include the surround; a prior native surface does not.
    image.gpu_display_generation = 0;
    return {};
}
} // namespace ravo
