#include "studio_operation_store.h"
#include <algorithm>
#include <QUuid>

namespace ravo
{
namespace
{
TaskError unknown_operation(std::string_view id)
{
    return make_error(ErrorCode::kNotFound, "Operation receipt is absent or expired",
                      {{"reason", "unknown_operation"},
                       {"operation_id", std::string(id)},
                       {"mutation_applied", "unknown"}});
}
} // namespace
Result<JsonValue> StudioOperationStore::reserve(const std::string_view session_id)
{
    constexpr std::size_t maximum = 128;
    if (entries_.size() >= maximum)
    {
        const auto found = std::find_if(order_.begin(), order_.end(),
                                        [&](const auto &id) { return !entries_.at(id).active; });
        if (found == order_.end())
            return make_error(ErrorCode::kConflict, "Operation receipt capacity is busy");
        entries_.erase(*found);
        order_.erase(found);
    }
    const auto id = std::string(session_id) + ":" + std::to_string(++sequence_) + ":" +
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    Entry entry;
    entry.receipt = {{"type", "ravo.studio.operation"},
                     {"version", JsonValue::number("1")},
                     {"operation_id", id},
                     {"session_id", std::string(session_id)},
                     {"phase", "reserved"},
                     {"mutation_applied", false},
                     {"lifecycle", "studio_session_bounded"}};
    entries_.emplace(id, std::move(entry));
    order_.push_back(id);
    return get(id);
}
Result<JsonValue> StudioOperationStore::get(const std::string_view id) const
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return unknown_operation(id);
    return JsonValue{found->second.receipt};
}
Result<bool> StudioOperationStore::begin(const std::string_view id, const JsonValue &request)
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return unknown_operation(id);
    auto &entry = found->second;
    const auto json = serialize_json(request);
    if (!entry.request.empty())
    {
        if (entry.request != json)
            return make_error(
                ErrorCode::kConflict, "Operation ID was reused for another command",
                {{"reason", "operation_id_conflict"}, {"operation_id", std::string(id)}});
        return false;
    }
    if (entry.cancellation.token().is_cancellation_requested())
        return make_error(ErrorCode::kCancelled, "Reserved operation was cancelled");
    entry.request = json;
    entry.active = true;
    entry.receipt["phase"] = "pending";
    entry.receipt["mutation_applied"] = JsonValue{nullptr};
    if (const auto *asset = request.find("asset_id"))
        entry.receipt["asset_id"] = *asset;
    if (const auto *kind = request.find("kind"))
        entry.receipt["kind"] = *kind;
    return true;
}
Result<CancellationToken> StudioOperationStore::token(const std::string_view id) const
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return unknown_operation(id);
    return found->second.cancellation.token();
}
Result<JsonValue> StudioOperationStore::cancel(const std::string_view id)
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return unknown_operation(id);
    auto &entry = found->second;
    if (!entry.active && entry.receipt.at("phase").string_if() &&
        *entry.receipt.at("phase").string_if() != "reserved")
        return get(id);
    static_cast<void>(entry.cancellation.cancel("operation_cancelled"));
    entry.receipt["cancel_requested"] = true;
    if (!entry.active)
        finish(id, false, 0, 0, "cancelled", "Reserved operation was cancelled");
    return get(id);
}
void StudioOperationStore::finish(const std::string_view id, const bool committed,
                                  const std::int64_t revision, const std::int64_t head,
                                  std::string code, std::string message)
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return;
    auto &entry = found->second;
    entry.active = false;
    entry.receipt["phase"] = committed ? "committed" : code == "cancelled" ? "cancelled" : "failed";
    entry.receipt["mutation_applied"] = committed;
    entry.receipt["catalog_revision"] = JsonValue::number(std::to_string(revision));
    entry.receipt["history_head"] = JsonValue::number(std::to_string(head));
    if (!code.empty())
        entry.receipt["error"] =
            JsonValue::Object{{"code", std::move(code)}, {"message", std::move(message)}};
}
void StudioOperationStore::unresolved(const std::string_view id, const TaskError &error)
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return;
    auto &entry = found->second;
    entry.active = false;
    entry.receipt["phase"] = "unknown";
    const auto committed = error.context.find("catalog_committed");
    entry.receipt["mutation_applied"] =
        committed != error.context.end() && committed->second == "true" ? JsonValue{true} :
                                                                          JsonValue{nullptr};
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
        context.emplace(key, value);
    entry.receipt["error"] = JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                                               {"message", error.message},
                                               {"context", std::move(context)}};
}

void StudioOperationStore::stage(const std::string_view id, const JsonValue &state,
                                 const bool changed)
{
    const auto found = entries_.find(id);
    if (found == entries_.end())
        return;
    found->second.active = false;
    found->second.receipt["phase"] = "staged";
    found->second.receipt["changed"] = changed;
    found->second.receipt["mutation_applied"] = false;
    JsonValue::Object projection;
    if (const auto *scope = state.find("editing_scope"))
        projection.emplace("editing_scope", *scope);
    if (const auto *recipe = state.find("recipe"))
        if (const auto *revision = recipe->find("revision"))
            projection.emplace("recipe_revision", *revision);
    found->second.receipt["state"] = JsonValue{std::move(projection)};
}
void StudioOperationStore::progress(const std::string_view id, JsonValue result)
{
    const auto found = entries_.find(id);
    if (found != entries_.end())
    {
        if (!found->second.active)
            if (const auto *partial = result.find("partial");
                partial && partial->boolean_if() && *partial->boolean_if())
                found->second.receipt["phase"] = "partially_committed";
        if (const auto *applied = result.find("applied");
            applied && applied->number_if() && applied->number_if()->text != "0")
            found->second.receipt["mutation_applied"] = true;
        found->second.receipt["result"] = std::move(result);
    }
}
void StudioOperationStore::cancel_all()
{
    for (auto &[id, entry] : entries_)
    {
        static_cast<void>(id);
        static_cast<void>(entry.cancellation.cancel("session_closed"));
    }
}
} // namespace ravo
