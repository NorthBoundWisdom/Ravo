#include "application_internal.h"

#include <filesystem>
#include <string>
#include <utility>
#include <variant>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/catalog_service.h"
#include "ravo/foundation/color.h"
#include "ravo/services/image_artifact_verification.h"

namespace ravo::cli_internal
{

Result<void> validate_catalog_probe_flags(const CatalogCliArguments &flags)
{
    // Command-local acceptance for probe. Shared catalog dispatch still rejects
    // probe-only flags on other subcommands.
    if (flags.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "catalog probe requires --asset-id");
    }
    if (!flags.output.empty() && !ends_with_png(flags.output))
    {
        return make_error(ErrorCode::kInvalidArgument, "catalog probe --output must be a .png path",
                          {{"path", std::string(flags.output)}});
    }
    return {};
}

Result<JsonValue> run_catalog_probe_command(const EngineFacade &engine, CatalogService &service,
                                            const CatalogCliArguments &flags)
{
    auto validated = validate_catalog_probe_flags(flags);
    if (!validated)
        return validated.error();
    if (!flags.output.empty())
    {
        if (std::filesystem::exists(std::filesystem::path(std::string(flags.output))))
        {
            return make_error(ErrorCode::kConflict, "Output path already exists",
                              {{"path", std::string(flags.output)}});
        }
    }
    auto stored_before = service.develop().load_recipe(flags.asset_id);
    if (!stored_before)
    {
        return stored_before.error();
    }
    auto serialized_before = serialize_recipe(stored_before.value());
    if (!serialized_before)
    {
        return serialized_before.error();
    }
    auto previews_before = service.library().list_previews();
    if (!previews_before)
    {
        return previews_before.error();
    }
    auto source =
        flags.baseline ? service.develop().load_baseline_recipe(flags.asset_id) : stored_before;
    if (!source)
    {
        return source.error();
    }
    auto params = develop_from_recipe(source.value());
    if (!params)
    {
        return params.error();
    }
    auto applied = apply_develop_overrides(params.value(), flags);
    if (!applied)
    {
        return applied.error();
    }

    PreviewRequest request;
    request.asset_id = std::string(flags.asset_id);
    request.max_edge = flags.max_edge.value_or(512U);
    request.prefer_embedded_preview = false;
    request.persist_preview_record = false;
    auto previewed = service.preview().request_preview(request, params.value());
    if (!previewed)
    {
        return previewed.error();
    }
    if (!previewed.value().cache_path.empty() || previewed.value().rgb.empty())
    {
        return make_error(ErrorCode::kIo,
                          "Develop probe did not return a non-persistent memory preview");
    }
    auto statistics = probe_statistics_json(previewed.value());
    if (!statistics)
    {
        return statistics.error();
    }
    auto stored_after = service.develop().load_recipe(flags.asset_id);
    if (!stored_after)
    {
        return stored_after.error();
    }
    auto serialized_after = serialize_recipe(stored_after.value());
    if (!serialized_after)
    {
        return serialized_after.error();
    }
    if (serialized_before.value() != serialized_after.value())
    {
        return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed the recipe");
    }
    auto previews_after = service.library().list_previews();
    if (!previews_after)
    {
        return previews_after.error();
    }
    if (previews_before.value() != previews_after.value())
    {
        return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed preview records");
    }

    JsonValue::Object overrides;
    for (const auto &item : applied.value())
    {
        if (const auto *number = std::get_if<double>(&item.value); number != nullptr)
            overrides.emplace(item.name, JsonValue::number(std::to_string(*number)));
        else
            overrides.emplace(item.name, JsonValue{std::get<std::string>(item.value)});
    }
    JsonValue::Object payload{
        {"asset_id", previewed.value().asset_id},
        {"baseline", flags.baseline},
        {"color_profile", previewed.value().color_profile.identifier},
        {"height", JsonValue::number(std::to_string(previewed.value().height))},
        {"iq_consistency", iq_consistency_policy_json()},
        {"original_missing", previewed.value().original_missing},
        {"media_state", previewed.value().media_state},
        {"pixel_provenance", previewed.value().pixel_provenance},
        {"preview_apply_mode", previewed.value().preview_apply_mode},
        {"overrides", std::move(overrides)},
        {"preview_records_unchanged", true},
        {"recipe_unchanged", true},
        {"statistics", std::move(statistics).value()},
        {"width", JsonValue::number(std::to_string(previewed.value().width))},
        {"gpu_backend",
         previewed.value().gpu_backend.empty() ? "cpu" : previewed.value().gpu_backend},
    };
    if (!flags.output.empty())
    {
        RenderedImage image;
        image.width = previewed.value().width;
        image.height = previewed.value().height;
        image.rgb = previewed.value().rgb;
        image.color_profile = previewed.value().color_profile;
        auto encoded = engine.encode_png(image);
        if (!encoded)
        {
            return encoded.error();
        }
        QtRasterDecoder raster_decoder;
        auto artifact = describe_png_artifact(raster_decoder, encoded.value(), image.width,
                                              image.height, image.color_profile, flags.output);
        if (!artifact)
            return artifact.error();
        auto written = publish_bytes_artifact_no_replace(flags.output, encoded.value());
        if (!written)
            return written.error();
        payload.emplace("output", std::string(flags.output));
        payload.emplace("artifact", std::move(artifact).value());
    }
    return JsonValue{std::move(payload)};
}

} // namespace ravo::cli_internal
