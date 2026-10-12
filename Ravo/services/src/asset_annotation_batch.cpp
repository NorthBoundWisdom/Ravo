#include "ravo/services/asset_annotation_batch.h"
#include "ravo/services/catalog_service.h"

#include <exception>
#include <set>

namespace ravo
{
namespace
{
std::string diagnostic_prefix(const std::string_view text, const std::size_t maximum)
{
    if (text.size() <= maximum)
        return std::string(text);
    auto end = maximum;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
        --end;
    return std::string(text.substr(0, end));
}
Result<void> validate_ids(const std::vector<std::string> &ids)
{
    if (ids.empty() || ids.size() > kAnnotationBatchMaximumItems)
        return make_error(ErrorCode::kInvalidArgument, "Annotation target count must be 1-32");
    std::set<std::string_view> unique;
    for (const auto &id : ids)
        if (id.empty() || id.size() > 256 || !unique.insert(id).second)
            return make_error(ErrorCode::kInvalidArgument, "Invalid or duplicate annotation target",
                              {{"asset_id", id}});
    return {};
}
Result<void> check_revision(CatalogService &service, const std::int64_t revision)
{
    auto snapshot = service.library().snapshot();
    if (!snapshot)
        return snapshot.error();
    if (revision < 0 || snapshot.value().revision != revision)
        return make_error(ErrorCode::kConflict, "Annotation catalog revision changed",
                          {{"reason", "stale_catalog_revision"}});
    return {};
}
JsonValue error_json(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
        context.emplace(key, value);
    JsonValue value = JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                                        {"message", error.message},
                                        {"context", std::move(context)}};
    if (serialize_json(value).size() <= 4096)
        return value;
    JsonValue::Object bounded;
    for (const auto name :
         {"reason", "mutation_applied", "catalog_committed", "committed_revision"})
        if (const auto found = error.context.find(name); found != error.context.end())
            bounded.emplace(name, diagnostic_prefix(found->second, 64));
    return JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                             {"message", diagnostic_prefix(error.message, 256)},
                             {"context", std::move(bounded)},
                             {"diagnostics_truncated", true}};
}
std::string_view status_name(const AssetAnnotationBatchStatus status)
{
    switch (status)
    {
    case AssetAnnotationBatchStatus::kNotStarted:
        return "not_started";
    case AssetAnnotationBatchStatus::kCommitted:
        return "committed";
    case AssetAnnotationBatchStatus::kFailed:
        return "failed";
    case AssetAnnotationBatchStatus::kCancelled:
        return "cancelled";
    case AssetAnnotationBatchStatus::kUnknown:
        return "unknown";
    }
    std::terminate();
}
} // namespace

Result<std::vector<AssetAnnotationBatchItem>> parse_asset_annotation_batch(const JsonValue &items)
try
{
    const auto *array = items.array_if();
    if (!array || array->empty() || array->size() > kAnnotationBatchMaximumItems ||
        serialize_json(items).size() > kAnnotationBatchMaximumBytes)
        return make_error(ErrorCode::kInvalidArgument, "Annotation batch exceeds its bounds");
    std::vector<AssetAnnotationBatchItem> parsed;
    std::vector<std::string> ids;
    for (const auto &item : *array)
    {
        const auto *id = item.find("asset_id"), *annotation = item.find("annotation");
        if (!item.object_if() || item.object_if()->size() != 2 || !id || !id->string_if() ||
            !annotation)
            return make_error(ErrorCode::kInvalidArgument, "Invalid annotation batch item");
        auto edit = parse_asset_annotation_edit(*annotation);
        if (!edit)
            return edit.error();
        ids.push_back(*id->string_if());
        parsed.push_back({*id->string_if(), std::move(edit).value()});
    }
    if (auto valid = validate_ids(ids); !valid)
        return valid.error();
    return parsed;
}
catch (const std::exception &error)
{
    return make_error(ErrorCode::kIo, "Unable to parse annotation batch",
                      {{"mutation_applied", "false"},
                       {"detail", std::string(std::string_view(error.what()).substr(0, 4096))}});
}
catch (...)
{
    return make_error(ErrorCode::kIo, "Annotation batch parsing raised an unknown exception",
                      {{"mutation_applied", "false"}});
}

Result<AssetAnnotationBatchResult>
apply_asset_annotation_batch(CatalogService &service, const JsonValue &items,
                             const std::int64_t expected_revision,
                             const CancellationToken &cancellation)
