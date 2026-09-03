#include "application_internal.h"

#include "ravo/services/xmp_interchange.h"

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue fingerprint_catalog_json(const XmpInterchangeCatalogFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"recovery_generation", JsonValue::number(std::to_string(value.recovery_generation))},
        {"recipe_sha256", value.recipe_sha256},
    }};
}

[[nodiscard]] JsonValue fingerprint_sidecar_json(const XmpInterchangeSidecarFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] JsonValue status_to_json(const XmpInterchangeStatus &status)
{
    JsonValue::Object object{
        {"asset_id", status.asset_id},
        {"original_path", status.original_path},
        {"conflict_class", std::string(xmp_interchange_conflict_class_name(status.conflict_class))},
        {"has_baseline", status.has_baseline},
        {"has_edits", status.has_edits},
        {"catalog", fingerprint_catalog_json(status.catalog)},
        {"crs_parse_ok", status.crs_parse_ok},
    };
    if (status.sidecar_path)
        object.emplace("sidecar_path", *status.sidecar_path);
    if (status.sidecar)
        object.emplace("sidecar", fingerprint_sidecar_json(*status.sidecar));
    if (status.baseline_catalog)
        object.emplace("baseline_catalog", fingerprint_catalog_json(*status.baseline_catalog));
    if (status.baseline_sidecar)
        object.emplace("baseline_sidecar", fingerprint_sidecar_json(*status.baseline_sidecar));
    if (status.crs_parse_reason)
        object.emplace("crs_parse_reason", *status.crs_parse_reason);
    if (!status.omitted.empty())
        object.emplace("omitted", crs_omissions_json(status.omitted));
    return JsonValue{std::move(object)};
}

} // namespace

Result<JsonValue> run_catalog_xmp_command(CatalogService &service,
                                          const std::string_view subcommand,
                                          const CatalogCliArguments &flags)
{
    if (flags.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "catalog xmp commands require --asset-id");
    }
    std::optional<std::string_view> sidecar;
    if (!flags.xmp_path.empty())
        sidecar = flags.xmp_path;

    if (subcommand == "xmp-status")
    {
        if (!flags.xmp_resolve.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "--resolve is only valid for catalog xmp-import or xmp-export");
        }
        auto status = service.xmp_interchange_status(flags.asset_id, sidecar);
        if (!status)
            return status.error();
        return status_to_json(status.value());
    }

    XmpInterchangeResolve resolve = XmpInterchangeResolve::kAbort;
    if (!flags.xmp_resolve.empty())
    {
        auto parsed = parse_xmp_interchange_resolve(flags.xmp_resolve);
        if (!parsed)
            return parsed.error();
        resolve = parsed.value();
    }

    if (subcommand == "xmp-import")
    {
        auto imported = service.xmp_interchange_import(flags.asset_id, resolve, sidecar);
        if (!imported)
            return imported.error();
        auto asset_json = asset_to_json(imported.value().asset);
        JsonValue::Object object =
            asset_json.object_if() != nullptr ? *asset_json.object_if() : JsonValue::Object{};
        object.emplace("status", status_to_json(imported.value().status));
        object.emplace("preset_name", imported.value().preset_name);
        object.emplace("omitted", crs_omissions_json(imported.value().omitted));
        object.emplace("resolve", std::string(xmp_interchange_resolve_name(resolve)));
        return JsonValue{std::move(object)};
    }
    if (subcommand == "xmp-export")
    {
        auto exported = service.xmp_interchange_export(flags.asset_id, resolve, sidecar);
        if (!exported)
            return exported.error();
        auto asset_json = asset_to_json(exported.value().asset);
        JsonValue::Object object =
            asset_json.object_if() != nullptr ? *asset_json.object_if() : JsonValue::Object{};
        object.emplace("status", status_to_json(exported.value().status));
        object.emplace("sidecar_path", exported.value().sidecar_path);
        JsonValue::Array omitted;
        for (const auto &field : exported.value().omitted_catalog_fields)
            omitted.emplace_back(field);
        object.emplace("omitted_catalog_fields", std::move(omitted));
        object.emplace("resolve", std::string(xmp_interchange_resolve_name(resolve)));
        return JsonValue{std::move(object)};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog XMP subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
