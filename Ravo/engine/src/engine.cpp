#include "ravo/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "capability_ops.h"
#include "canvas_frame.h"
#include "dehaze.h"
#include "gpu_adapter.h"
#include "gpu_preview.h"
#include "image_ops.h"
#include "input_color.h"
#include "lut3d.h"
#include "mask_evaluator.h"
#include "output_color.h"
#include "output_dither.h"
#include "parallel_rows.h"
#include "perspective_transform.h"
#include "profile_gamma.h"
#include "primaries.h"
#include "raw_ca.h"
#include "raw_denoise.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "ravo/recipe/color_output.h"
#include "watermark.h"

namespace ravo
{

struct InteractivePreviewRenderCache::Impl
{
    struct PrefixState
    {
        std::string fingerprint;
        std::size_t operation_count = 0U;
        LinearWorkingBuffer buffer;
    };

    std::optional<PrefixState> prefix;
    std::unique_ptr<detail::ParallelRowSession> parallel_rows;
    std::uint64_t generation = 0U;
};

InteractivePreviewRenderCache::InteractivePreviewRenderCache() noexcept = default;
InteractivePreviewRenderCache::~InteractivePreviewRenderCache() = default;
InteractivePreviewRenderCache::InteractivePreviewRenderCache(
    InteractivePreviewRenderCache &&) noexcept = default;
InteractivePreviewRenderCache &
InteractivePreviewRenderCache::operator=(InteractivePreviewRenderCache &&) noexcept = default;

bool InteractivePreviewRenderCache::populated() const noexcept
{
    return impl_ != nullptr && impl_->prefix.has_value();
}

std::uint64_t InteractivePreviewRenderCache::generation() const noexcept
{
    return impl_ != nullptr ? impl_->generation : 0U;
}

Result<std::size_t> validate_profiled_output_for_pack(const ProfiledOutputBuffer &input,
                                                      const std::size_t bytes_per_pixel)
{
    if (bytes_per_pixel != 3U && bytes_per_pixel != 6U && bytes_per_pixel != 12U)
    {
        return make_error(ErrorCode::kValidation, "Profiled output sample width is unsupported",
                          {{"reason", "unsupported_sample_width"},
                           {"bytes_per_pixel", std::to_string(bytes_per_pixel)}});
    }
    if (input.width == 0 || input.height == 0)
    {
        return make_error(ErrorCode::kValidation, "Profiled output dimensions must be non-zero",
                          {{"reason", "invalid_dimensions"},
                           {"width", std::to_string(input.width)},
                           {"height", std::to_string(input.height)}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    const std::size_t maximum_channels =
        bytes_per_pixel == 3U ? std::vector<std::uint8_t>{}.max_size() :
        bytes_per_pixel == 6U ? std::vector<std::uint16_t>{}.max_size() :
                                std::vector<float>{}.max_size();
    if (pixels > std::numeric_limits<std::size_t>::max() / bytes_per_pixel ||
        pixels > static_cast<std::uint64_t>(maximum_channels / 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Profiled output dimensions exceed the export buffer limit",
                          {{"reason", "dimensions_overflow"},
                           {"bytes_per_pixel", std::to_string(bytes_per_pixel)}});
    }
    const std::size_t expected_channels = static_cast<std::size_t>(pixels) * 3U;
    if (input.channels.size() != expected_channels)
    {
        return make_error(ErrorCode::kValidation,
                          "Profiled output channel count does not match its dimensions",
                          {{"reason", "channel_count_mismatch"},
                           {"expected_channels", std::to_string(expected_channels)},
                           {"actual_channels", std::to_string(input.channels.size())}});
    }
    if (input.color_profile.model != ColorModel::kRgb)
    {
        return make_error(
            ErrorCode::kUnsupported, "Profiled output must use an RGB colour model",
            {{"reason", "unsupported_color_model"}, {"profile", input.color_profile.identifier}});
    }
    return expected_channels;
}

namespace
{

std::once_flag g_gpu_once;
std::shared_ptr<GpuAdapter> g_gpu_adapter;
std::optional<TaskError> g_gpu_create_error;

void ensure_gpu_adapter()
{
    std::call_once(g_gpu_once,
                   []()
                   {
                       auto created = GpuAdapter::try_create();
                       if (created)
                       {
                           g_gpu_adapter = std::move(created).value();
                           return;
                       }
                       g_gpu_create_error = created.error();
                   });
}

[[nodiscard]] TaskError gpu_adapter_unavailable_error()
{
    if (g_gpu_create_error.has_value())
    {
        return *g_gpu_create_error;
    }
    return make_error(ErrorCode::kUnsupported, "GPU adapter is unavailable on this host",
                      {{"reason", "gpu_unavailable"}});
}

[[nodiscard]] Result<void> pack_profiled_row(const ProfiledOutputBuffer &input,
                                             const std::uint32_t row,
                                             const RenderSampleKind sample_kind,
                                             RenderedExportImage &result)
{
    const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
    const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
    for (std::size_t index = begin; index < end; ++index)
    {
        const float sample = input.channels[index];
        if (!std::isfinite(sample))
        {
            return make_error(
                ErrorCode::kValidation, "Profiled output contains NaN or infinity",
                {{"reason", "non_finite_sample"}, {"sample_index", std::to_string(index)}});
        }
        switch (sample_kind)
        {
        case RenderSampleKind::kRgb8:
        {
            // Output colour already owns transfer encoding. Preserve the frozen
            // _copy_output arithmetic here while keeping Ravo's RGB byte order.
            const float nonnegative = std::fmax(sample, 0.0F);
            const float rounded = std::round(255.0F * nonnegative);
            std::get<std::vector<std::uint8_t>>(result.samples)[index] =
                static_cast<std::uint8_t>(std::fmin(rounded, 255.0F));
            break;
        }
        case RenderSampleKind::kRgb16:
        {
            const float clamped = std::clamp(sample, 0.0F, 1.0F);
            const float rounded = std::round(clamped * 65535.0F);
            std::get<std::vector<std::uint16_t>>(result.samples)[index] =
                static_cast<std::uint16_t>(rounded);
            break;
        }
        case RenderSampleKind::kRgbFloat:
        {
            std::get<std::vector<float>>(result.samples)[index] = sample;
            break;
        }
        }
    }
    return {};
}

} // namespace

Result<RenderedExportImage> encode_profiled_output(const ProfiledOutputBuffer &input,
                                                   const RenderSampleKind sample_kind,
                                                   const CancellationToken &cancellation)
try
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const std::size_t bytes_per_pixel = render_sample_bytes_per_pixel(sample_kind);
    auto expected = validate_profiled_output_for_pack(input, bytes_per_pixel);
    if (!expected)
    {
        return expected.error();
    }

    RenderedExportImage result;
    result.width = input.width;
    result.height = input.height;
    result.color_profile = input.color_profile;
    switch (sample_kind)
    {
    case RenderSampleKind::kRgb8:
        result.samples = std::vector<std::uint8_t>(expected.value());
        break;
    case RenderSampleKind::kRgb16:
        result.samples = std::vector<std::uint16_t>(expected.value());
        break;
    case RenderSampleKind::kRgbFloat:
        result.samples = std::vector<float>(expected.value());
        break;
    default:
        return make_error(ErrorCode::kValidation, "Profiled output sample kind is unsupported",
                          {{"reason", "unsupported_sample_kind"}});
    }

    std::mutex error_mutex;
    std::uint32_t first_error_row = std::numeric_limits<std::uint32_t>::max();
    std::optional<TaskError> first_error;
    auto packed = detail::for_each_row(input.height, cancellation,
                                       [&](const std::uint32_t row)
                                       {
                                           auto row_result =
                                               pack_profiled_row(input, row, sample_kind, result);
                                           if (!row_result)
                                           {
                                               const std::lock_guard lock(error_mutex);
                                               if (row < first_error_row)
                                               {
                                                   first_error_row = row;
                                                   first_error = row_result.error();
                                               }
                                           }
                                       });
    if (!packed)
    {
        return packed.error();
    }
    if (first_error)
    {
        return std::move(*first_error);
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Final export output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<RenderedImage> encode_profiled_output_rgb8(const ProfiledOutputBuffer &input,
                                                  const CancellationToken &cancellation)
{
    auto packed = encode_profiled_output(input, RenderSampleKind::kRgb8, cancellation);
    if (!packed)
    {
        return packed.error();
    }
    RenderedImage result;
    result.width = packed.value().width;
    result.height = packed.value().height;
    result.color_profile = std::move(packed.value().color_profile);
    result.rgb = std::get<std::vector<std::uint8_t>>(std::move(packed.value().samples));
    return result;
}

EngineFacade::EngineFacade(OperationRegistry registry)
    : registry_(std::move(registry))
{
}

Result<EngineFacade> EngineFacade::create_phase1()
{
    auto registry = make_phase1_registry();
    if (!registry)
    {
        return registry.error();
    }
    return EngineFacade{std::move(registry).value()};
}

std::string_view EngineFacade::gpu_backend() const
{
    ensure_gpu_adapter();
    return g_gpu_adapter != nullptr ? g_gpu_adapter->backend_id() : std::string_view{"unavailable"};
}

EngineFacade::GpuDisplayFrame EngineFacade::gpu_display_frame(const GpuDisplayKind kind) const
{
    ensure_gpu_adapter();
    GpuDisplayFrame out;
    if (g_gpu_adapter == nullptr)
    {
        return out;
    }
    const auto frame = g_gpu_adapter->display_frame(static_cast<std::uint32_t>(kind));
    out.width = frame.width;
    out.height = frame.height;
    out.generation = frame.generation;
    out.backend = std::string(frame.backend);
    out.native_surface = frame.native_surface;
    return out;
}

Result<void> EngineFacade::gpu_copy_rgb(const std::span<const float> input,
                                        const std::span<float> output,
                                        const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    ensure_gpu_adapter();
    if (g_gpu_adapter == nullptr)
    {
        return gpu_adapter_unavailable_error();
    }
    return g_gpu_adapter->copy_rgb(input, output, cancellation);
}

Result<LinearWorkingBuffer>
EngineFacade::gpu_apply_exposure(const LinearWorkingBuffer &working, const ExposureParams &params,
                                 const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    ensure_gpu_adapter();
    if (g_gpu_adapter == nullptr)
    {
        return gpu_adapter_unavailable_error();
    }
    return apply_exposure_gpu(working, params, *g_gpu_adapter, cancellation);
}

Result<InspectionResult> EngineFacade::inspect(const std::string_view input_uri,
                                               const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (input_uri.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Input URI must not be empty for inspection",
                          {{"reason", "empty_raw_path"}});
    }
    return identify_raw(input_uri);
}

Result<std::array<double, 4>>
EngineFacade::sample_white_balance(const DecodedRaw &raw,
                                   const WhiteBalancePickRequest &request) const
{
    return sample_white_balance_coefficients(raw, request);
}

Result<PerspectiveAnalysis>
EngineFacade::analyze_perspective(const RasterBuffer &raster, const PerspectiveAnalysisMode mode,
                                  const CancellationToken &cancellation) const
{
    return analyze_perspective_raster(raster, mode, cancellation);
}

Result<EmbeddedPreview>
EngineFacade::extract_embedded_preview(const std::string_view input_uri,
                                       const std::uint32_t max_edge,
                                       const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (input_uri.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Input URI must not be empty for preview extraction",
                          {{"reason", "empty_raw_path"}});
    }
    return extract_libraw_preview(input_uri, max_edge, cancellation);
}

Result<RawInspectPreview>
EngineFacade::inspect_with_embedded_preview(const std::string_view input_uri,
                                            const std::uint32_t max_edge,
                                            const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (input_uri.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Input URI must not be empty for inspection",
                          {{"reason", "empty_raw_path"}});
    }
    return inspect_raw_with_embedded_preview(input_uri, max_edge, cancellation);
}

Result<EngineCaptureMetadata>
EngineFacade::read_embedded_capture_metadata(const std::string_view input_uri,
                                             const CancellationToken &cancellation) const
{
    return ravo::read_embedded_capture_metadata(input_uri, cancellation);
}

const std::vector<OperationDescriptor> &EngineFacade::operations() const noexcept
{
    return registry_.descriptors();
}

Result<Recipe> EngineFacade::upgrade(Recipe recipe) const
{
    return upgrade_recipe(std::move(recipe));
}

Result<void> EngineFacade::validate(const Recipe &recipe) const
{
    return validate_recipe(recipe, registry_);
}

Result<std::string> EngineFacade::input_color_cache_fingerprint(const Recipe &recipe) const
{
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    return ravo::input_color_cache_fingerprint(recipe);
}

Result<std::string> EngineFacade::output_color_cache_fingerprint(const Recipe &recipe) const
{
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    return ravo::output_color_cache_fingerprint(recipe);
}

Result<Lut3dInspection> EngineFacade::inspect_lut3d(const std::string_view path,
                                                    const CancellationToken &cancellation) const
{
    auto lut = process_lut3d_cache().load(path, cancellation);
    if (!lut)
        return lut.error();
    return Lut3dInspection{lut.value()->canonical_path, lut.value()->fingerprint,
                           lut.value()->title,          lut.value()->size,
                           lut.value()->domain_min,     lut.value()->domain_max};
}

Result<std::string>
EngineFacade::lut3d_cache_fingerprint(const Recipe &recipe,
                                      const CancellationToken &cancellation) const
{
    auto valid = validate(recipe);
    if (!valid)
        return valid.error();
    return lut3d_recipe_cache_fingerprint(recipe, process_lut3d_cache(), cancellation);
}

Result<RenderResult> EngineFacade::render(const RenderRequest &request,
                                          ProgressSink *progress_sink) const
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (request.worker_count == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Render worker count must be at least one");
    }
    if (request.asset.id != request.recipe.asset.id ||
        request.asset.input_uri != request.recipe.asset.input_uri)
    {
        return make_error(ErrorCode::kValidation,
                          "Render request asset does not match the recipe asset");
    }
    auto valid = validate(request.recipe);
    if (!valid)
    {
        return valid.error();
    }
    if (progress_sink != nullptr)
    {
        progress_sink->on_progress({request.correlation_id, "validation_complete", 1, 1});
    }
    if (request.output_uri.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Render output URI must not be empty");
    }
    auto rendered = render_to_image(request, nullptr);
    if (!rendered)
    {
        return rendered.error();
    }
    auto written = write_png_atomically(request.output_uri, rendered.value());
    if (!written)
    {
        return written.error();
    }
    if (progress_sink != nullptr)
    {
        progress_sink->on_progress({request.correlation_id, "output_complete", 1, 1});
    }
    return RenderResult{request.correlation_id, request.output_uri, rendered.value().width,
                        rendered.value().height};
}

Result<DecodedRaw> EngineFacade::decode_raw_frame(const std::string_view input_uri,
                                                  const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (input_uri.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Input URI must not be empty for RAW decode",
                          {{"reason", "empty_raw_path"}});
    }
    auto decoded = decode_raw(input_uri, cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return decoded;
}

Result<LinearWorkingBuffer>
EngineFacade::linear_working_from_raw(const DecodedRaw &raw, const Recipe &recipe,
                                      const std::uint32_t width, const std::uint32_t height,
                                      const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    auto temperature = resolve_raw_temperature(raw, recipe);
    if (!temperature)
    {
        return temperature.error();
    }
    auto exposure_analysis = build_exposure_analysis_context(raw, cancellation);
    if (!exposure_analysis)
    {
        return exposure_analysis.error();
    }
    std::string demosaic_mode(kDemosaicModeRcd);
    bool has_demosaic_mode = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kDemosaicOperationId)
        {
            continue;
        }
        if (has_demosaic_mode)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled RAW demosaic operation",
                              {{"reason", "duplicate_demosaic_operation"}});
        }
        auto selected = demosaic_mode_from_parameters(operation.parameters);
        if (!selected)
        {
            return selected.error();
        }
        demosaic_mode = std::move(selected).value();
        has_demosaic_mode = true;
    }
    const DecodedRaw *source = &raw;
    DecodedRaw prepared;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled ||
            (operation.id != "ravo.raw.hotpixels" && operation.id != "ravo.raw.highlights" &&
             operation.id != "ravo.raw.cacorrect" && operation.id != "ravo.raw.denoise"))
        {
            continue;
        }
        if (source == &raw)
        {
            prepared = raw;
            source = &prepared;
        }
        Result<void> applied{};
        if (operation.id == "ravo.raw.hotpixels")
        {
            applied = apply_raw_hotpixels(prepared, operation, cancellation);
        }
        else if (operation.id == "ravo.raw.highlights")
        {
            applied = apply_raw_highlights(prepared, operation, temperature.value().coefficients,
                                           cancellation);
        }
        else if (operation.id == "ravo.raw.cacorrect")
        {
            applied = apply_raw_cacorrect(prepared, operation, temperature.value().coefficients,
                                          cancellation);
        }
        else
        {
            applied = apply_raw_denoise(prepared, operation, cancellation);
        }
        if (!applied)
        {
            return applied.error();
        }
    }
    if (!has_demosaic_mode && source->cfa_width == 6U && source->cfa_height == 6U)
    {
        demosaic_mode = std::string(kDemosaicModeMarkesteijn3);
    }
    auto demosaiced = working_from_raw(*source, width, height, temperature.value().coefficients,
                                       demosaic_mode, cancellation);
    if (!demosaiced)
    {
        return demosaiced.error();
    }
    auto input_color = resolve_input_color(recipe);
    if (!input_color)
    {
        return input_color.error();
    }
    auto profile_gamma = resolve_profile_gamma(recipe);
    if (!profile_gamma)
    {
        return profile_gamma.error();
    }
    ProfiledColorBuffer profiled;
    profiled.width = demosaiced.value().width;
    profiled.height = demosaiced.value().height;
    profiled.channels = std::move(demosaiced.value().rgb);
    profiled.color_profile = std::move(demosaiced.value().color_profile);
    profiled.canonical_roi_scale = demosaiced.value().canonical_roi_scale;
    profiled.mask_attached_frame = demosaiced.value().mask_attached_frame;
    WorkingImage source_working{profiled.width,
                                profiled.height,
                                std::move(profiled.channels),
                                std::move(profiled.color_profile),
                                nullptr,
                                profiled.canonical_roi_scale,
                                profiled.mask_attached_frame};
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kDehazeOperationId)
        {
            continue;
        }
        auto dehazed = apply_dehaze(source_working, operation, cancellation);
        if (!dehazed)
        {
            return dehazed.error();
        }
        source_working = std::move(dehazed).value();
    }
    profiled.channels = std::move(source_working.rgb);
    profiled.color_profile = std::move(source_working.color_profile);
    if (profile_gamma.value())
    {
        auto corrected = apply_profile_gamma(profiled, *profile_gamma.value(), cancellation);
        if (!corrected)
        {
            return corrected.error();
        }
        profiled = std::move(corrected).value();
    }
    auto working = apply_input_color(profiled, input_color.value(), cancellation);
    if (!working)
    {
        return working.error();
    }
    working.value().exposure_analysis = std::move(exposure_analysis).value();
    return working;
}

