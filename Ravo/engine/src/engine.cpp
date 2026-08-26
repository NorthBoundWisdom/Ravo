#include "ravo/engine/engine.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "capability_ops.h"
#include "image_ops.h"
#include "input_color.h"
#include "output_color.h"
#include "profile_gamma.h"
#include "primaries.h"
#include "raw_ca.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "ravo/recipe/color_output.h"

namespace ravo
{

Result<RenderedImage> encode_profiled_output_rgb8(const ProfiledOutputBuffer &input,
                                                  const CancellationToken &cancellation)
try
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (input.width == 0 || input.height == 0)
    {
        return make_error(ErrorCode::kValidation, "Profiled output dimensions must be non-zero",
                          {{"reason", "invalid_dimensions"},
                           {"width", std::to_string(input.width)},
                           {"height", std::to_string(input.height)}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U)
    {
        return make_error(ErrorCode::kValidation,
                          "Profiled output dimensions exceed the RGB8 buffer limit",
                          {{"reason", "dimensions_overflow"}});
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

    RenderedImage result;
    result.width = input.width;
    result.height = input.height;
    result.color_profile = input.color_profile;
    result.rgb.resize(expected_channels);
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
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
            // Output colour already owns transfer encoding. Preserve the frozen
            // _copy_output arithmetic here while keeping Ravo's RGB byte order.
            const float nonnegative = std::fmax(sample, 0.0F);
            const float rounded = std::round(255.0F * nonnegative);
            result.rgb[index] = static_cast<std::uint8_t>(std::fmin(rounded, 255.0F));
        }
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Final RGB8 output allocation failed",
                      {{"reason", "allocation_failed"}});
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
        return make_error(ErrorCode::kInvalidArgument,
                          "Input URI must not be empty for inspection");
    }
    return identify_raw(input_uri);
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
                          "Input URI must not be empty for preview extraction");
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
        return make_error(ErrorCode::kInvalidArgument,
                          "Input URI must not be empty for inspection");
    }
    return inspect_raw_with_embedded_preview(input_uri, max_edge, cancellation);
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
        return make_error(ErrorCode::kInvalidArgument,
                          "Input URI must not be empty for RAW decode");
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
    const DecodedRaw *source = &raw;
    DecodedRaw prepared;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled ||
            (operation.id != "ravo.raw.hotpixels" && operation.id != "ravo.raw.highlights" &&
             operation.id != "ravo.raw.cacorrect"))
        {
            continue;
        }
        if (source == &raw)
        {
            prepared = raw;
            source = &prepared;
        }
        Result<void> applied =
            operation.id == "ravo.raw.hotpixels" ?
                apply_raw_hotpixels(prepared, operation, cancellation) :
            operation.id == "ravo.raw.highlights" ?
                apply_raw_highlights(prepared, operation, cancellation) :
                apply_raw_cacorrect(prepared, operation, temperature.value().coefficients,
                                    cancellation);
        if (!applied)
        {
            return applied.error();
        }
    }
    auto demosaiced =
        working_from_raw(*source, width, height, temperature.value().coefficients, cancellation);
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

Result<RenderedImage>
EngineFacade::render_linear_working(const LinearWorkingBuffer &working, const Recipe &recipe,
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
    auto adjusted = apply_recipe_ops(std::move(image), remaining_recipe, cancellation);
    if (!adjusted)
    {
        return adjusted.error();
    }
    auto output = apply_output_color(adjusted.value(), output_color.value(), cancellation);
    if (!output)
    {
        return output.error();
    }
    return encode_profiled_output_rgb8(output.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Working render allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<RenderedImage> EngineFacade::render_to_image(const RenderRequest &request,
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
    if (raster != nullptr)
    {
        auto working = linear_working_from_raster(*raster, request.recipe, request.cancellation);
        if (!working)
        {
            return working.error();
        }
        return render_linear_working(working.value(), request.recipe, request.cancellation);
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
        estimate_raw_render_memory(decoded.value(), request.recipe, width, height);
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
    Recipe rgb_recipe = request.recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.color.temperature" || operation.id == "ravo.raw.hotpixels" ||
            operation.id == "ravo.raw.highlights" || operation.id == "ravo.raw.cacorrect" ||
            operation.id == kProfileGammaOperationId)
        {
            operation.enabled = false;
        }
    }
    return render_linear_working(working.value(), rgb_recipe, request.cancellation);
}

Result<std::vector<std::uint8_t>> EngineFacade::encode_png(const RenderedImage &image) const
{
    return encode_png_bytes(image);
}

} // namespace ravo
