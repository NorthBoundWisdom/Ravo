#include "application_internal.h"

#include <string>
#include <utility>

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

} // namespace

Result<JsonValue> run_catalog_convert_command(CatalogService &service,
                                              const std::string_view subcommand,
                                              const CatalogCliArguments &flags)
{
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