Result<LinearWorkingBuffer> EngineFacade::linear_working_from_raw_window(
    const DecodedRaw &raw, const Recipe &recipe, const std::uint32_t origin_x,
    const std::uint32_t origin_y, const std::uint32_t width, const std::uint32_t height,
    const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    auto temperature = resolve_raw_temperature(raw, recipe);
    if (!temperature)
    {
        return temperature.error();
    }
    auto exposure_analysis = build_exposure_analysis_context(raw, cancellation);
    if (!exposure_analysis)
    {
        return exposure_analysis.error();
    }
    std::string demosaic_mode(kDemosaicModeRcd);
    bool has_demosaic_mode = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kDemosaicOperationId)
        {
            continue;
        }
        if (has_demosaic_mode)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one enabled RAW demosaic operation",
                              {{"reason", "duplicate_demosaic_operation"}});
        }
        auto selected = demosaic_mode_from_parameters(operation.parameters);
        if (!selected)
        {
            return selected.error();
        }
        demosaic_mode = std::move(selected).value();
        has_demosaic_mode = true;
    }
    const DecodedRaw *source = &raw;
    DecodedRaw prepared;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled ||
            (operation.id != "ravo.raw.hotpixels" && operation.id != "ravo.raw.highlights" &&
             operation.id != "ravo.raw.cacorrect" && operation.id != "ravo.raw.denoise"))
        {
            continue;
        }
        if (source == &raw)
        {
            prepared = raw;
            source = &prepared;
        }
        Result<void> applied{};
        if (operation.id == "ravo.raw.hotpixels")
        {
            applied = apply_raw_hotpixels(prepared, operation, cancellation);
        }
        else if (operation.id == "ravo.raw.highlights")
        {
            applied = apply_raw_highlights(prepared, operation, temperature.value().coefficients,
                                           cancellation);
        }
        else if (operation.id == "ravo.raw.cacorrect")
        {
            applied = apply_raw_cacorrect(prepared, operation, temperature.value().coefficients,
                                          cancellation);
        }
        else
        {
            applied = apply_raw_denoise(prepared, operation, cancellation);
        }
        if (!applied)
        {
            return applied.error();
        }
    }
    ensure_gpu_adapter();
    // CFA demosaic for ROI windows stays on CPU with RCD tile-grid alignment so
    // owned pixels match full-frame demosaic+crop. Interactive Metal stages still
    // retain linear working later in render_interactive_linear_working.
    auto demosaiced = working_from_raw_window(*source, origin_x, origin_y, width, height,
                                              temperature.value().coefficients, demosaic_mode,
                                              cancellation, nullptr);
    if (!demosaiced)
    {
        return demosaiced.error();
    }
    auto input_color = resolve_input_color(recipe);
    if (!input_color)
    {
        return input_color.error();
    }
    auto profile_gamma = resolve_profile_gamma(recipe);
    if (!profile_gamma)
    {
        return profile_gamma.error();
    }
    ProfiledColorBuffer profiled;
    profiled.width = demosaiced.value().width;
    profiled.height = demosaiced.value().height;
    profiled.channels = std::move(demosaiced.value().rgb);
    profiled.color_profile = std::move(demosaiced.value().color_profile);
    profiled.canonical_roi_scale = demosaiced.value().canonical_roi_scale;
    profiled.mask_attached_frame = demosaiced.value().mask_attached_frame;
    if (profile_gamma.value())
    {
        auto corrected = apply_profile_gamma(profiled, *profile_gamma.value(), cancellation);
        if (!corrected)
        {
            return corrected.error();
        }
        profiled = std::move(corrected).value();
    }
    auto working = apply_input_color(profiled, input_color.value(), cancellation);
    if (!working)
    {
        return working.error();
    }
    working.value().exposure_analysis = std::move(exposure_analysis).value();
    return working;
}

