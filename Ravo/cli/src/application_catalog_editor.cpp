#include "application_internal.h"

#include "ravo/services/external_editor.h"

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue fingerprint_json(const ExternalEditorFileFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] JsonValue provenance_to_json(const ExternalEditorProvenance &provenance)
{
    JsonValue::Object object{
        {"schema", provenance.schema},
        {"schema_version", JsonValue::number(std::to_string(provenance.schema_version))},
        {"derived_asset_id", provenance.derived_asset_id},
        {"source_asset_id", provenance.source_asset_id},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(provenance.observed_catalog_revision))},
        {"source_recovery_generation",
         JsonValue::number(std::to_string(provenance.source_recovery_generation))},
        {"source_original", fingerprint_json(provenance.source_original)},
        {"derived", fingerprint_json(provenance.derived)},
        {"editor_id", provenance.editor_id},
        {"registered_unix_ms", JsonValue::number(std::to_string(provenance.registered_unix_ms))},
        {"derived_path", provenance.derived_path},
        {"source_original_path", provenance.source_original_path},
    };
    if (provenance.editor_version)
        object.emplace("editor_version", *provenance.editor_version);
    return JsonValue{std::move(object)};
}

} // namespace

Result<JsonValue> run_catalog_editor_command(CatalogService &service,
                                             const std::string_view subcommand,
                                             const CatalogCliArguments &flags)
{
    if (subcommand == "editor-register")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires --asset-id");
        }
        if (flags.inputs.size() != 1U)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires exactly one --input");
        }
        if (flags.editor_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires --editor");
        }
        ExternalEditorRegisterRequest request;
        request.source_asset_id = std::string(flags.asset_id);
        request.editor_output_path = std::string(flags.inputs.front());
        request.editor_id = std::string(flags.editor_id);
        if (!flags.editor_version.empty())
            request.editor_version = std::string(flags.editor_version);
        if (!flags.import_destination.empty())
            request.destination_directory = std::string(flags.import_destination);
        request.expected_catalog_revision = flags.expected_revision;
        auto registered = service.register_external_editor_output(request);
        if (!registered)
            return registered.error();
        auto asset_json = asset_to_json(registered.value().derived_asset);
        JsonValue::Object object =
            asset_json.object_if() != nullptr ? *asset_json.object_if() : JsonValue::Object{};
        object.emplace("provenance", provenance_to_json(registered.value().provenance));
        object.emplace("source_original_unchanged", registered.value().source_original_unchanged);
        return JsonValue{std::move(object)};
    }
    if (subcommand == "editor-show")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-show requires --asset-id <derived-id>");
        }
        auto provenance = service.external_editor_provenance(flags.asset_id);
        if (!provenance)
            return provenance.error();
        return provenance_to_json(provenance.value());
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog editor subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
