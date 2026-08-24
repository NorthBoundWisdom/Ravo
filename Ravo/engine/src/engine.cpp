#include "ravo/engine/engine.h"

#include <utility>

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
    return extract_libraw_preview(input_uri, cancellation);
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
        auto working = working_from_srgb8(*raster);
        if (!working)
        {
            return working.error();
        }
        auto adjusted =
            apply_recipe_ops(std::move(working).value(), request.recipe, request.cancellation);
        if (!adjusted)
        {
            return adjusted.error();
        }
        return encode_working_srgb(adjusted.value());
    }
    auto decoded = decode_raw(request.asset.input_uri);
    if (!decoded)
    {
        return decoded.error();
    }
    return render_raw(decoded.value(), request);
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