Result<LinearWorkingBuffer>
EngineFacade::linear_working_from_raster(const RasterBuffer &raster, const Recipe &recipe,
                                         const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    auto encoded = working_from_encoded_rgb8(raster);
    if (!encoded)
    {
        return encoded.error();
    }
    auto input_color = resolve_input_color(recipe);
    if (!input_color)
    {
        return input_color.error();
    }
    auto profile_gamma = resolve_profile_gamma(recipe);
    if (!profile_gamma)
    {
        return profile_gamma.error();
    }
    ProfiledColorBuffer profiled;
    profiled.width = encoded.value().width;
    profiled.height = encoded.value().height;
    profiled.channels = std::move(encoded.value().rgb);
    profiled.color_profile = std::move(encoded.value().color_profile);
    profiled.canonical_roi_scale = encoded.value().canonical_roi_scale;
    profiled.mask_attached_frame = encoded.value().mask_attached_frame;
    for (const auto &operation : recipe.operations)
    {
        if (operation.enabled && operation.id == kDehazeOperationId)
        {
            return make_error(
                ErrorCode::kUnsupported, "Dehaze requires a source-linear RAW buffer",
                {{"operation_id", operation.id}, {"reason", "dehaze_raster_source_unsupported"}});
        }
    }
    if (profile_gamma.value())
    {
        auto corrected = apply_profile_gamma(profiled, *profile_gamma.value(), cancellation);
        if (!corrected)
        {
            return corrected.error();
        }
        profiled = std::move(corrected).value();
    }
    return apply_input_color(profiled, input_color.value(), cancellation);
}

