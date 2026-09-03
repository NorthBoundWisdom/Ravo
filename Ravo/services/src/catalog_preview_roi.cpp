#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/watermark.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>

namespace ravo
{
namespace
{

[[nodiscard]] double parameter_number(const OperationInstance &operation,
                                      const std::string_view key, const double fallback)
{
    const auto found = operation.parameters.find(std::string(key));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *value = std::get_if<double>(&found->second.value))
    {
        return *value;
    }
    if (const auto *value = std::get_if<std::int64_t>(&found->second.value))
    {
        return static_cast<double>(*value);
    }
    return fallback;
}

[[nodiscard]] Result<void> validate_preview_roi(const PreviewNormRect &roi)
{
    if (!std::isfinite(roi.x) || !std::isfinite(roi.y) || !std::isfinite(roi.width) ||
        !std::isfinite(roi.height) || roi.x < 0.0 || roi.y < 0.0 || roi.width <= 0.0 ||
        roi.height <= 0.0 || roi.x + roi.width > 1.0 + 1.0e-9 || roi.y + roi.height > 1.0 + 1.0e-9)
    {
        return make_error(ErrorCode::kInvalidArgument, "Preview ROI is not a unit rectangle",
                          {{"reason", "invalid_preview_roi"}});
    }
    if (roi.width >= 0.8 && roi.height >= 0.8)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Preview ROI covers the full frame; use the settled preview",
                          {{"reason", "preview_roi_covers_full_frame"}});
    }
    return {};
}

[[nodiscard]] Result<void> preview_roi_recipe_supported(const Recipe &recipe)
{
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        if (operation.id == kPerspectiveOperationId || operation.id == kCanvasOperationId ||
            operation.id == "ravo.geometry.lens" || operation.id == kDehazeOperationId ||
            operation.id == kRetouchOperationId ||
            operation.id == kColorReconstructionOperationId ||
            operation.id == kWatermarkOperationId || operation.id == kFrameOperationId ||
            operation.id == "ravo.effect.vignette" || operation.id == "ravo.effect.graduatednd" ||
            operation.id == "ravo.effect.grain" || operation.id == "ravo.effect.bloom" ||
            operation.id == "ravo.effect.soften")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Preview ROI is unavailable for this recipe geometry",
                              {{"reason", "preview_roi_geometry_unsupported"},
                               {"operation_id", std::string(operation.id)}});
        }
        if (operation.id == "ravo.geometry.straighten" &&
            std::abs(parameter_number(operation, "degrees", 0.0)) > 1.0e-9)
        {
            return make_error(ErrorCode::kUnsupported, "Preview ROI is unavailable with straighten",
                              {{"reason", "preview_roi_geometry_unsupported"},
                               {"operation_id", std::string(operation.id)}});
        }
        if (operation.id == "ravo.geometry.rotate" &&
            static_cast<int>(parameter_number(operation, "quarters", 0.0)) % 4 != 0)
        {
            return make_error(ErrorCode::kUnsupported, "Preview ROI is unavailable with rotate",
                              {{"reason", "preview_roi_geometry_unsupported"},
                               {"operation_id", std::string(operation.id)}});
        }
        if (operation.id == "ravo.geometry.flip" &&
            (parameter_number(operation, "horizontal", 0.0) != 0.0 ||
             parameter_number(operation, "vertical", 0.0) != 0.0))
        {
            return make_error(ErrorCode::kUnsupported, "Preview ROI is unavailable with flip",
                              {{"reason", "preview_roi_geometry_unsupported"},
                               {"operation_id", std::string(operation.id)}});
        }
    }
    return {};
}

[[nodiscard]] std::string roi_working_preprocess_key(const Recipe &recipe)
{
    std::string key = raw_preprocess_key(recipe);
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kDemosaicOperationId)
        {
            continue;
        }
        key += ":demosaic";
        for (const auto &[name, value] : operation.parameters)
        {
            key.push_back(':');
            key += name;
            key.push_back('=');
            if (const auto *text = std::get_if<std::string>(&value.value))
            {
                key += *text;
            }
            else if (const auto *number = std::get_if<double>(&value.value))
            {
                key += std::to_string(*number);
            }
            else if (const auto *integer = std::get_if<std::int64_t>(&value.value))
            {
                key += std::to_string(*integer);
            }
        }
    }
    return key;
}

void disable_mapped_geometry(Recipe &recipe)
{
    for (auto &operation : recipe.operations)
    {
        if (operation.id == "ravo.geometry.crop" || operation.id == "ravo.geometry.rotate" ||
            operation.id == "ravo.geometry.flip")
        {
            operation.enabled = false;
        }
    }
}

} // namespace

