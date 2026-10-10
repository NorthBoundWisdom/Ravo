#include "ravo/services/foreign_catalog.h"
#include <charconv>

namespace ravo
{
JsonValue foreign_catalog_item_to_json(const ForeignCatalogItemReport &item)
{
    JsonValue::Object value{{"foreign_id", item.foreign_id},
                            {"phase", item.phase},
                            {"resumed", item.resumed},
                            {"status", std::string(foreign_catalog_item_status_name(item.status))}};
    if (item.asset_id)
        value.emplace("asset_id", *item.asset_id);
    if (item.original_path)
        value.emplace("original_path", *item.original_path);
    if (item.source_fingerprint)
        value.emplace("source_fingerprint",
                      JsonValue::Object{
                          {"path", item.source_fingerprint->path},
                          {"sha256", item.source_fingerprint->sha256},
                          {"size_bytes",
                           JsonValue::number(std::to_string(item.source_fingerprint->size_bytes))},
                          {"mtime_unix_ms", JsonValue::number(std::to_string(
                                                item.source_fingerprint->mtime_unix_ms))}});
    JsonValue::Array mapped, reasons, omissions;
    for (const auto &field : item.mapped_fields)
        mapped.emplace_back(field);
    for (const auto &reason : item.reasons)
        reasons.emplace_back(reason);
    for (const auto &omitted : item.unsupported_fields)
        omissions.emplace_back(JsonValue::Object{
            {"key", omitted.key}, {"value", omitted.value}, {"reason", omitted.reason}});
    value.emplace("mapped_fields", std::move(mapped));
    value.emplace("reasons", std::move(reasons));
    value.emplace("unsupported_fields", std::move(omissions));
    return JsonValue{std::move(value)};
}

Result<ForeignCatalogItemReport> foreign_catalog_item_from_json(const std::string_view text)
{
    const auto invalid = []
    { return make_error(ErrorCode::kValidation, "Invalid conversion receipt"); };
    if (text.size() > 16000000)
        return invalid();
    auto parsed = parse_json(text);
    if (!parsed || !parsed.value().object_if())
        return invalid();
    const auto string = [](const JsonValue &value, const char *key) -> const std::string *
    {
        const auto *item = value.find(key);
        return item ? item->string_if() : nullptr;
    };
    const auto *id = string(parsed.value(), "foreign_id");
    const auto *status = string(parsed.value(), "status");
    const auto *phase = string(parsed.value(), "phase");
    if (!id || !status || !phase)
        return invalid();
    ForeignCatalogItemReport item;
    item.foreign_id = *id;
    item.phase = *phase;
    bool found = false;
    for (const auto candidate :
         {ForeignCatalogItemStatus::kImported, ForeignCatalogItemStatus::kFailed,
          ForeignCatalogItemStatus::kSkipped, ForeignCatalogItemStatus::kUnsupported})
        if (*status == foreign_catalog_item_status_name(candidate))
        {
            item.status = candidate;
            found = true;
        }
    if (!found)
        return invalid();
    if (const auto *asset = string(parsed.value(), "asset_id"))
        item.asset_id = *asset;
    if (const auto *path = string(parsed.value(), "original_path"))
        item.original_path = *path;
    if (const auto *source = parsed.value().find("source_fingerprint"))
    {
        const auto *path = string(*source, "path"), *hash = string(*source, "sha256");
        const auto *size = source->find("size_bytes"), *mtime = source->find("mtime_unix_ms");
        if (!path || !hash || hash->size() != 64 || !size || !size->number_if() || !mtime ||
            !mtime->number_if())
            return invalid();
        ForeignCatalogFileFingerprint fingerprint;
        fingerprint.path = *path;
        fingerprint.sha256 = *hash;
        const auto number = [](const JsonValue *value, auto &out)
        {
            const auto &text = value->number_if()->text;
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), out);
            return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
        };
        if (!number(size, fingerprint.size_bytes) || !number(mtime, fingerprint.mtime_unix_ms))
            return invalid();
        item.source_fingerprint = std::move(fingerprint);
    }
    for (const auto &[key, output] :
         {std::pair{"mapped_fields", &item.mapped_fields}, std::pair{"reasons", &item.reasons}})
    {
        const auto *array = parsed.value().find(key);
        if (!array || !array->array_if())
            return invalid();
        for (const auto &value : *array->array_if())
        {
            if (!value.string_if())
                return invalid();
            output->push_back(*value.string_if());
        }
    }
    const auto *omissions = parsed.value().find("unsupported_fields");
    if (!omissions || !omissions->array_if())
        return invalid();
    for (const auto &value : *omissions->array_if())
    {
        const auto *key = string(value, "key"), *raw = string(value, "value"),
                   *reason = string(value, "reason");
        if (!key || !raw || !reason)
            return invalid();
        item.unsupported_fields.push_back({*key, *raw, *reason});
    }
    return item;
}
JsonValue foreign_catalog_collection_to_json(const ForeignCatalogCollectionReport &item)
{
    JsonValue::Array reasons;
    for (const auto &reason : item.reasons)
        reasons.emplace_back(reason);
    JsonValue::Object value{
        {"foreign_id", item.foreign_id},
        {"name", item.name},
        {"imported_members", JsonValue::number(std::to_string(item.imported_members))},
        {"reasons", std::move(reasons)}};
    if (item.set_id)
        value.emplace("set_id", *item.set_id);
    return JsonValue{std::move(value)};
}

Result<ForeignCatalogCollectionReport>
foreign_catalog_collection_from_json(const std::string_view text)
{
    auto parsed = parse_json(text);
    if (!parsed || !parsed.value().object_if())
        return make_error(ErrorCode::kValidation, "Invalid collection receipt");
    const auto *id = parsed.value().find("foreign_id"), *name = parsed.value().find("name"),
               *members = parsed.value().find("imported_members"),
               *reasons = parsed.value().find("reasons");
    if (!id || !id->string_if() || !name || !name->string_if() || !members ||
        !members->number_if() || !reasons || !reasons->array_if())
        return make_error(ErrorCode::kValidation, "Invalid collection receipt");
    ForeignCatalogCollectionReport item;
    item.foreign_id = *id->string_if();
    item.name = *name->string_if();
    std::uint64_t count = 0;
    const auto &number = members->number_if()->text;
    const auto converted = std::from_chars(number.data(), number.data() + number.size(), count);
    if (converted.ec != std::errc{} || converted.ptr != number.data() + number.size() ||
        count > 5000000)
        return make_error(ErrorCode::kValidation, "Invalid collection member count");
    item.imported_members = static_cast<std::size_t>(count);
    if (const auto *set = parsed.value().find("set_id"))
    {
        if (!set->string_if())
            return make_error(ErrorCode::kValidation, "Invalid collection set identity");
        item.set_id = *set->string_if();
    }
    for (const auto &reason : *reasons->array_if())
    {
        if (!reason.string_if())
            return make_error(ErrorCode::kValidation, "Invalid collection reason");
        item.reasons.push_back(*reason.string_if());
    }
    return item;
}
} // namespace ravo
