#include "application_internal.h"

#include <string>
#include <utility>

#include "ravo/services/dng_smart_preview.h"
#include "ravo/services/offline_edit_proxy.h"
#include "ravo/services/foreign_catalog.h"

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue fingerprint_json(const ForeignCatalogFileFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"path", value.path},
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] JsonValue item_json(const ForeignCatalogItemReport &item)
{
    JsonValue::Object object{
        {"foreign_id", item.foreign_id},
        {"status", std::string(foreign_catalog_item_status_name(item.status))},
    };
    if (item.original_path)
        object.emplace("original_path", *item.original_path);
    if (item.asset_id)
        object.emplace("asset_id", *item.asset_id);
    JsonValue::Array mapped;
    mapped.reserve(item.mapped_fields.size());
    for (const auto &field : item.mapped_fields)
        mapped.emplace_back(field);
    object.emplace("mapped_fields", std::move(mapped));
    object.emplace("unsupported_fields", crs_omissions_json(item.unsupported_fields));
    JsonValue::Array reasons;
    reasons.reserve(item.reasons.size());
    for (const auto &reason : item.reasons)
        reasons.emplace_back(reason);
    object.emplace("reasons", std::move(reasons));
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue report_json(const ForeignCatalogConversionReport &report)
{
    JsonValue::Object object{
        {"schema", report.schema},
        {"schema_version", JsonValue::number(std::to_string(report.schema_version))},
        {"source_kind", std::string(foreign_catalog_source_kind_name(report.source_kind))},
        {"source_path", report.source_path},
        {"destination_catalog", report.destination_catalog},
        {"imported", JsonValue::number(std::to_string(report.imported))},
        {"skipped", JsonValue::number(std::to_string(report.skipped))},
        {"unsupported", JsonValue::number(std::to_string(report.unsupported))},
        {"failed", JsonValue::number(std::to_string(report.failed))},
        {"unsupported_field_count", JsonValue::number(std::to_string(report.unsupported_fields))},
        {"originals_unchanged", report.originals_unchanged},
        {"cancelled", report.cancelled},
    };
    if (report.source_product_version)
        object.emplace("source_product_version", *report.source_product_version);
    JsonValue::Array originals;
    originals.reserve(report.source_originals.size());
    for (const auto &fingerprint : report.source_originals)
        originals.push_back(fingerprint_json(fingerprint));
    object.emplace("source_originals", std::move(originals));
    JsonValue::Array items;
    items.reserve(report.items.size());
    for (const auto &item : report.items)
        items.push_back(item_json(item));
    object.emplace("items", std::move(items));
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue offline_proxy_manifest_json(const OfflineEditProxyManifest &manifest)
{
    return JsonValue{JsonValue::Object{
        {"schema", manifest.schema},
        {"schema_version", JsonValue::number(std::to_string(manifest.schema_version))},
        {"asset_id", manifest.asset_id},
        {"source_sha256", manifest.source_sha256},
        {"source_size_bytes", JsonValue::number(std::to_string(manifest.source_size_bytes))},
        {"source_mtime_unix_ms", JsonValue::number(std::to_string(manifest.source_mtime_unix_ms))},
        {"recipe_cache_key", manifest.recipe_cache_key},
        {"max_edge", JsonValue::number(std::to_string(manifest.max_edge))},
        {"profile", manifest.profile},
        {"proxy_path", manifest.proxy_path},
        {"proxy_sha256", manifest.proxy_sha256},
        {"width", JsonValue::number(std::to_string(manifest.width))},
        {"height", JsonValue::number(std::to_string(manifest.height))},
        {"created_unix_ms", JsonValue::number(std::to_string(manifest.created_unix_ms))},
        {"pixel_provenance", manifest.pixel_provenance},
    }};
}

[[nodiscard]] JsonValue offline_proxy_status_json(const OfflineEditProxyStatus &status)
{
    JsonValue::Object object{
        {"schema", status.schema},
        {"asset_id", status.asset_id},
        {"media_state", std::string(offline_edit_media_state_name(status.media_state))},
        {"proxy_present", status.proxy_present},
        {"proxy_verified", status.proxy_verified},
        {"usable_for_develop", status.usable_for_develop},
        {"usable_for_export", status.usable_for_export},
        {"reason", status.reason},
    };
    if (status.manifest)
        object.emplace("manifest", offline_proxy_manifest_json(*status.manifest));
    return JsonValue{std::move(object)};
}

} // namespace

[[nodiscard]] JsonValue smart_preview_status_json(const SmartPreviewStatus &status)
{
    JsonValue::Object object{
        {"schema", status.schema},
        {"asset_id", status.asset_id},
        {"encoder_available", status.encoder_available},
        {"present", status.present},
        {"develop_fallback", status.develop_fallback},
        {"reason", status.reason},
    };
    if (status.path)
        object.emplace("path", *status.path);
    return JsonValue{std::move(object)};
}

Result<JsonValue> run_catalog_convert_command(CatalogService &service,
                                              const std::string_view subcommand,
                                              const CatalogCliArguments &flags)
{
    if (subcommand == "dng-convert")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog dng-convert requires --asset-id");
        }
        DngConversionRequest request;
        request.asset_id = std::string(flags.asset_id);
        if (!flags.output.empty())
            request.output_path = std::string(flags.output);
        auto converted = service.convert_asset_to_dng(request);
        if (!converted)
            return converted.error();
        return JsonValue{JsonValue::Object{
            {"schema", converted.value().schema},
            {"asset_id", converted.value().asset_id},
            {"source_path", converted.value().source_path},
            {"originals_unchanged", converted.value().originals_unchanged},
            {"converter_available", converted.value().converter_available},
            {"reason", converted.value().reason},
        }};
    }
    if (subcommand == "dng-status")
    {
        return JsonValue{JsonValue::Object{
            {"converter_available", dng_converter_is_packaged()},
            {"reason",
             dng_converter_is_packaged() ? "dng_converter_packaged" : "dng_converter_unavailable"},
        }};
    }
    if (subcommand == "smart-preview")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog smart-preview requires --asset-id");
        }
        if (flags.ensure)
        {
            SmartPreviewEnsureRequest request;
            request.asset_id = std::string(flags.asset_id);
            auto ensured = service.ensure_smart_preview(request);
            if (!ensured)
                return ensured.error();
            return smart_preview_status_json(ensured.value());
        }
        auto status = service.smart_preview_status(flags.asset_id);
        if (!status)
            return status.error();
        return smart_preview_status_json(status.value());
    }

    if (subcommand == "offline-proxy-create")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-create requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-create requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        OfflineEditProxyCreateRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.user_initiated = true;
        if (flags.max_edge)
            request.max_edge = *flags.max_edge;
        // v1 profile is fixed to srgb (ADR-0146); do not reuse export delivery flags.
        auto created = service.create_offline_edit_proxy(request);
        if (!created)
            return created.error();
        return JsonValue{JsonValue::Object{
            {"manifest", offline_proxy_manifest_json(created.value().manifest)},
            {"originals_unchanged", created.value().originals_unchanged},
        }};
    }
    if (subcommand == "offline-proxy-list")
    {
        auto listed = service.list_offline_edit_proxies();
        if (!listed)
            return listed.error();
        JsonValue::Array items;
        items.reserve(listed.value().manifests.size());
        for (const auto &manifest : listed.value().manifests)
            items.push_back(offline_proxy_manifest_json(manifest));
        JsonValue::Array corrupt;
        corrupt.reserve(listed.value().corrupt.size());
        for (const auto &entry : listed.value().corrupt)
        {
            corrupt.push_back(JsonValue{JsonValue::Object{
                {"asset_id", entry.asset_id},
                {"path", entry.path},
                {"reason", entry.reason},
            }});
        }
        return JsonValue{
            JsonValue::Object{{"proxies", std::move(items)}, {"corrupt", std::move(corrupt)}}};
    }
    if (subcommand == "offline-proxy-verify" || subcommand == "offline-proxy-status")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-verify requires --asset-id");
        }
        auto status = service.verify_offline_edit_proxy(flags.asset_id);
        if (!status)
            return status.error();
        return offline_proxy_status_json(status.value());
    }
    if (subcommand == "offline-proxy-reconnect")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-reconnect requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-reconnect requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        OfflineEditProxyReconnectRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.user_initiated = true;
        auto reconnected = service.reconnect_offline_edit_proxy(request);
        if (!reconnected)
            return reconnected.error();
        return JsonValue{JsonValue::Object{
            {"status", offline_proxy_status_json(reconnected.value().status)},
            {"source_hash_matched", reconnected.value().source_hash_matched},
            {"originals_unchanged", reconnected.value().originals_unchanged},
            {"offline_states_cleared", reconnected.value().offline_states_cleared},
        }};
    }
    if (subcommand != "convert-foreign")
    {
        return make_error(ErrorCode::kInvalidArgument, "Unknown catalog conversion subcommand",
                          {{"subcommand", std::string(subcommand)}});
    }
    if (flags.foreign_source.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "catalog convert-foreign requires --foreign-source <path>");
    }
    ForeignCatalogConversionRequest request;
    request.source_path = std::string(flags.foreign_source);
    if (!flags.foreign_source_kind.empty())
    {
        auto kind = parse_foreign_catalog_source_kind(flags.foreign_source_kind);
        if (!kind)
            return kind.error();
        request.source_kind = kind.value();
    }
    auto converted = service.convert_foreign_catalog(request);
    if (!converted)
        return converted.error();
    return report_json(converted.value());
}

} // namespace ravo::cli_internal