Result<LinearWorkingBuffer>
EngineFacade::scale_linear_working(const LinearWorkingBuffer &working, const std::uint32_t width,
                                   const std::uint32_t height, const std::uint32_t original_width,
                                   const std::uint32_t original_height,
                                   const CancellationToken &cancellation) const
{
    return scale_working_image(working, width, height, original_width, original_height,
                               cancellation);
}

namespace
{

[[nodiscard]] Result<ProfiledOutputBuffer> render_recipe_to_profiled_output(
    const LinearWorkingBuffer &working, const Recipe &recipe, const CancellationToken &cancellation,
    const std::optional<std::string> &overlay_mask_id, AlphaPlane *overlay_alpha,
    std::string *gpu_backend = nullptr, const bool need_cpu_pixels = true,
    const std::uint32_t display_slot = 0, const bool prefer_retained_source = false,
    const bool use_gpu = false, const GpuDisplayPublishCrop display_publish_crop = {})
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto output_color = resolve_output_color(recipe);
    if (!output_color)
    {
        return output_color.error();
    }
    WorkingImage image = working;
    Recipe remaining_recipe = recipe;
    for (auto &operation : remaining_recipe.operations)
    {
        if (!operation.enabled || operation.id != kPrimariesOperationId)
        {
            continue;
        }
        auto transformed = apply_primaries(image, operation, cancellation);
        if (!transformed)
        {
            return transformed.error();
        }
        image = std::move(transformed).value();
        operation.enabled = false;
    }
    if (image.color_profile.identifier != kInputProfileLinearRec709)
    {
        auto operation_working =
            convert_working_profile(image, kInputProfileLinearRec709, cancellation);
        if (!operation_working)
        {
            return operation_working.error();
        }
        image = std::move(operation_working).value();
    }
    std::optional<AlphaPlane> pending_overlay;
    if (overlay_alpha != nullptr && overlay_mask_id.has_value() && !overlay_mask_id->empty())
    {
        Recipe overlay_recipe = remaining_recipe;
        std::size_t replay_begin = 0U;
        for (std::size_t index = 0U; index < overlay_recipe.operations.size(); ++index)
        {
            if (overlay_recipe.operations[index].enabled &&
                overlay_recipe.operations[index].id == kCanvasOperationId)
                replay_begin = index + 1U;
        }
        for (std::size_t index = replay_begin; index < overlay_recipe.operations.size(); ++index)
        {
            auto &operation = overlay_recipe.operations[index];
            if (operation.id == "ravo.geometry.rotate" || operation.id == "ravo.geometry.flip" ||
                operation.id == "ravo.geometry.crop" ||
                operation.id == "ravo.geometry.straighten" ||
                operation.id == kPerspectiveOperationId)
                operation.enabled = false;
        }
        auto overlay_basis = apply_recipe_ops(image, overlay_recipe, cancellation);
        if (!overlay_basis)
            return overlay_basis.error();
        if (overlay_basis.value().width == 0U || overlay_basis.value().height == 0U ||
            overlay_basis.value().width > std::numeric_limits<std::uint32_t>::max() / 3U)
        {
            return make_error(ErrorCode::kValidation, "Overlay working image is invalid",
                              {{"reason", "invalid_mask_overlay"}});
        }
        const std::uint32_t stride = overlay_basis.value().width * 3U;
        MaskEvaluationRequest request{
            .full_width = overlay_basis.value().width,
            .full_height = overlay_basis.value().height,
            .roi_x = 0U,
            .roi_y = 0U,
            .roi_width = overlay_basis.value().width,
            .roi_height = overlay_basis.value().height,
            .input = MaskRgbPlaneView{overlay_basis.value().rgb, stride},
            .operation_output = MaskRgbPlaneView{overlay_basis.value().rgb, stride},
            .attached_frame = overlay_basis.value().mask_attached_frame,
            .cancellation = cancellation,
        };
        auto alpha = evaluate_canonical_mask(recipe.masks, *overlay_mask_id, request);
        if (!alpha)
            return alpha.error();
        auto transformed = apply_recipe_geometry_to_alpha(std::move(alpha).value(),
                                                          remaining_recipe, cancellation);
        if (!transformed)
            return transformed.error();
        pending_overlay = std::move(transformed).value();
    }
    const GpuAdapter *gpu = nullptr;
    if (use_gpu)
    {
        ensure_gpu_adapter();
        gpu = g_gpu_adapter.get();
    }
    const GpuDisplayPublishCrop gpu_publish_crop{display_publish_crop.x, display_publish_crop.y,
                                                 display_publish_crop.width,
                                                 display_publish_crop.height};
    Result<WorkingImage> adjusted =
        overlay_alpha == nullptr ?
            apply_preview_rgb(std::move(image), remaining_recipe, gpu, gpu_backend, cancellation,
                              need_cpu_pixels, display_slot, prefer_retained_source,
                              gpu_publish_crop) :
            apply_recipe_ops(std::move(image), remaining_recipe, cancellation);
    if (!adjusted)
    {
        return adjusted.error();
    }
    if (overlay_alpha != nullptr && gpu_backend != nullptr)
    {
        gpu_backend->clear();
    }
    if (pending_overlay.has_value() && (pending_overlay->width != adjusted.value().width ||
                                        pending_overlay->height != adjusted.value().height))
    {
        return make_error(ErrorCode::kInternal,
                          "Mask overlay geometry does not match rendered dimensions",
                          {{"reason", "mask_overlay_geometry_mismatch"},
                           {"overlay_width", std::to_string(pending_overlay->width)},
                           {"overlay_height", std::to_string(pending_overlay->height)},
                           {"render_width", std::to_string(adjusted.value().width)},
                           {"render_height", std::to_string(adjusted.value().height)}});
    }
    if (!need_cpu_pixels && adjusted.value().rgb.empty())
    {
        ProfiledOutputBuffer skipped;
        skipped.width = adjusted.value().width;
        skipped.height = adjusted.value().height;
        skipped.color_profile = std::move(adjusted.value().color_profile);
        if (pending_overlay.has_value())
            *overlay_alpha = std::move(*pending_overlay);
        return skipped;
    }
    auto encoded = apply_output_color(adjusted.value(), output_color.value(), cancellation);
    if (!encoded)
        return encoded.error();
    if (pending_overlay.has_value())
        *overlay_alpha = std::move(*pending_overlay);
    return encoded;
}

