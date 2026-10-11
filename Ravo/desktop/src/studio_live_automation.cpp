#include "ravo/desktop/studio_live_session_controller.h"

#include <algorithm>
#include <set>
#include <chrono>
#include <QJsonDocument>
#include <QMetaObject>
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/adapters/text_file.h"
#include "ravo/recipe/develop_patch.h"
#include "studio_control_json.h"
#include "studio_operation_store.h"
#include "studio_qt.h"

namespace ravo
{
using namespace studio_control_internal;
namespace
{
JsonValue batch_json(const DevelopApplyResult &result)
{
    JsonValue::Array items;
    for (const auto &item : result.items)
    {
        JsonValue::Object value{
            {"asset_id", item.asset_id},
            {"status", std::string(develop_apply_item_status_name(item.status))}};
        if (item.history_id)
            value.emplace("history_id", JsonValue::number(std::to_string(*item.history_id)));
        if (item.revision)
            value.emplace("catalog_revision", JsonValue::number(std::to_string(*item.revision)));
        if (item.error)
            value.emplace(
                "error", JsonValue::Object{{"code", std::string(error_code_name(item.error->code))},
                                           {"message", item.error->message}});
        items.emplace_back(std::move(value));
    }
    return JsonValue::Object{{"items", std::move(items)},
                             {"applied", JsonValue::number(std::to_string(result.applied))},
                             {"failed", JsonValue::number(std::to_string(result.failed))},
                             {"skipped", JsonValue::number(std::to_string(result.skipped))},
                             {"partial", result.applied > 0 && result.failed + result.skipped > 0}};
}
} // namespace

Result<JsonValue> StudioLiveSessionController::handleAutomation(const LiveControlRequest &request)
{
    auto params = require_object(request.params, "params");
    if (!params)
        return params.error();
    const auto &object = *params.value();
    if (request.method == "operation")
    {
        auto known = reject_unknown(object, {"action", "operation_id"}, "params");
        if (!known)
            return known.error();
        auto action = string_field(object, "action", 16);
        if (!action)
            return action.error();
        if (action.value() == "reserve")
        {
            if (object.contains("operation_id"))
                return make_error(ErrorCode::kInvalidArgument,
                                  "Reserve does not accept an operation ID");
            return operations_->reserve(descriptor_.session_id);
        }
        auto id = string_field(object, "operation_id", 200);
        if (!id)
            return id.error();
        if (action.value() == "get")
            return operations_->get(id.value());
        if (action.value() == "cancel")
            return operations_->cancel(id.value());
        return make_error(ErrorCode::kInvalidArgument, "Unknown operation action");
    }

    if (request.method == "history")
    {
        auto known = reject_unknown(object,
                                    {"asset_id", "expected_selection_revision",
                                     "expected_session_revision", "expected_recipe_revision",
                                     "expected_catalog_revision", "offset", "limit"},
                                    "params");
        if (!known)
            return known.error();
        auto id = string_field(object, "asset_id", 256);
        auto revision = integer_field<std::uint64_t>(object, "expected_selection_revision");
        if (!id)
            return id.error();
        if (!revision)
            return revision.error();
        if (id.value() != utf8_from_qstring(presenter_.selected_asset_id_) ||
            revision.value() != selection_revision_)
            return make_error(ErrorCode::kConflict, "History selection changed",
                              {{"reason", "stale_selection"}});
        for (const auto &[field, actual] :
             {std::pair{"expected_session_revision", session_revision_},
              std::pair{"expected_recipe_revision", recipe_revision_},
              std::pair{"expected_catalog_revision",
                        static_cast<std::uint64_t>(presenter_.observed_catalog_revision_)}})
        {
            if (!object.contains(field))
                continue;
            auto value = integer_field<std::uint64_t>(object, field);
            if (!value)
                return value.error();
            if (value.value() != actual)
                return make_error(ErrorCode::kConflict, "History binding is stale",
                                  {{"reason", "stale_binding"}});
        }
        if (presenter_.busy() || !presenter_.develop()->state().develop_loaded_)
            return make_error(ErrorCode::kConflict, "History state is busy", {{"reason", "busy"}});
        std::uint64_t offset = 0, limit = 200;
        if (object.contains("offset"))
        {
            auto value = integer_field<std::uint64_t>(object, "offset");
            if (!value)
                return value.error();
            offset = value.value();
        }
        if (object.contains("limit"))
        {
            auto value = integer_field<std::uint64_t>(object, "limit");
            if (!value)
                return value.error();
            limit = value.value();
        }
        if (limit == 0 || limit > 200)
            return make_error(ErrorCode::kInvalidArgument, "History page limit is invalid");
        const auto &entries = presenter_.develop()->state().recipe_history_entries_;
        if (presenter_.develop()->historyLoading())
            return make_error(ErrorCode::kConflict, "History is still loading",
                              {{"reason", "history_loading"}});
        const auto first = std::min<std::uint64_t>(offset, entries.size());
        const auto count = std::min<std::uint64_t>(limit, entries.size() - first);
        JsonValue::Array rows;
        for (auto index = first; index < first + count; ++index)
        {
            const auto &entry = entries[static_cast<std::size_t>(index)];
            rows.push_back(
                JsonValue::Object{{"id", JsonValue::number(std::to_string(entry.id))},
                                  {"seq", JsonValue::number(std::to_string(entry.seq))},
                                  {"kind", entry.kind},
                                  {"label", entry.label ? *entry.label : std::string{}},
                                  {"recipe_sha256", sha256_utf8_hex(entry.recipe_json)}});
        }
        return JsonValue{
            JsonValue::Object{{"type", "ravo.studio.history"},
                              {"version", JsonValue::number("1")},
                              {"asset_id", id.value()},
                              {"entries", std::move(rows)},
                              {"total", JsonValue::number(std::to_string(entries.size()))},
                              {"offset", JsonValue::number(std::to_string(first))},
                              {"has_more", first + count < entries.size()}}};
    }

    auto id = string_field(object, "operation_id", 200);
    if (!id)
        return id.error();
    auto begun = operations_->begin(id.value(), request.params);
    if (!begun)
        return begun.error();
    if (!begun.value())
        return operations_->get(id.value());
    const auto reject = [&](const TaskError &error) -> Result<JsonValue>
    {
        operations_->finish(id.value(), false, 0, 0, std::string(error_code_name(error.code)),
                            error.message);
        auto failure = error;
        failure.context.insert_or_assign("operation_id", id.value());
        failure.context.insert_or_assign("mutation_applied", "false");
        return failure;
    };
    auto kind = string_field(object, "kind", 16);
    if (!kind)
        return reject(kind.error());
    std::set<std::string, std::less<>> allowed{"operation_id",
                                               "kind",
                                               "session_id",
                                               "asset_id",
                                               "expected_session_revision",
                                               "expected_selection_revision",
                                               "expected_recipe_revision",
                                               "expected_catalog_revision"};
    if (kind.value() == "patch" || kind.value() == "batch")
    {
        allowed.insert("fields");
        allowed.insert("local_id");
        allowed.insert("base_recipe_sha256");
        allowed.insert("target_recipe_sha256");
        if (kind.value() == "batch")
            allowed.insert("asset_ids");
    }
    else if (kind.value() == "mask")
    {
        allowed.insert("action");
        allowed.insert("arguments");
    }
    else if (kind.value() == "restore")
        allowed.insert("history_id");
    else
        return reject(make_error(ErrorCode::kInvalidArgument, "Unknown automation command kind"));
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (!allowed.contains(key))
            return reject(make_error(ErrorCode::kInvalidArgument,
                                     "Unknown automation command field", {{"field", key}}));
    }
    auto asset = string_field(object, "asset_id", 256);
    auto session_id = string_field(object, "session_id", 128);
    auto session = integer_field<std::uint64_t>(object, "expected_session_revision");
    auto selection = integer_field<std::uint64_t>(object, "expected_selection_revision");
    auto recipe = integer_field<std::uint64_t>(object, "expected_recipe_revision");
    auto catalog = integer_field<std::int64_t>(object, "expected_catalog_revision");
    if (!asset)
        return reject(asset.error());
    if (!session_id)
        return reject(session_id.error());
    if (!session)
        return reject(session.error());
    if (!selection)
        return reject(selection.error());
    if (!recipe)
        return reject(recipe.error());
    if (!catalog)
        return reject(catalog.error());
    if (session_id.value() != descriptor_.session_id ||
        asset.value() != utf8_from_qstring(presenter_.selected_asset_id_) ||
        session.value() != session_revision_ || selection.value() != selection_revision_ ||
        recipe.value() != recipe_revision_ ||
        catalog.value() != presenter_.observed_catalog_revision_)
        return reject(make_error(ErrorCode::kConflict, "Automation binding is stale",
                                 {{"reason", "stale_binding"}}));
    if (presenter_.busy() || presenter_.imports()->importPageOpen() || commands_.modalOpen() ||
        presenter_.develop()->historyLoading() || commands_.settingsOpen() ||
        !presenter_.develop()->state().develop_loaded_ ||
        presenter_.develop()->state().develop_job_in_flight_ ||
        presenter_.develop()->state().pending_save_ ||
        presenter_.develop()->state().pending_preview_)
        return reject(make_error(ErrorCode::kConflict, "Studio command is busy or unavailable",
                                 {{"reason", "busy"}}));
    const auto token = operations_->token(id.value());
    if (!token)
        return reject(token.error());
    const auto cancellation = token.value();
    Result<bool> applied = false;
    if (kind.value() == "patch" || kind.value() == "batch")
    {
        const auto *fields = request.params.find("fields");
        if (!fields)
            return reject(make_error(ErrorCode::kInvalidArgument, "Develop fields are required"));
        std::string local_id;
        if (object.contains("local_id"))
        {
            auto local = string_field(object, "local_id", 256);
            if (!local)
                return reject(local.error());
            local_id = local.value();
        }
        if (object.contains("base_recipe_sha256") || object.contains("target_recipe_sha256"))
        {
            auto base_hash = string_field(object, "base_recipe_sha256", 64);
            auto target_hash = string_field(object, "target_recipe_sha256", 64);
            if (!base_hash)
                return reject(base_hash.error());
            if (!target_hash)
                return reject(target_hash.error());
            if (sha256_utf8_hex(current_recipe_json_) != base_hash.value())
                return reject(make_error(ErrorCode::kConflict, "Candidate base recipe changed",
                                         {{"reason", "stale_candidate"}}));
            auto base = parse_recipe_json(current_recipe_json_);
            if (!base)
                return reject(base.error());
            auto base_params = develop_from_recipe(base.value());
            if (!base_params)
                return reject(base_params.error());
            auto changed = apply_develop_patch(base_params.value(), *fields, local_id);
            if (!changed)
                return reject(changed.error());
            auto target = recipe_from_develop(base.value().asset, changed.value());
            if (!target)
                return reject(target.error());
            auto serialized = serialize_recipe(target.value());
            if (!serialized)
                return reject(serialized.error());
            if (sha256_utf8_hex(serialized.value()) != target_hash.value())
                return reject(make_error(ErrorCode::kConflict,
                                         "Candidate target recipe does not match",
                                         {{"reason", "invalid_candidate"}}));
        }
        if (kind.value() == "batch")
        {
            if (!local_id.empty() || presenter_.develop()->localEditing() ||
                current_recipe_json_ != saved_recipe_json_)
                return reject(make_error(ErrorCode::kConflict,
                                         "Batch requires a saved global editing context",
                                         {{"reason", "pending_recipe"}}));
            const auto *assets = request.params.find("asset_ids");
            if (!assets || !assets->array_if() || assets->array_if()->empty() ||
                assets->array_if()->size() > 256)
                return reject(make_error(ErrorCode::kInvalidArgument,
                                         "Batch requires 1 through 256 explicit selected IDs"));
            std::vector<std::string> ids;
            std::set<std::string> seen;
            for (const auto &value : *assets->array_if())
            {
                if (!value.string_if() || !presenter_.selected_ids_.contains(*value.string_if()) ||
                    !seen.insert(*value.string_if()).second)
                    return reject(
                        make_error(ErrorCode::kConflict,
                                   "Batch contains an unconfirmed or duplicate selection ID"));
                ids.push_back(*value.string_if());
            }
            presenter_.setBusy(true);
            batch_active_ = true;
            const auto path = presenter_.catalog_path_;
            const auto operation_id = id.value();
            const auto patch = *fields;
            const auto expected = catalog.value();
            const bool queued = presenter_.executor_.post(
                [this, operation_id, ids, patch, expected, cancellation, path]
                {
                    Result<DevelopApplyResult> result =
                        make_error(ErrorCode::kIo, "Catalog session is closed");
                    if (presenter_.service_)
                    {
                        DevelopApplyResult completed;
                        auto last = std::chrono::steady_clock::time_point{};
                        result = presenter_.service_->develop().apply_patch_selection(
                            ids, patch, expected, cancellation,
                            [this, operation_id, &completed,
                             &last](std::size_t done, std::size_t total,
                                    const DevelopApplyItemResult *item)
                            {
                                if (item)
                                {
                                    completed.items.push_back(*item);
                                    if (item->status == DevelopApplyItemStatus::kApplied)
                                        ++completed.applied;
                                    else if (item->status == DevelopApplyItemStatus::kFailed)
                                        ++completed.failed;
                                    else
                                        ++completed.skipped;
                                }
                                const auto now = std::chrono::steady_clock::now();
                                if (done != total && now - last < std::chrono::milliseconds(100))
                                    return;
                                last = now;
                                auto value = batch_json(completed);
                                QMetaObject::invokeMethod(
                                    this, [this, operation_id, value = std::move(value)]() mutable
                                    { operations_->progress(operation_id, std::move(value)); },
                                    Qt::QueuedConnection);
                            });
                    }
                    QMetaObject::invokeMethod(
                        this,
                        [this, operation_id, path, ids, cancellation,
                         result = std::move(result)]() mutable
                        {
                            if (!presenter_alive_)
                                return;
                            presenter_.setBusy(false);
                            batch_active_ = false;
                            if (!result)
                                operations_->finish(
                                    operation_id, false, 0, 0,
                                    std::string(error_code_name(result.error().code)),
                                    result.error().message);
                            else
                            {
                                const bool committed = result.value().applied > 0;
                                operations_->finish(
                                    operation_id, committed, result.value().revision, 0,
                                    committed                                ? "" :
                                    cancellation.is_cancellation_requested() ? "cancelled" :
                                                                               "batch_failed",
                                    committed ? "" : "No batch photos were committed");
                                operations_->progress(operation_id, batch_json(result.value()));
                                if (presenter_.catalog_path_ == path && committed)
                                {
                                    presenter_.observed_catalog_revision_ =
                                        std::max(presenter_.observed_catalog_revision_,
                                                 result.value().revision);
                                    presenter_.reloadVisibleAssets();
                                    if (std::find(ids.begin(), ids.end(),
                                                  utf8_from_qstring(
                                                      presenter_.selected_asset_id_)) != ids.end())
                                    {
                                        presenter_.develop()->load_develop_for_selection();
                                        presenter_.requestPreviewForSelection();
                                    }
                                }
                            }
                            refresh();
                        },
                        Qt::QueuedConnection);
                });
            if (!queued)
            {
                presenter_.setBusy(false);
                batch_active_ = false;
                return reject(make_error(ErrorCode::kIo, "Catalog executor is stopped"));
            }
            return operations_->get(id.value());
        }
        applied = commands_.applyDevelopPatch(*fields, local_id, id.value(), cancellation,
                                              catalog.value());
    }
    else if (kind.value() == "restore")
    {
        auto history = integer_field<std::int64_t>(object, "history_id");
        if (!history)
            return reject(history.error());
        applied = commands_.restoreDevelopHistory(history.value(), id.value(), cancellation,
                                                  catalog.value());
    }
    else
    {
        auto action = string_field(object, "action", 64);
        const auto *args = request.params.find("arguments");
        if (!action)
            return reject(action.error());
        if (!args || !args->object_if() || serialize_json(*args).size() > 65536)
            return reject(
                make_error(ErrorCode::kInvalidArgument, "Mask arguments must be a bounded object"));
        auto map = QJsonDocument::fromJson(QByteArray::fromStdString(serialize_json(*args)))
                       .toVariant()
                       .toMap();
        applied = commands_.applyLocalAdjustment(qstring_from_utf8(action.value()), map, id.value(),
                                                 cancellation, catalog.value());
    }
    if (!applied)
        return reject(applied.error());
    if (!applied.value())
    {
        operations_->finish(id.value(), true, presenter_.observed_catalog_revision_,
                            presenter_.develop()->state().loaded_recipe_history_head_);
        operations_->progress(id.value(), JsonValue::Object{{"changed", false}});
    }
    else if (!presenter_.develop()->controlSaveQueued(id.value()))
        operations_->stage(id.value(), snapshot(0, 0), true);
    else
    {
        single_active_id_ = id.value();
        presenter_.setBusy(true);
    }
    refresh();
    return operations_->get(id.value());
}
} // namespace ravo