Result<CatalogService::CachedRoiLinearWorking *> CatalogService::cached_roi_linear_working(
    const AssetRecord &asset, const std::string_view path, const Recipe &recipe,
    const std::uint32_t origin_x, const std::uint32_t origin_y, const std::uint32_t width,
    const std::uint32_t height, const CancellationToken &cancellation)
{
    if (engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    auto color_fingerprint = engine_->input_color_cache_fingerprint(recipe);
    if (!color_fingerprint)
    {
        return color_fingerprint.error();
    }
    const std::string preprocess_key =
        roi_working_preprocess_key(recipe) + ":" + color_fingerprint.value();
    if (roi_linear_working_.has_value() && roi_linear_working_->asset_id == asset.id &&
        roi_linear_working_->fingerprint == fingerprint &&
        roi_linear_working_->preprocess_key == preprocess_key &&
        roi_linear_working_->origin_x == origin_x && roi_linear_working_->origin_y == origin_y &&
        roi_linear_working_->width == width && roi_linear_working_->height == height)
    {
        return &*roi_linear_working_;
    }
    auto raw = cached_raw_frame(asset, path, cancellation, PreviewLane::kForegroundDevelop);
    if (!raw)
    {
        return raw.error();
    }
    auto linear = engine_->linear_working_from_raw_window(*raw.value(), recipe, origin_x, origin_y,
                                                          width, height, cancellation);
    if (!linear)
    {
        return linear.error();
    }
    const auto generation =
        roi_linear_working_.has_value() ? roi_linear_working_->generation + 1U : 1U;
    roi_linear_working_ = CachedRoiLinearWorking{
        .asset_id = std::string(asset.id),
        .fingerprint = fingerprint,
        .preprocess_key = preprocess_key,
        .origin_x = origin_x,
        .origin_y = origin_y,
        .width = width,
        .height = height,
        .buffer = std::move(linear).value(),
        .interactive_render_cache = {},
        .generation = generation,
    };
    return &*roi_linear_working_;
}

Result<PreviewResult> CatalogService::generate_roi_preview(const AssetRecord &asset,
                                                           const PreviewRequest &request,
                                                           const Recipe &recipe,
                                                           const std::string_view path)
{
    if (engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    if (!request.roi.has_value())
    {
        return make_error(ErrorCode::kInvalidArgument, "Preview ROI is required",
                          {{"reason", "missing_preview_roi"}});
    }
    auto valid_roi = validate_preview_roi(*request.roi);
    if (!valid_roi)
    {
        return valid_roi.error();
    }
    if (!is_raw_media_type(asset.media_type))
    {
        return make_error(
            ErrorCode::kUnsupported, "Preview ROI requires a RAW asset",
            {{"reason", "preview_roi_media_unsupported"}, {"media_type", asset.media_type}});
    }
    auto supported = preview_roi_recipe_supported(recipe);
    if (!supported)
    {
        return supported.error();
    }
    const auto source_width = asset.width.value_or(0);
    const auto source_height = asset.height.value_or(0);
    if (source_width == 0U || source_height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Asset dimensions are required for preview ROI",
                          {{"reason", "missing_asset_dimensions"}});
    }

    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_w = 1.0;
    double crop_h = 1.0;
    if (!request.ignore_crop)
    {
        for (const auto &operation : recipe.operations)
        {
            if (!operation.enabled || operation.id != "ravo.geometry.crop")
            {
                continue;
            }
            crop_x = parameter_number(operation, "x", 0.0);
            crop_y = parameter_number(operation, "y", 0.0);
            crop_w = parameter_number(operation, "width", 1.0);
            crop_h = parameter_number(operation, "height", 1.0);
        }
    }
    const double display_x = crop_x + request.roi->x * crop_w;
    const double display_y = crop_y + request.roi->y * crop_h;
    const double display_w = request.roi->width * crop_w;
    const double display_h = request.roi->height * crop_h;
    auto pixel = [&](const double normalized, const std::uint32_t extent) -> std::uint32_t
    {
        return static_cast<std::uint32_t>(
            std::clamp(std::llround(normalized * static_cast<double>(extent)), 0LL,
                       static_cast<long long>(extent > 0U ? extent - 1U : 0U)));
    };
    const auto px = pixel(display_x, source_width);
    const auto py = pixel(display_y, source_height);
    auto pw = static_cast<std::uint32_t>(
        std::clamp(std::llround(display_w * static_cast<double>(source_width)), 1LL,
                   static_cast<long long>(source_width)));
    auto ph = static_cast<std::uint32_t>(
        std::clamp(std::llround(display_h * static_cast<double>(source_height)), 1LL,
                   static_cast<long long>(source_height)));
    if (px + pw > source_width)
    {
        pw = source_width - px;
    }
    if (py + ph > source_height)
    {
        ph = source_height - py;
    }
    if (pw == 0U || ph == 0U)
    {
        return make_error(ErrorCode::kInvalidArgument, "Preview ROI is empty after mapping",
                          {{"reason", "empty_preview_roi"}});
    }

    auto raw = cached_raw_frame(asset, path, request.cancellation, PreviewLane::kForegroundDevelop);
    if (!raw)
    {
        return raw.error();
    }
    auto cfa = map_display_rect_to_cfa(*raw.value(), px, py, pw, ph);
    if (!cfa)
    {
        return cfa.error();
    }
    Recipe window_recipe = recipe;
    disable_mapped_geometry(window_recipe);
    auto linear =
        cached_roi_linear_working(asset, path, window_recipe, cfa.value().x, cfa.value().y,
                                  cfa.value().width, cfa.value().height, request.cancellation);
    if (!linear)
    {
        return linear.error();
    }
    Recipe rgb_recipe = window_recipe;
    disable_raw_preprocess(rgb_recipe);
    auto applied = engine_->render_interactive_linear_working(
        linear.value()->buffer, rgb_recipe, linear.value()->interactive_render_cache,
        request.cancellation, request.overlay_mask_id);
    if (!applied)
    {
        return applied.error();
    }
    LOG_INFO(ravo::logger(), "preview roi asset={} {}x{} cfa={},{} {}x{}", asset.id,
             applied.value().width, applied.value().height, cfa.value().x, cfa.value().y,
             cfa.value().width, cfa.value().height);
    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request.request_revision;
    result.width = applied.value().width;
    result.height = applied.value().height;
    result.rgb = std::move(applied.value().rgb);
    result.color_profile = std::move(applied.value().color_profile);
    result.mask_alpha = std::move(applied.value().mask_alpha);
    result.gpu_backend = applied.value().gpu_backend;
    result.original_missing = false;
    return result;
}

} // namespace ravo