[[nodiscard]] Recipe rgb_recipe_after_raw_preprocess(Recipe recipe)
{
    for (auto &operation : recipe.operations)
    {
        if (operation.id == "ravo.color.temperature" || operation.id == "ravo.raw.hotpixels" ||
            operation.id == "ravo.raw.highlights" || operation.id == "ravo.raw.cacorrect" ||
            operation.id == "ravo.raw.denoise" || operation.id == kProfileGammaOperationId ||
            operation.id == kDehazeOperationId)
        {
            operation.enabled = false;
        }
    }
    return recipe;
}

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_recipe_output_dither(ProfiledOutputBuffer output, const Recipe &recipe,
                           const OutputDitherTarget target, const CancellationToken &cancellation)
{
    const OperationInstance *selected = nullptr;
    for (const auto &operation : recipe.operations)
    {
        if (operation.id != kOutputDitherOperationId)
            continue;
        if (selected != nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Recipe contains duplicate Output Dither operations",
                              {{"operation_id", std::string(kOutputDitherOperationId)},
                               {"reason", "duplicate_output_dither"}});
        }
        selected = &operation;
    }
    if (selected == nullptr || !selected->enabled)
        return output;
    return apply_output_dither(std::move(output), *selected, target, cancellation);
}

[[nodiscard]] Result<ProfiledOutputBuffer> apply_recipe_frame(ProfiledOutputBuffer output,
                                                              const Recipe &recipe,
                                                              const CancellationToken &cancellation,
                                                              AlphaPlane *overlay = nullptr)
{
    const OperationInstance *selected = nullptr;
    for (const auto &operation : recipe.operations)
    {
        if (operation.id != kFrameOperationId)
            continue;
        if (selected != nullptr)
            return make_error(ErrorCode::kValidation, "Recipe contains duplicate Frame operations",
                              {{"reason", "duplicate_output_frame"}});
        selected = &operation;
    }
    if (selected == nullptr || !selected->enabled)
        return output;
    auto params = frame_from_parameters(selected->parameters);
    if (!params)
        return params.error();
    auto layout = compute_frame_layout(output.width, output.height, params.value());
    if (!layout)
        return layout.error();
    if (overlay != nullptr && overlay->width == output.width && overlay->height == output.height)
    {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(layout.value().output_width) * layout.value().output_height;
        if (pixels > std::vector<float>{}.max_size())
            return make_error(ErrorCode::kValidation, "Framed mask overlay is too large");
        std::vector<float> framed(static_cast<std::size_t>(pixels), 0.0F);
        for (std::uint32_t row = 0U; row < overlay->height; ++row)
        {
            auto active = cancellation.check();
            if (!active)
                return active.error();
            std::copy_n(
                overlay->alpha.begin() +
                    static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * overlay->width),
                overlay->width,
                framed.begin() + static_cast<std::ptrdiff_t>(
                                     static_cast<std::size_t>(layout.value().image_y + row) *
                                         layout.value().output_width +
                                     layout.value().image_x));
        }
        overlay->width = layout.value().output_width;
        overlay->height = layout.value().output_height;
        overlay->alpha = std::move(framed);
    }
    return apply_frame(std::move(output), *selected, cancellation);
}

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_recipe_watermark(ProfiledOutputBuffer output, const Recipe &recipe,
                       const CancellationToken &cancellation)
{
    const OperationInstance *selected = nullptr;
    for (const auto &operation : recipe.operations)
    {
        if (operation.id != kWatermarkOperationId)
            continue;
        if (selected != nullptr)
            return make_error(ErrorCode::kValidation,
                              "Recipe contains duplicate Watermark operations",
                              {{"reason", "duplicate_watermark"}});
        selected = &operation;
    }
    if (selected == nullptr || !selected->enabled)
        return output;
    return apply_watermark(std::move(output), *selected, recipe.asset, cancellation);
}