try
{
    auto parsed = parse_asset_annotation_batch(items);
    if (!parsed)
        return parsed.error();
    if (auto current = check_revision(service, expected_revision); !current)
        return current.error();
    AssetAnnotationBatchResult result;
    result.initial_revision = result.revision = expected_revision;
    result.items.reserve(parsed.value().size());
    for (const auto &item : parsed.value())
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        auto asset = service.library().find_asset(item.asset_id);
        if (!asset)
            return asset.error();
        result.items.push_back({item.asset_id, AssetAnnotationBatchStatus::kNotStarted, {}, {}});
    }
    if (auto current = check_revision(service, expected_revision); !current)
        return current.error();
    for (std::size_t index = 0; index < parsed.value().size(); ++index)
    {
        const auto &item = parsed.value()[index];
        auto &outcome = result.items[index];
        auto changed = apply_asset_annotation(service, item.asset_id, item.edit, result.revision,
                                              cancellation);
        if (changed)
        {
            result.revision = changed.value().revision;
            ++result.committed_count;
            outcome.status = AssetAnnotationBatchStatus::kCommitted;
            outcome.result = std::move(changed).value();
            continue;
        }
        outcome.error = changed.error();
        const auto committed = changed.error().context.find("catalog_committed");
        const auto mutation = changed.error().context.find("mutation_applied");
        result.unknown =
            (committed != changed.error().context.end() && committed->second == "true") ||
            (mutation != changed.error().context.end() && mutation->second == "unknown");
        outcome.status = result.unknown ? AssetAnnotationBatchStatus::kUnknown :
                         changed.error().code == ErrorCode::kCancelled ?
                                          AssetAnnotationBatchStatus::kCancelled :
                                          AssetAnnotationBatchStatus::kFailed;
        break;
    }
    return result;
}
catch (const std::exception &error)
{
    return make_error(ErrorCode::kIo, "Annotation batch raised an exception",
                      {{"mutation_applied", "unknown"},
                       {"detail", std::string(std::string_view(error.what()).substr(0, 4096))}});
}
catch (...)
{
    return make_error(ErrorCode::kIo, "Annotation batch raised an unknown exception",
                      {{"mutation_applied", "unknown"}});
}

JsonValue asset_annotation_batch_result_json(const AssetAnnotationBatchResult &result)
{
    JsonValue::Array rows;
    bool cancelled = false;
    for (const auto &item : result.items)
    {
        JsonValue::Object row{{"asset_id", item.asset_id},
                              {"status", std::string(status_name(item.status))}};
        cancelled = cancelled || item.status == AssetAnnotationBatchStatus::kCancelled;
        if (item.result)
        {
            row.emplace("catalog_revision",
                        JsonValue::number(std::to_string(item.result->revision)));
            JsonValue::Array warnings;
            for (std::size_t index = 0; index < item.result->warnings.size() && index < 16; ++index)
                warnings.push_back(error_json(item.result->warnings[index]));
            row.emplace("warnings", std::move(warnings));
            if (item.result->warnings.size() > 16)
                row.emplace("warnings_truncated", true);
        }
        if (item.error)
            row.emplace("error", error_json(*item.error));
        rows.emplace_back(std::move(row));
    }
    const auto outcome = result.unknown                                ? "unknown" :
                         result.committed_count == result.items.size() ? "completed" :
                         result.committed_count != 0                   ? "partial" :
                         cancelled                                     ? "cancelled" :
                                                                         "failed";
    return JsonValue::Object{
        {"schema", "ravo.asset.annotation-batch-result/v1"},
        {"outcome", outcome},
        {"initial_catalog_revision", JsonValue::number(std::to_string(result.initial_revision))},
        {"catalog_revision", JsonValue::number(std::to_string(result.revision))},
        {"committed_count", JsonValue::number(std::to_string(result.committed_count))},
        {"atomic", false},
        {"items", std::move(rows)}};
}

Result<JsonValue> read_asset_annotations(CatalogService &service,
                                         const std::vector<std::string> &asset_ids,
                                         const std::int64_t expected_revision,
                                         const CancellationToken &cancellation)
try
{
    if (auto valid = validate_ids(asset_ids); !valid)
        return valid.error();
    if (auto current = check_revision(service, expected_revision); !current)
        return current.error();
    JsonValue::Array states;
    constexpr std::size_t maximum = 1024U * 1024U;
    std::size_t bytes = 2048;
    for (const auto &id : asset_ids)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        auto asset = service.library().find_asset(id);
        if (!asset)
            return asset.error();
        auto state = asset_annotation_state_json(asset.value());
        auto size = serialize_json(state).size();
        // Reserve a bounded unavailable-state entry for every remaining ID.
        constexpr std::size_t reserve = kAnnotationBatchMaximumItems * 2048;
        if (bytes >= maximum - reserve || size > maximum - reserve - bytes)
        {
            state = JsonValue::Object{{"schema", "ravo.asset.annotations/v1"},
                                      {"asset_id", id},
                                      {"available", false},
                                      {"reason", "annotation_batch_state_budget_exceeded"}};
            size = serialize_json(state).size();
        }
        bytes += size;
        states.emplace_back(std::move(state));
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (auto current = check_revision(service, expected_revision); !current)
        return current.error();
    return JsonValue{JsonValue::Object{
        {"schema", "ravo.asset.annotations-read/v1"},
        {"catalog_revision", JsonValue::number(std::to_string(expected_revision))},
        {"assets", std::move(states)}}};
}
catch (const std::exception &error)
{
    return make_error(ErrorCode::kIo, "Unable to read annotation states",
                      {{"mutation_applied", "false"},
                       {"detail", std::string(std::string_view(error.what()).substr(0, 4096))}});
}
catch (...)
{
    return make_error(ErrorCode::kIo, "Annotation state read raised an unknown exception",
                      {{"mutation_applied", "false"}});
}
} // namespace ravo
