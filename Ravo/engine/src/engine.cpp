#include "ravo/engine/engine.h"

#include <utility>

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
    auto decoded = decode_raw(request.asset.input_uri);
    if (!decoded)
    {
        return decoded.error();
    }
    if (progress_sink != nullptr)
    {
        progress_sink->on_progress({request.correlation_id, "decode_complete", 1, 1});
    }
    auto rendered = render_raw(decoded.value(), request);
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

} // namespace ravo