[[nodiscard]] bool is_interactive_prefix_operation(const std::string_view id) noexcept
{
    return id == "ravo.color.temperature" || id == kProfileGammaOperationId ||
           id == "ravo.color.input" || id == kPrimariesOperationId ||
           id == "ravo.color.channelmixerrgb" || id == "ravo.raw.hotpixels" ||
           id == "ravo.raw.highlights" || id == "ravo.raw.cacorrect" || id == "ravo.raw.denoise" ||
           id == "ravo.detail.denoiseprofile" || id == "ravo.geometry.lens" ||
           id == kCanvasOperationId;
}

[[nodiscard]] std::size_t interactive_prefix_operation_count(const Recipe &recipe) noexcept
{
    std::size_t count = 0U;
    while (count < recipe.operations.size() &&
           is_interactive_prefix_operation(recipe.operations[count].id))
    {
        ++count;
    }
    // The ordinary render contract extracts every enabled Primaries instance before bridging.
    // A non-canonical late Primaries operation therefore cannot be split without changing order.
    for (std::size_t index = count; index < recipe.operations.size(); ++index)
    {
        if (recipe.operations[index].enabled &&
            recipe.operations[index].id == kPrimariesOperationId)
        {
            return 0U;
        }
    }
    return count;
}

[[nodiscard]] Result<std::string> interactive_prefix_fingerprint(const Recipe &recipe,
                                                                 const std::size_t count)
{
    Recipe prefix_recipe = recipe;
    prefix_recipe.operations.resize(count);
    return serialize_recipe(prefix_recipe);
}

[[nodiscard]] Result<LinearWorkingBuffer>
render_interactive_prefix(const LinearWorkingBuffer &working, const Recipe &recipe,
                          const std::size_t count, const CancellationToken &cancellation)
{
    WorkingImage image = working;
    Recipe prefix_recipe = recipe;
    prefix_recipe.operations.resize(count);
    for (auto &operation : prefix_recipe.operations)
    {
        if (!operation.enabled || operation.id != kPrimariesOperationId)
        {
            continue;
        }
        auto transformed = apply_primaries(image, operation, cancellation);
        if (!transformed)
        {
            return transformed.error();
        }
        image = std::move(transformed).value();
        operation.enabled = false;
    }
    if (image.color_profile.identifier != kInputProfileLinearRec709)
    {
        auto operation_working =
            convert_working_profile(image, kInputProfileLinearRec709, cancellation);
        if (!operation_working)
        {
            return operation_working.error();
        }
        image = std::move(operation_working).value();
    }
    return apply_recipe_ops(std::move(image), prefix_recipe, cancellation);
}

