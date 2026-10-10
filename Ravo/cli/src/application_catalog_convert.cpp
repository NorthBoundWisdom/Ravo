#include "application_internal.h"

#include <string>
#include <algorithm>
#include <utility>

#include "ravo/services/dng_smart_preview.h"
#include "ravo/services/offline_edit_proxy.h"
#include "ravo/services/foreign_catalog.h"
#include "ravo/services/conversion_service.h"

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
    return foreign_catalog_item_to_json(item);
}

[[nodiscard]] JsonValue report_json(const ForeignCatalogConversionReport &report)
{
    JsonValue::Object object{
        {"schema", report.schema},
        {"schema_version", JsonValue::number(std::to_string(report.schema_version))},
        {"source_kind", std::string(foreign_catalog_source_kind_name(report.source_kind))},
        {"source_path", report.source_path},
        {"destination_catalog", report.destination_catalog},
        {"conversion_id", report.conversion_id},
        {"imported", JsonValue::number(std::to_string(report.imported))},
        {"skipped", JsonValue::number(std::to_string(report.skipped))},
        {"unsupported", JsonValue::number(std::to_string(report.unsupported))},
        {"failed", JsonValue::number(std::to_string(report.failed))},
        {"unsupported_field_count", JsonValue::number(std::to_string(report.unsupported_fields))},
        {"originals_unchanged", report.originals_unchanged},
        {"source_audit_complete", report.source_audit_complete},
        {"cancelled", report.cancelled},
        {"source_photo_count", JsonValue::number(std::to_string(report.source_photo_count))},
        {"selected_photo_count", JsonValue::number(std::to_string(report.selected_photo_count))},
    };
    if (report.source_product_version)
        object.emplace("source_product_version", *report.source_product_version);
    if (report.source_archive)
        object.emplace("source_archive",
                       JsonValue::Object{{"source_id", report.source_archive->source_id},
                                         {"sha256", report.source_archive->sha256},
                                         {"size_bytes", JsonValue::number(std::to_string(
                                                            report.source_archive->size_bytes))}});
    JsonValue::Array originals;
    originals.reserve(report.source_originals.size());
    for (const auto &fingerprint : report.source_originals)
        originals.push_back(fingerprint_json(fingerprint));
    object.emplace("source_originals", std::move(originals));
    const auto error_json = [](const TaskError &error)
    {
        JsonValue::Object context;
        for (const auto &[key, value] : error.context)
            context.emplace(key, value);
        return JsonValue{JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                                           {"message", error.message},
                                           {"context", std::move(context)}}};
    };
    JsonValue::Array audits, issues;
    for (const auto &audit : report.source_audits)
    {
        JsonValue::Object entry{{"before", fingerprint_json(audit.before)},
                                {"status", audit.status}};
        if (audit.after)
            entry.emplace("after", fingerprint_json(*audit.after));
        if (audit.error)
            entry.emplace("error", error_json(*audit.error));
        audits.emplace_back(std::move(entry));
    }
    for (const auto &issue : report.issues)
        issues.emplace_back(error_json(issue));
    object.emplace("source_audits", std::move(audits));
    object.emplace("issues", std::move(issues));
    object.emplace("committed_item_count",
                   JsonValue::number(std::to_string(
                       std::count_if(report.items.begin(), report.items.end(),
                                     [](const auto &item) { return item.asset_id.has_value(); }))));
    JsonValue::Array items;
    items.reserve(report.items.size());
    for (const auto &item : report.items)
        items.push_back(item_json(item));
    object.emplace("items", std::move(items));
    JsonValue::Array collections;
    std::size_t collection_issues = 0;
    for (const auto &collection : report.collections)
    {
        collection_issues += collection.reasons.size();
        collections.push_back(foreign_catalog_collection_to_json(collection));
    }
    object.emplace("collections", std::move(collections));
    object.emplace("collection_issue_count", JsonValue::number(std::to_string(collection_issues)));
    object.emplace("archived_only_table_count",
                   JsonValue::number(std::to_string(report.archived_only_tables.size())));
    JsonValue::Array archived_tables;
    for (const auto &table : report.archived_only_tables)
        archived_tables.emplace_back(table);
    object.emplace("archived_only_tables", std::move(archived_tables));
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
        {"pinned", manifest.pinned},
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
    if (subcommand == "foreign-conversions")
    {
        auto ids = service.conversion().foreign_conversion_ids();
        if (!ids)
            return ids.error();
        JsonValue::Array conversions;
        for (const auto &id : ids.value())
            conversions.emplace_back(id);
        return JsonValue{JsonValue::Object{{"schema", "ravo.foreign-conversions/v1"},
                                           {"conversion_ids", std::move(conversions)}}};
    }
    if (subcommand == "foreign-conversion-status")
    {
        if (flags.conversion_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "foreign-conversion-status requires --conversion-id");
        auto journal = service.conversion().foreign_conversion_status(flags.conversion_id);
        if (!journal)
            return journal.error();
        if (!journal.value())
            return make_error(ErrorCode::kNotFound, "Conversion journal not found");
        JsonValue::Array records;
        JsonValue::Array commits;
        for (const auto &proof : journal.value()->commits)
        {
            JsonValue::Object entry{
                {"foreign_id", proof.foreign_id},
                {"phase", proof.phase},
                {"revision", JsonValue::number(std::to_string(proof.revision))}};
            if (proof.target_id)
                entry.emplace("target_id", *proof.target_id);
            commits.emplace_back(std::move(entry));
        }
        for (const auto &record : journal.value()->records)
        {
            auto receipt = parse_json(record.receipt_json);
            if (!receipt)
                return receipt.error();
            JsonValue::Object item{{"foreign_id", record.foreign_id},
                                   {"phase", record.phase},
                                   {"complete", record.complete},
                                   {"receipt", std::move(receipt).value()}};
            if (record.asset_id)
                item.emplace("target_id", *record.asset_id);
            records.emplace_back(std::move(item));
        }
        return JsonValue{JsonValue::Object{
            {"schema", "ravo.foreign-conversion-status/v1"},
            {"conversion_id", journal.value()->conversion_id},
            {"source_sha256", journal.value()->source_sha256},
            {"catalog_revision",
             JsonValue::number(std::to_string(journal.value()->catalog_revision))},
            {"records", std::move(records)},
            {"commits", std::move(commits)}}};
    }
    if (subcommand == "foreign-sources")
    {
        auto sources = service.conversion().foreign_catalog_archives();
        if (!sources)
            return sources.error();
        JsonValue::Array items;
        for (const auto &source : sources.value())
            items.emplace_back(JsonValue::Object{
                {"source_id", source.source_id},
                {"source_path", source.source_path},
                {"sha256", source.sha256},
                {"size_bytes", JsonValue::number(std::to_string(source.size_bytes))}});
        return JsonValue{JsonValue::Object{{"schema", "ravo.foreign-catalog-sources/v1"},
                                           {"sources", std::move(items)}}};
    }
    if (subcommand == "foreign-source-export")
    {
        if (flags.source_id.empty() || flags.output.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "foreign-source-export requires --source-id and --output");
        auto exported =
            service.conversion().export_foreign_catalog_archive(flags.source_id, flags.output);
        if (!exported)
            return exported.error();
        return JsonValue{JsonValue::Object{{"schema", "ravo.foreign-catalog-source-export/v1"},
                                           {"source_id", std::string(flags.source_id)},
                                           {"output", std::string(flags.output)}}};
    }
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
        auto converted = service.conversion().convert_asset_to_dng(request);
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
            auto ensured = service.conversion().ensure_smart_preview(request);
            if (!ensured)
                return ensured.error();
            return smart_preview_status_json(ensured.value());
        }
        auto status = service.conversion().smart_preview_status(flags.asset_id);
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
        auto created = service.offline().create_offline_edit_proxy(request);
        if (!created)
            return created.error();
        return JsonValue{JsonValue::Object{
            {"manifest", offline_proxy_manifest_json(created.value().manifest)},
            {"originals_unchanged", created.value().originals_unchanged},
        }};
    }
    if (subcommand == "offline-proxy-list")
    {
        auto listed = service.offline().list_offline_edit_proxies();
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
        auto status = service.offline().verify_offline_edit_proxy(flags.asset_id);
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
        request.clear_proxy = flags.clear_proxy;
        auto reconnected = service.offline().reconnect_offline_edit_proxy(request);
        if (!reconnected)
            return reconnected.error();
        return JsonValue{JsonValue::Object{
            {"status", offline_proxy_status_json(reconnected.value().status)},
            {"source_hash_matched", reconnected.value().source_hash_matched},
            {"originals_unchanged", reconnected.value().originals_unchanged},
            {"offline_states_cleared", reconnected.value().offline_states_cleared},
            {"proxy_cleared", reconnected.value().proxy_cleared},
        }};
    }
    if (subcommand == "offline-proxy-delete")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-delete requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-delete requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        OfflineEditProxyDeleteRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.user_initiated = true;
        request.force = flags.force;
        auto deleted = service.offline().delete_offline_edit_proxy(request);
        if (!deleted)
            return deleted.error();
        return JsonValue{JsonValue::Object{
            {"deleted", deleted.value().deleted},
            {"originals_unchanged", deleted.value().originals_unchanged},
            {"reason", deleted.value().reason},
        }};
    }
    if (subcommand == "offline-proxy-pin")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-pin requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-pin requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        OfflineEditProxyPinRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.user_initiated = true;
        request.pinned = !flags.unpin;
        auto pinned = service.offline().pin_offline_edit_proxy(request);
        if (!pinned)
            return pinned.error();
        return JsonValue{JsonValue::Object{
            {"manifest", offline_proxy_manifest_json(pinned.value().manifest)},
            {"pinned", pinned.value().manifest.pinned},
        }};
    }
    if (subcommand == "offline-proxy-evict")
    {
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-evict requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        if (!flags.max_total_bytes)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog offline-proxy-evict requires --max-total-bytes",
                              {{"reason", "missing_max_total_bytes"}});
        }
        OfflineEditProxyEvictRequest request;
        request.user_initiated = true;
        request.max_total_bytes = *flags.max_total_bytes;
        auto evicted = service.offline().evict_offline_edit_proxies(request);
        if (!evicted)
            return evicted.error();
        JsonValue::Array evicted_ids;
        for (const auto &id : evicted.value().evicted_asset_ids)
            evicted_ids.push_back(JsonValue{id});
        JsonValue::Array pinned_ids;
        for (const auto &id : evicted.value().retained_pinned_asset_ids)
            pinned_ids.push_back(JsonValue{id});
        return JsonValue{JsonValue::Object{
            {"evicted", JsonValue::number(std::to_string(evicted.value().evicted))},
            {"retained_pinned", JsonValue::number(std::to_string(evicted.value().retained_pinned))},
            {"bytes_retained", JsonValue::number(std::to_string(evicted.value().bytes_retained))},
            {"evicted_asset_ids", std::move(evicted_ids)},
            {"retained_pinned_asset_ids", std::move(pinned_ids)},
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
    request.resume = flags.foreign_resume;
    request.path_mappings = flags.foreign_path_mappings;
    request.foreign_ids = flags.foreign_ids;
    request.expected_source_sha256 = std::string(flags.expected_foreign_source_sha256);
    if (!flags.foreign_source_kind.empty())
    {
        auto kind = parse_foreign_catalog_source_kind(flags.foreign_source_kind);
        if (!kind)
            return kind.error();
        request.source_kind = kind.value();
    }
    auto converted = service.conversion().convert_foreign_catalog(request);
    if (!converted)
        return converted.error();
    return report_json(converted.value());
}

Result<JsonValue> run_catalog_inspect_foreign_command(const CatalogCliArguments &flags)
{
    if (flags.foreign_source.empty())
        return make_error(ErrorCode::kInvalidArgument, "inspect-foreign requires --foreign-source");
    if (!flags.foreign_source_kind.empty() && flags.foreign_source_kind != "lightroom-classic")
        return make_error(ErrorCode::kUnsupported,
                          "inspect-foreign currently supports Lightroom Classic");
    auto inspected = ConversionService::inspect_lightroom_catalog(flags.foreign_source);
    if (!inspected)
        return inspected.error();
    const auto &value = inspected.value();
    const auto count = [](const std::size_t number)
    { return JsonValue::number(std::to_string(number)); };
    JsonValue::Object fields, profiles;
    for (const auto &[name, number] : value.develop_fields)
        fields.emplace(name, count(number));
    for (const auto &[name, number] : value.camera_profiles)
        profiles.emplace(name, count(number));
    JsonValue::Array archived, photos, malformed;
    JsonValue::Object editing_samples;
    for (const auto &[reason, photo] : value.editing_samples)
        editing_samples.emplace(reason, JsonValue::Object{{"foreign_id", photo.foreign_id},
                                                          {"original_path", photo.original_path}});
    for (const auto &sample : value.malformed_samples)
    {
        JsonValue::Object issue;
        for (const auto &[key, text] : sample)
            issue.emplace(key, text);
        malformed.emplace_back(std::move(issue));
    }
    for (const auto &table : value.archived_only_tables)
        archived.emplace_back(table);
    for (const auto &photo : value.sample_photos)
    {
        JsonValue::Object entry{{"foreign_id", photo.foreign_id},
                                {"original_path", photo.original_path}};
        if (photo.master_id)
            entry.emplace("master_id", *photo.master_id);
        photos.emplace_back(std::move(entry));
    }
    return JsonValue{JsonValue::Object{
        {"schema", "ravo.lightroom-catalog-inspection/v1"},
        {"source_path", value.source_path},
        {"source_sha256", value.source_sha256},
        {"photos", count(value.photos)},
        {"virtual_copies", count(value.virtual_copies)},
        {"current_edits", count(value.current_edits)},
        {"history_steps", count(value.history_steps)},
        {"snapshots", count(value.snapshots)},
        {"collections", count(value.collections)},
        {"metadata_photos", count(value.metadata_photos)},
        {"available_originals", count(value.available_originals)},
        {"malformed_edits", count(value.malformed_edits)},
        {"develop_fields", std::move(fields)},
        {"malformed_samples", std::move(malformed)},
        {"camera_profiles", std::move(profiles)},
        {"archived_only_tables", std::move(archived)},
        {"sample_photos", std::move(photos)},
        {"editing_samples", std::move(editing_samples)},
        {"companion", JsonValue::Object{{"path", value.companion_path},
                                        {"present", value.companion_present},
                                        {"is_directory", value.companion_is_directory},
                                        {"handling", "not_converted_or_archived"}}}}};
}

} // namespace ravo::cli_internal
