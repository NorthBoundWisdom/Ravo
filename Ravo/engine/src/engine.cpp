#include "ravo/engine/engine.h"

#include <string>
#include <utility>

#include "capability_ops.h"
#include "image_ops.h"
#include "raw_pipeline.h"

namespace ravo
{

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
    auto decoded = decode_raw(input_uri);
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
    const DecodedRaw *source = &raw;
    DecodedRaw highlighted;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != "ravo.raw.highlights")
        {
            continue;
        }
        highlighted = raw;
        auto reconstructed = apply_raw_highlights(highlighted, operation, cancellation);
        if (!reconstructed)
        {
            return reconstructed.error();
        }
        source = &highlighted;
        break;
    }
    return working_from_raw(*source, width, height, cancellation);
}

Result<LinearWorkingBuffer>
EngineFacade::linear_working_from_raster(const RasterBuffer &raster) const
{
    return working_from_srgb8(raster);
}

Result<RenderedImage>
EngineFacade::render_linear_working(const LinearWorkingBuffer &working, const Recipe &recipe,
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
    WorkingImage image = working;
    auto adjusted = apply_recipe_ops(std::move(image), recipe, cancellation);
    if (!adjusted)
    {
        return adjusted.error();
    }
    return encode_working_srgb(adjusted.value());
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
        auto working = linear_working_from_raster(*raster);
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
    const std::uint64_t output_bytes = static_cast<std::uint64_t>(width) * height * 3U;
    const std::uint64_t working_bytes =
        output_bytes +
        static_cast<std::uint64_t>(decoded.value().pixels.size()) * sizeof(std::uint16_t);
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
        if (operation.id == "ravo.raw.highlights")
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

Result<RasterBuffer> EngineFacade::decode_png(const std::vector<std::uint8_t> &bytes) const
{
    return decode_png_bytes(bytes);
}

} // namespace ravo
