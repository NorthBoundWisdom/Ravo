#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/canvas_frame.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/perspective.h"
#include "ravo/recipe/retouch.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/rapidraw_tone_controls.h"
#include "ravo/recipe/sharpen.h"
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

// Spatial RGB ops (Lab USM / RapidRAW tone radius) need neighbors outside the
// owned ROI. Expand the demosaic window by this apron, process, then crop back
// so owned pixels match full-frame export crop (macOS IQ-00 contract).
[[nodiscard]] std::uint32_t preview_roi_spatial_apron_px(const Recipe &recipe,
                                                         const std::uint32_t width,
                                                         const std::uint32_t height) noexcept
{
    std::uint32_t apron = 0U;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
            continue;
        if (operation.id == kSharpenOperationId)
        {
            // Engine scales recipe radius by 2.5 before ceil for the Gaussian.
            const double radius = parameter_number(operation, "radius", 2.0);
            if (std::isfinite(radius) && radius > 0.0)
            {
                const auto px = static_cast<std::uint32_t>(std::max(0.0, std::ceil(2.5 * radius)));
                apron = std::max(apron, px);
            }
        }
        else if (operation.id == kRapidRawToneControlsOperationId)
        {
            // RapidRAW scales its 3.5px reference radius from a 1080px short edge.
            const double scale = static_cast<double>(std::min(width, height)) / 1080.0;
            apron =
                std::max(apron, static_cast<std::uint32_t>(std::max(1.0, std::ceil(3.5 * scale))));
        }
    }
    // Keep a small floor so mild demosaic/filter edge bleed stays outside owned
    // pixels even when the recipe has no explicit spatial radius ops.
    return std::max(apron, 2U);
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
    const std::uint32_t spatial_apron =
        preview_roi_spatial_apron_px(recipe, source_width, source_height);
    const std::uint32_t expanded_x = px > spatial_apron ? px - spatial_apron : 0U;
    const std::uint32_t expanded_y = py > spatial_apron ? py - spatial_apron : 0U;
    const std::uint32_t expanded_x2 = std::min(source_width, px + pw + spatial_apron);
    const std::uint32_t expanded_y2 = std::min(source_height, py + ph + spatial_apron);
    const std::uint32_t expanded_w = expanded_x2 - expanded_x;
    const std::uint32_t expanded_h = expanded_y2 - expanded_y;
    const std::uint32_t crop_x_px = px - expanded_x;
    const std::uint32_t crop_y_px = py - expanded_y;
    auto cfa =
        map_display_rect_to_cfa(*raw.value(), expanded_x, expanded_y, expanded_w, expanded_h);
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
    // Metal publishes an owned-size IOSurface after the spatial apron via
    // GpuDisplayPublishCrop. CPU pixels are only required when the caller asks
    // for them (IQ probes) or when GPU-native publish is unavailable and the
    // owned window must be cropped from the expanded CPU render.
    const bool need_cpu_pixels = request.need_cpu_pixels;
    const GpuDisplayPublishCrop display_crop{crop_x_px, crop_y_px, pw, ph};
    auto applied = engine_->render_interactive_linear_working(
        linear.value()->buffer, rgb_recipe, linear.value()->interactive_render_cache,
        request.cancellation, request.overlay_mask_id, need_cpu_pixels,
        EngineFacade::GpuDisplayKind::kRoi, display_crop);
    if (!applied)
    {
        return applied.error();
    }
    if (applied.value().width != expanded_w || applied.value().height != expanded_h)
    {
        return make_error(ErrorCode::kInternal,
                          "Preview ROI render size does not match the expanded window",
                          {{"reason", "preview_roi_size_mismatch"},
                           {"render_width", std::to_string(applied.value().width)},
                           {"render_height", std::to_string(applied.value().height)},
                           {"expanded_width", std::to_string(expanded_w)},
                           {"expanded_height", std::to_string(expanded_h)}});
    }
    if (crop_x_px + pw > applied.value().width || crop_y_px + ph > applied.value().height)
    {
        return make_error(ErrorCode::kInternal, "Preview ROI owned crop is outside the render",
                          {{"reason", "preview_roi_crop_out_of_bounds"}});
    }
    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request.request_revision;
    result.width = pw;
    result.height = ph;
    result.color_profile = std::move(applied.value().color_profile);
    result.gpu_backend = applied.value().gpu_backend;
    result.gpu_display_generation = applied.value().gpu_display_generation;
    if (result.gpu_display_generation != 0U)
    {
        const auto frame = engine_->gpu_display_frame(EngineFacade::GpuDisplayKind::kRoi);
        if (frame.generation == result.gpu_display_generation && frame.width == pw &&
            frame.height == ph && frame.native_surface != 0U)
        {
            result.gpu_display_width = frame.width;
            result.gpu_display_height = frame.height;
            result.gpu_display_native_surface = frame.native_surface;
        }
        else
        {
            // Owned-size Metal surface missing/mismatched: fall back to CPU crop.
            result.gpu_display_generation = 0U;
            result.gpu_display_width = 0U;
            result.gpu_display_height = 0U;
            result.gpu_display_native_surface = 0U;
            if (applied.value().rgb.empty())
            {
                return make_error(ErrorCode::kIo,
                                  "Preview ROI GPU surface is not owned-size after apron publish",
                                  {{"reason", "preview_roi_gpu_apron_surface_mismatch"},
                                   {"frame_width", std::to_string(frame.width)},
                                   {"frame_height", std::to_string(frame.height)},
                                   {"owned_width", std::to_string(pw)},
                                   {"owned_height", std::to_string(ph)}});
            }
        }
    }
    if (!applied.value().rgb.empty())
    {
        result.rgb.resize(static_cast<std::size_t>(pw) * ph * 3U);
        for (std::uint32_t row = 0U; row < ph; ++row)
        {
            const auto src =
                (static_cast<std::size_t>(crop_y_px + row) * applied.value().width + crop_x_px) *
                3U;
            const auto dst = static_cast<std::size_t>(row) * pw * 3U;
            std::copy_n(applied.value().rgb.begin() + static_cast<std::ptrdiff_t>(src),
                        static_cast<std::size_t>(pw) * 3U,
                        result.rgb.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    }
    else if (result.gpu_display_generation == 0U)
    {
        return make_error(ErrorCode::kIo, "Preview ROI render produced neither CPU nor GPU pixels",
                          {{"reason", "preview_roi_pixels_missing"}});
    }
    if (!applied.value().mask_alpha.empty())
    {
        if (applied.value().mask_alpha.size() !=
            static_cast<std::size_t>(applied.value().width) * applied.value().height)
        {
            return make_error(ErrorCode::kInternal, "Preview ROI mask alpha size mismatch",
                              {{"reason", "preview_roi_mask_size_mismatch"}});
        }
        result.mask_alpha.resize(static_cast<std::size_t>(pw) * ph);
        for (std::uint32_t row = 0U; row < ph; ++row)
        {
            const auto src =
                static_cast<std::size_t>(crop_y_px + row) * applied.value().width + crop_x_px;
            const auto dst = static_cast<std::size_t>(row) * pw;
            std::copy_n(applied.value().mask_alpha.begin() + static_cast<std::ptrdiff_t>(src), pw,
                        result.mask_alpha.begin() + static_cast<std::ptrdiff_t>(dst));
        }
    }
    LOG_INFO(ravo::logger(), "preview roi asset={} {}x{} owned={},{} apron={} cfa={},{} {}x{}",
             asset.id, pw, ph, px, py, spatial_apron, cfa.value().x, cfa.value().y,
             cfa.value().width, cfa.value().height);
    result.original_missing = false;
    return result;
}

} // namespace ravo