[[nodiscard]] std::string gpu_retained_key(const Recipe &recipe, const std::string_view fingerprint,
                                           const std::uint32_t width, const std::uint32_t height)
{
    std::string key;
    key.reserve(recipe.asset.id.size() + fingerprint.size() + 24U);
    key.append(recipe.asset.id);
    key.push_back('|');
    key.append(fingerprint);
    key.push_back('|');
    key.append(std::to_string(width));
    key.push_back('x');
    key.append(std::to_string(height));
    return key;
}

[[nodiscard]] Result<RenderedImage> render_validated_preview(
    const LinearWorkingBuffer &working, const Recipe &recipe, const CancellationToken &cancellation,
    const std::optional<std::string> &overlay_mask_id, const bool need_cpu_pixels = true,
    const EngineFacade::GpuDisplayKind display_kind = EngineFacade::GpuDisplayKind::kPreview,
    const bool prefer_retained_source = false, const bool use_gpu = false,
    const GpuDisplayPublishCrop display_publish_crop = {})
{
    AlphaPlane overlay;
    std::string gpu_backend;
    const bool force_cpu_pixels = need_cpu_pixels || overlay_mask_id.has_value();
    const auto display_slot = static_cast<std::uint32_t>(display_kind);
    auto output = render_recipe_to_profiled_output(
        working, recipe, cancellation, overlay_mask_id, overlay_mask_id ? &overlay : nullptr,
        overlay_mask_id ? nullptr : &gpu_backend, force_cpu_pixels, display_slot,
        prefer_retained_source && !overlay_mask_id.has_value(), use_gpu, display_publish_crop);
    if (!output)
    {
        return output.error();
    }
    if (!force_cpu_pixels && output.value().channels.empty())
    {
        if (g_gpu_adapter == nullptr)
        {
            return make_error(ErrorCode::kIo, "GPU display skipped CPU pixels without an adapter",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        const auto frame = g_gpu_adapter->display_frame(display_slot);
        if (frame.generation == 0U || frame.native_surface == 0U || frame.width == 0U ||
            frame.height == 0U)
        {
            return make_error(ErrorCode::kIo, "GPU display frame is missing",
                              {{"reason", "gpu_pipeline_failed"}});
        }
        RenderedImage result;
        result.width = output.value().width;
        result.height = output.value().height;
        result.color_profile = std::move(output.value().color_profile);
        result.gpu_backend = std::move(gpu_backend);
        result.gpu_display_generation = frame.generation;
        return result;
    }
    auto dithered = apply_recipe_output_dither(std::move(output).value(), recipe,
                                               OutputDitherTarget::kPreviewRgb8, cancellation);
    if (!dithered)
    {
        return dithered.error();
    }
    auto framed = apply_recipe_frame(std::move(dithered).value(), recipe, cancellation, &overlay);
    if (!framed)
    {
        return framed.error();
    }
    auto watermarked = apply_recipe_watermark(std::move(framed).value(), recipe, cancellation);
    if (!watermarked)
    {
        return watermarked.error();
    }
    auto packed =
        encode_profiled_output(watermarked.value(), RenderSampleKind::kRgb8, cancellation);
    if (!packed)
    {
        return packed.error();
    }
    RenderedImage result;
    result.width = packed.value().width;
    result.height = packed.value().height;
    result.color_profile = std::move(packed.value().color_profile);
    result.rgb = std::get<std::vector<std::uint8_t>>(std::move(packed.value().samples));
    result.gpu_backend = std::move(gpu_backend);
    // Metal may publish a display surface even when CPU RGB was also downloaded.
    if (!result.gpu_backend.empty() && g_gpu_adapter != nullptr)
    {
        const auto frame = g_gpu_adapter->display_frame(display_slot);
        if (frame.generation != 0U && frame.native_surface != 0U && frame.width != 0U &&
            frame.height != 0U)
        {
            result.gpu_display_generation = frame.generation;
        }
    }
    if (overlay_mask_id && overlay.width == result.width && overlay.height == result.height)
    {
        result.mask_alpha = std::move(overlay.alpha);
    }
    return result;
}

} // namespace

Result<void> EngineFacade::composite_preview_mask_overlay(
    std::vector<std::uint8_t> &rgb, const std::uint32_t width, const std::uint32_t height,
    const std::vector<float> &alpha, const CancellationToken &cancellation) const
{
    AlphaPlane plane;
    plane.width = width;
    plane.height = height;
    plane.alpha = alpha;
    return composite_mask_overlay_rgb8(rgb, plane, cancellation);
}

Result<RenderedImage>
EngineFacade::render_linear_working(const LinearWorkingBuffer &working, const Recipe &recipe,
                                    const CancellationToken &cancellation,
                                    std::optional<std::string> overlay_mask_id) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto parallel_rows = detail::ScopedParallelRowSession::create();
    if (!parallel_rows)
    {
        return parallel_rows.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    return render_validated_preview(working, recipe, cancellation, overlay_mask_id);
}

Result<RenderedImage> EngineFacade::render_interactive_linear_working(
    const LinearWorkingBuffer &working, const Recipe &recipe, InteractivePreviewRenderCache &cache,
    const CancellationToken &cancellation, std::optional<std::string> overlay_mask_id,
    const bool need_cpu_pixels, const GpuDisplayKind display_kind,
    const GpuDisplayPublishCrop display_publish_crop) const
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    if (cache.impl_ == nullptr)
    {
        cache.impl_ = std::make_unique<InteractivePreviewRenderCache::Impl>();
    }
    if (cache.impl_->parallel_rows == nullptr)
    {
        auto created = detail::ParallelRowSession::create();
        if (!created)
        {
            return created.error();
        }
        cache.impl_->parallel_rows = std::move(created).value();
    }
    auto parallel_rows = detail::ScopedParallelRowSession::borrow(*cache.impl_->parallel_rows);
    const std::size_t prefix_count = interactive_prefix_operation_count(recipe);
    const auto retain_working = [&](const LinearWorkingBuffer &buffer,
                                    const std::string_view fingerprint) -> Result<void>
    {
        ensure_gpu_adapter();
        if (g_gpu_adapter == nullptr || overlay_mask_id.has_value() || buffer.rgb.empty())
        {
            return {};
        }
        const auto key = gpu_retained_key(recipe, fingerprint, buffer.width, buffer.height);
        if (g_gpu_adapter->has_retained_source(buffer.width, buffer.height) &&
            g_gpu_adapter->retained_source_key() == key)
        {
            return {};
        }
        return g_gpu_adapter->retain_source_rgb(buffer.rgb, buffer.width, buffer.height,
                                                cancellation, key);
    };
    if (prefix_count == 0U)
    {
        auto retained = retain_working(working, "noprefix");
        if (!retained)
        {
            return retained.error();
        }
        return render_validated_preview(working, recipe, cancellation, overlay_mask_id,
                                        need_cpu_pixels, display_kind, g_gpu_adapter != nullptr,
                                        true, display_publish_crop);
    }
    auto fingerprint = interactive_prefix_fingerprint(recipe, prefix_count);
    if (!fingerprint)
    {
        return fingerprint.error();
    }
    const bool prefix_hit = cache.impl_->prefix.has_value() &&
                            cache.impl_->prefix->operation_count == prefix_count &&
                            cache.impl_->prefix->fingerprint == fingerprint.value();
    if (!prefix_hit)
    {
        auto prefix = render_interactive_prefix(working, recipe, prefix_count, cancellation);
        if (!prefix)
        {
            return prefix.error();
        }
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        InteractivePreviewRenderCache::Impl::PrefixState published;
        published.fingerprint = std::move(fingerprint).value();
        published.operation_count = prefix_count;
        published.buffer = std::move(prefix).value();
        cache.impl_->prefix = std::move(published);
        ++cache.impl_->generation;
    }
    Recipe remaining = recipe;
    for (std::size_t index = 0U; index < prefix_count; ++index)
    {
        remaining.operations[index].enabled = false;
    }
    auto retained = retain_working(cache.impl_->prefix->buffer, cache.impl_->prefix->fingerprint);
    if (!retained)
    {
        return retained.error();
    }
    return render_validated_preview(cache.impl_->prefix->buffer, remaining, cancellation,
                                    overlay_mask_id, need_cpu_pixels, display_kind,
                                    g_gpu_adapter != nullptr, true, display_publish_crop);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Interactive working render allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<RenderedExportImage>
EngineFacade::render_linear_working_export(const LinearWorkingBuffer &working, const Recipe &recipe,
                                           const RenderSampleKind sample_kind,
                                           const CancellationToken &cancellation) const
try
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate(recipe);
    if (!valid)
    {
        return valid.error();
    }
    if (render_sample_bytes_per_pixel(sample_kind) == 0U)
    {
        return make_error(ErrorCode::kValidation, "Render sample kind is unsupported",
                          {{"reason", "unsupported_sample_kind"}});
    }
    auto output =
        render_recipe_to_profiled_output(working, recipe, cancellation, std::nullopt, nullptr);
    if (!output)
    {
        return output.error();
    }
    const auto target =
        sample_kind == RenderSampleKind::kRgb8  ? OutputDitherTarget::kExportRgb8 :
        sample_kind == RenderSampleKind::kRgb16 ? OutputDitherTarget::kExportRgb16 :
                                                  OutputDitherTarget::kExportRgbFloat;
    auto dithered =
        apply_recipe_output_dither(std::move(output).value(), recipe, target, cancellation);
    if (!dithered)
    {
        return dithered.error();
    }
    auto framed = apply_recipe_frame(std::move(dithered).value(), recipe, cancellation);
    if (!framed)
        return framed.error();
    auto watermarked = apply_recipe_watermark(std::move(framed).value(), recipe, cancellation);
    if (!watermarked)
        return watermarked.error();
    return encode_profiled_output(watermarked.value(), sample_kind, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Working render allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<RenderedImage> EngineFacade::render_to_image(const RenderRequest &request,
                                                    const RasterBuffer *raster) const
{
    auto packed = render_to_export_image(request, RenderSampleKind::kRgb8, raster);
    if (!packed)
    {
        return packed.error();
    }
    RenderedImage result;
    result.width = packed.value().width;
    result.height = packed.value().height;
    result.color_profile = std::move(packed.value().color_profile);
    result.rgb = std::get<std::vector<std::uint8_t>>(std::move(packed.value().samples));
    return result;
}

Result<RenderedExportImage> EngineFacade::render_to_export_image(const RenderRequest &request,
                                                                 const RenderSampleKind sample_kind,
                                                                 const RasterBuffer *raster) const
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate(request.recipe);
    if (!valid)
    {
        return valid.error();
    }
    const std::size_t bytes_per_pixel = render_sample_bytes_per_pixel(sample_kind);
    if (bytes_per_pixel == 0U)
    {
        return make_error(ErrorCode::kValidation, "Render sample kind is unsupported",
                          {{"reason", "unsupported_sample_kind"}});
    }
    if (raster != nullptr)
    {
        auto working = linear_working_from_raster(*raster, request.recipe, request.cancellation);
        if (!working)
        {
            return working.error();
        }
        return render_linear_working_export(working.value(), request.recipe, sample_kind,
                                            request.cancellation);
    }
    auto decoded = decode_raw_frame(request.asset.input_uri, request.cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    std::uint32_t default_width = decoded.value().width;
    std::uint32_t default_height = decoded.value().height;
    apply_display_rotation_to_size(default_width, default_height, decoded.value().rotate_quarters);
    const std::uint32_t width = request.output_width.value_or(default_width);
    const std::uint32_t height = request.output_height.value_or(default_height);
    const std::uint64_t working_bytes =
        estimate_raw_render_memory(decoded.value(), request.recipe, width, height, bytes_per_pixel);
    if (request.memory_budget_bytes != 0 && working_bytes > request.memory_budget_bytes)
    {
        return make_error(ErrorCode::kValidation, "Render memory budget is too small",
                          {{"required_bytes", std::to_string(working_bytes)}});
    }
    auto working = linear_working_from_raw(decoded.value(), request.recipe, width, height,
                                           request.cancellation);
    if (!working)
    {
        return working.error();
    }
    return render_linear_working_export(working.value(),
                                        rgb_recipe_after_raw_preprocess(request.recipe),
                                        sample_kind, request.cancellation);
}

Result<std::vector<std::uint8_t>> EngineFacade::encode_png(const RenderedImage &image) const
{
    return encode_png_bytes(image);
}

Result<std::vector<std::uint8_t>> EngineFacade::encode_preview_png(const RenderedImage &image) const
{
    return encode_png_bytes(image, true);
}

} // namespace ravo
