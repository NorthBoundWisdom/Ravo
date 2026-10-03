#include "ravo/engine/crop_preview.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <variant>

#include "canvas_frame.h"
#include "perspective_transform.h"
#include "ravo/recipe/develop.h"
#include "image_ops_internal.h"

namespace ravo
{
namespace
{
using image_ops_internal::parameter;
Result<PreviewSourceSize> preview_size_at_edge(const Recipe &recipe,
                                               const std::uint32_t source_width,
                                               const std::uint32_t source_height,
                                               const std::uint32_t edge,
                                               const CancellationToken &cancellation)
{
    const auto native_edge = std::max(source_width, source_height);
    const auto width =
        std::max(1U, static_cast<std::uint32_t>(std::uint64_t(source_width) * edge / native_edge));
    const auto height =
        std::max(1U, static_cast<std::uint32_t>(std::uint64_t(source_height) * edge / native_edge));
    PreviewSourceSize size{width, height, width, height};
    for (const auto &operation : recipe.operations)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        if (!operation.enabled || operation.bypass)
            continue;
        auto &w = size.output_width;
        auto &h = size.output_height;
        if (operation.id == kCanvasOperationId)
        {
            auto params = canvas_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto layout = compute_canvas_layout(w, h, params.value());
            if (!layout)
                return layout.error();
            w = layout.value().output_width;
            h = layout.value().output_height;
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            if (static_cast<int>(parameter(operation, "quarters", 0.0)) % 2 != 0)
                std::swap(w, h);
        }
        else if (operation.id == kPerspectiveOperationId)
        {
            auto params = perspective_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto layout = compute_perspective_layout(w, h, params.value());
            if (!layout)
                return layout.error();
            w = layout.value().output_width;
            h = layout.value().output_height;
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            const auto left = std::clamp(std::llround(parameter(operation, "x", 0.0) * w), 0LL,
                                         static_cast<long long>(w - 1));
            const auto top = std::clamp(std::llround(parameter(operation, "y", 0.0) * h), 0LL,
                                        static_cast<long long>(h - 1));
            w = static_cast<std::uint32_t>(
                std::min(std::clamp(std::llround(parameter(operation, "width", 1.0) * w), 1LL,
                                    static_cast<long long>(w)),
                         static_cast<long long>(w) - left));
            h = static_cast<std::uint32_t>(
                std::min(std::clamp(std::llround(parameter(operation, "height", 1.0) * h), 1LL,
                                    static_cast<long long>(h)),
                         static_cast<long long>(h) - top));
        }
        else if (operation.id == kFrameOperationId)
        {
            auto params = frame_from_parameters(operation.parameters);
            if (!params)
                return params.error();
            auto layout = compute_frame_layout(w, h, params.value());
            if (!layout)
                return layout.error();
            w = layout.value().output_width;
            h = layout.value().output_height;
        }
    }
    return size;
}
} // namespace

Result<PreviewSourceSize> plan_preview_source_size(const Recipe &recipe,
                                                   const std::uint32_t source_width,
                                                   const std::uint32_t source_height,
                                                   const std::uint32_t max_edge,
                                                   const CancellationToken &cancellation)
try
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (source_width == 0 || source_height == 0)
        return PreviewSourceSize{};
    const auto native_edge = std::max(source_width, source_height);
    const auto base_edge = max_edge == 0 ? native_edge : std::min(max_edge, native_edge);
    auto base = preview_size_at_edge(recipe, source_width, source_height, base_edge, cancellation);
    if (!base)
        return base.error();
    if (base_edge == native_edge ||
        std::max(base.value().output_width, base.value().output_height) >= max_edge)
        return base;
    auto native =
        preview_size_at_edge(recipe, source_width, source_height, native_edge, cancellation);
    if (!native)
        return native.error();
    if (std::max(native.value().output_width, native.value().output_height) <= max_edge)
        return native;
    // Integer crop and constrained-perspective rounding must match rendering.
    // Find the densest native-bounded source whose final photo fits the request.
    auto low = base_edge;
    auto high = native_edge;
    while (high - low > 1U)
    {
        const auto middle = low + (high - low) / 2U;
        auto candidate =
            preview_size_at_edge(recipe, source_width, source_height, middle, cancellation);
        if (!candidate)
            return candidate.error();
        if (std::max(candidate.value().output_width, candidate.value().output_height) <= max_edge)
        {
            low = middle;
            base = std::move(candidate);
        }
        else
            high = middle;
    }
    return base;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Preview source planning allocation failed",
                      {{"reason", "allocation_failed"}});
}

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
