#include "ravo/desktop/studio_live_session_controller.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUuid>
#include <QJsonDocument>

#include "ravo/desktop/studio_command_controller.h"
#include "studio_command_ids.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/parse_number.h"
#include "ravo/recipe/recipe.h"

#include "studio_qt.h"
#include "studio_operation_store.h"
#include "studio_control_json.h"
#include "ravo/services/asset_annotation.h"

namespace ravo
{
namespace
{

using namespace studio_control_internal;

[[nodiscard]] std::string operation_key(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (object == nullptr)
        return {};
    const auto id = object->find("id");
    const auto instance = object->find("instance_id");
    if (id == object->end() || instance == object->end() || id->second.string_if() == nullptr ||
        instance->second.string_if() == nullptr)
        return {};
    return *id->second.string_if() + "\n" + *instance->second.string_if();
}

[[nodiscard]] JsonValue changed_operations(const JsonValue &current, const JsonValue &baseline)
{
    JsonValue::Array changed;
    const auto *current_operations = current.find("operations");
    const auto *baseline_operations = baseline.find("operations");
    if (current_operations == nullptr || current_operations->array_if() == nullptr)
        return changed;
    std::map<std::string, JsonValue, std::less<>> baseline_by_key;
    if (baseline_operations != nullptr && baseline_operations->array_if() != nullptr)
    {
        for (const auto &operation : *baseline_operations->array_if())
        {
            baseline_by_key.emplace(operation_key(operation), operation);
        }
    }
    std::set<std::string, std::less<>> current_keys;
    for (const auto &operation : *current_operations->array_if())
    {
        const auto key = operation_key(operation);
        current_keys.insert(key);
        const auto found = baseline_by_key.find(key);
        if (key.empty() || found == baseline_by_key.end() ||
            serialize_json(found->second) != serialize_json(operation))
        {
            auto item =
                operation.object_if() != nullptr ? *operation.object_if() : JsonValue::Object{};
            item.emplace("change", found == baseline_by_key.end() ? "added" : "modified");
            changed.emplace_back(std::move(item));
        }
    }
    for (const auto &[key, operation] : baseline_by_key)
    {
        if (key.empty() || current_keys.contains(key))
            continue;
        const auto *object = operation.object_if();
        if (object == nullptr)
            continue;
        JsonValue::Object removed{{"change", "removed"}};
        if (const auto id = object->find("id"); id != object->end())
            removed.emplace("id", id->second);
        if (const auto instance = object->find("instance_id"); instance != object->end())
            removed.emplace("instance_id", instance->second);
        changed.emplace_back(std::move(removed));
    }
    return changed;
}

[[nodiscard]] JsonValue string_array(const std::vector<std::string> &values)
{
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto &value : values)
        result.emplace_back(value);
    return result;
}

[[nodiscard]] std::string canonical_path(const QString &path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(
        filesystem_path_from_utf8(utf8_from_qstring(path)), error);
    return error ? utf8_from_qstring(QFileInfo(path).absoluteFilePath()) :
                   filesystem_path_to_utf8(canonical);
}

} // namespace

StudioLiveSessionController::StudioLiveSessionController(StudioPresenter &presenter,
                                                         StudioCommandController &commands,
                                                         QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , commands_(commands)
    , operations_(std::make_unique<StudioOperationStore>())
{
}

StudioLiveSessionController::~StudioLiveSessionController()
{
    server_.reset();
    operations_->cancel_all();
    static_cast<void>(annotation_binding_cancel_.cancel("annotation_owner_closed"));
    if (presenter_alive_)
    {
        // Batch/selection closures borrow this controller; single Develop jobs belong to
        // the presenter and retain their cancellation token independently.
        if (batch_active_ || selection_active_ || annotation_active_)
            presenter_.executor_.wait_idle();
        if (batch_active_ || selection_active_ || annotation_active_ || !single_active_id_.empty())
            presenter_.setBusy(false);
    }
}

Result<std::unique_ptr<StudioLiveSessionController>>
StudioLiveSessionController::create(StudioPresenter &presenter, StudioCommandController &commands,
                                    QObject *parent)
{
    auto controller = std::unique_ptr<StudioLiveSessionController>(
        new StudioLiveSessionController(presenter, commands, parent));
    auto started = controller->start();
    if (!started)
        return started.error();
    return controller;
}

Result<void> StudioLiveSessionController::start()
{
    connect(&presenter_, &StudioPresenter::aboutToShutdown, this,
            [this]
            {
                server_.reset();
                operations_->cancel_all();
                // Presenter cancels all of its task owners and joins the executor in
                // its destructor body, before any referenced member is destroyed.
                presenter_alive_ = false;
            });
    descriptor_.session_id = utf8_from_qstring(QUuid::createUuid().toString(QUuid::WithoutBraces));
    descriptor_.process_id = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    descriptor_.executable_path = canonical_path(QCoreApplication::applicationFilePath());
    auto workspace = find_ravo_workspace_root(descriptor_.executable_path);
    if (!workspace)
        return workspace.error();
    if (!workspace.value())
    {
        workspace = find_ravo_workspace_root(utf8_from_qstring(QDir::currentPath()));
        if (!workspace)
            return workspace.error();
    }
    descriptor_.workspace_root =
        workspace.value() ? filesystem_path_to_utf8(*workspace.value()) : std::string{};
    descriptor_.server_name = "ravo-studio-" + descriptor_.session_id;

    const auto changed = [this]() { refresh(); };
    connect(&presenter_, &StudioPresenter::catalogChanged, this, changed);
    connect(&presenter_, &StudioPresenter::busyChanged, this, changed);
    connect(&presenter_, &StudioPresenter::libraryWorkChanged, this, changed);
    connect(presenter_.imports(), &StudioImportWorkspace::libraryWorkChanged, this, changed);
    connect(&presenter_, &StudioPresenter::selectionChanged, this, changed);
    connect(presenter_.video(), &StudioVideoPresenter::changed, this, changed);
    connect(&presenter_, &StudioPresenter::browseModeChanged, this, changed);
    connect(presenter_.develop(), &StudioDevelopPresenter::editChanged, this, changed);
    connect(presenter_.inspect(), &StudioInspectPresenter::previewChanged, this, changed);
    connect(presenter_.inspect(), &StudioInspectPresenter::identityChanged, this, changed);
    connect(&commands_, &StudioCommandController::commandsChanged, this, changed);
    connect(commands_.backupSettings(), &StudioBackupSettings::changed, this, changed);
    refresh();

    connect(presenter_.develop(), &StudioDevelopPresenter::controlCommitFinished, this,
            [this](const QString &id, bool committed, qlonglong revision, qlonglong history,
                   const QString &code, const QString &message)
            {
                operations_->finish(utf8_from_qstring(id), committed, revision, history,
                                    utf8_from_qstring(code), utf8_from_qstring(message));
                if (single_active_id_ == utf8_from_qstring(id))
                {
                    single_active_id_.clear();
                    presenter_.setBusy(false);
                }
                refresh();
            });

    auto server = LocalControlServer::start(descriptor_, [this](const LiveControlRequest &request)
                                            { return handle(request); });
    if (!server)
        return server.error();
    server_ = std::move(server).value();
    return {};
}

const LiveSessionDescriptor &StudioLiveSessionController::descriptor() const noexcept
{
    return descriptor_;
}

const std::filesystem::path &StudioLiveSessionController::descriptorPath() const noexcept
{
    return server_->descriptor_path();
}

void StudioLiveSessionController::refresh()
{
    if (!presenter_alive_)
        return;
    std::string selection = utf8_from_qstring(presenter_.selected_asset_id_);
    for (const auto &id : presenter_.selected_asset_ids())
    {
        selection.push_back('\n');
        selection.append(id);
    }
    if (selection != selection_identity_)
    {
        selection_identity_ = std::move(selection);
        ++selection_revision_;
    }

    std::string current;
    std::string saved;
    std::string baseline;
    std::string failure;
    const auto asset = presenter_.assets_.assetById(presenter_.selected_asset_id_);
    if (asset && presenter_.develop()->state().develop_loaded_)
    {
        const AssetDescriptor descriptor{asset->id, asset->normalized_uri,
                                         asset->content_fingerprint};
        auto current_recipe =
            recipe_from_develop(descriptor, presenter_.develop()->state().develop_);
        auto saved_recipe =
            recipe_from_develop(descriptor, presenter_.develop()->state().saved_develop_);
        auto baseline_recipe =
            recipe_from_develop(descriptor, presenter_.develop()->baseline_develop());
        if (!current_recipe)
            failure = current_recipe.error().message;
        else if (!saved_recipe)
            failure = saved_recipe.error().message;
        else if (!baseline_recipe)
            failure = baseline_recipe.error().message;
        else
        {
            auto current_text = serialize_recipe(current_recipe.value());
            auto saved_text = serialize_recipe(saved_recipe.value());
            auto baseline_text = serialize_recipe(baseline_recipe.value());
            if (!current_text)
                failure = current_text.error().message;
            else if (!saved_text)
                failure = saved_text.error().message;
            else if (!baseline_text)
                failure = baseline_text.error().message;
            else
            {
                current = std::move(current_text).value();
                saved = std::move(saved_text).value();
                baseline = std::move(baseline_text).value();
            }
        }
    }
    else if (!presenter_.develop()->state().develop_load_error_.isEmpty())
    {
        failure = utf8_from_qstring(presenter_.develop()->state().develop_load_error_);
    }
    if (current != current_recipe_json_)
    {
        current_recipe_json_ = std::move(current);
        ++recipe_revision_;
    }
    if (saved != saved_recipe_json_)
    {
        saved_recipe_json_ = std::move(saved);
        ++saved_recipe_revision_;
    }
    baseline_recipe_json_ = std::move(baseline);
    recipe_error_ = std::move(failure);

    // Control revisions describe mutation preconditions. Frame publication and
    // playback timestamps have their own revisions and must not stale an
    // otherwise unchanged candidate while an immutable artifact is encoded.
    auto video = *presenter_.video()->jsonSnapshot().object_if();
    video.erase("position_ms");
    const auto identity = serialize_json(JsonValue::Object{
        {"catalog", utf8_from_qstring(presenter_.catalog_path_)},
        {"catalog_revision",
         JsonValue::number(std::to_string(presenter_.observed_catalog_revision_))},
        {"selection", selection_identity_},
        {"mode", utf8_from_qstring(presenter_.browse_mode_)},
        {"busy", presenter_.busy() || presenter_.imports()->importWorkActive() ||
                     presenter_.develop()->historyLoading() ||
                     presenter_.develop()->state().develop_job_in_flight_ ||
                     presenter_.develop()->state().pending_save_.has_value() ||
                     presenter_.develop()->state().pending_preview_.has_value()},
        {"loaded", presenter_.develop()->state().develop_loaded_},
        {"recipe", JsonValue::number(std::to_string(recipe_revision_))},
        {"saved_recipe", JsonValue::number(std::to_string(saved_recipe_revision_))},
        {"history_cursor",
         JsonValue::number(std::to_string(presenter_.develop()->state().active_history_id_))},
        {"history_sequence",
         JsonValue::number(std::to_string(presenter_.develop()->state().active_history_seq_))},
        {"local_id", utf8_from_qstring(presenter_.develop()->activeLocalId())},
        {"gesture", utf8_from_qstring(presenter_.develop()->state().mask_gesture_token_)},
        {"modal", commands_.modalOpen()},
        {"settings", commands_.settingsOpen()},
        {"import_open", presenter_.imports()->importPageOpen()},
        {"video", std::move(video)}});
    if (identity != video_control_identity_)
    {
        ++session_revision_;
        video_control_identity_ = identity;
    }
    if (annotation_active_ && annotation_session_revision_ &&
        *annotation_session_revision_ != session_revision_)
        static_cast<void>(annotation_binding_cancel_.cancel("stale_annotation_binding"));

    const std::string preview = std::to_string(presenter_.inspect()->frameRevision()) + "\n" +
                                utf8_from_qstring(presenter_.inspect()->pixelSha256()) + "\n" +
                                std::to_string(presenter_.inspect()->previewLoading() ? 1 : 0) +
                                "\n" +
                                std::to_string(presenter_.inspect()->identityPending() ? 1 : 0);
    if (preview != preview_identity_)
    {
        preview_identity_ = preview;
        ++preview_state_revision_;
    }
}

JsonValue StudioLiveSessionController::snapshot(const std::size_t selection_offset,
                                                const std::size_t selection_limit) const
{
    JsonValue::Object catalog{{"open", presenter_.catalogOpen()},
                              {"path", utf8_from_qstring(presenter_.catalog_path_)},
                              {"revision", JsonValue::number(std::to_string(std::max<std::int64_t>(
                                               presenter_.observed_catalog_revision_, 0)))}};

    const auto selected_ids = presenter_.selected_asset_ids();
    const auto first = std::min(selection_offset, selected_ids.size());
    const auto count = std::min({selection_limit, std::size_t{512}, selected_ids.size() - first});
    std::vector<std::string> projected(selected_ids.begin() + static_cast<std::ptrdiff_t>(first),
                                       selected_ids.begin() +
                                           static_cast<std::ptrdiff_t>(first + count));
    JsonValue::Object selection{
        {"asset_ids", string_array(projected)},
        {"total", JsonValue::number(std::to_string(selected_ids.size()))},
        {"offset", JsonValue::number(std::to_string(first))},
        {"truncated", count != selected_ids.size()},
        {"has_more", first + count < selected_ids.size()},
        {"resolving", presenter_.selectionResolving()},
        {"schema_version", JsonValue::number("2")},
        {"primary_asset_id", utf8_from_qstring(presenter_.selected_asset_id_)},
        {"revision", JsonValue::number(std::to_string(selection_revision_))},
    };
    if (const auto asset = presenter_.assets_.assetById(presenter_.selected_asset_id_); asset)
    {
        selection.emplace("display_name", utf8_from_qstring(presenter_.selectedDisplayName()));
        selection.emplace("has_edits", asset->has_edits);
        selection.emplace("media_type", asset->media_type);
        selection.emplace("uri", asset->normalized_uri);
        selection.emplace("annotations", asset_annotation_state_json(*asset));
    }

    JsonValue current{nullptr};
    JsonValue saved{nullptr};
    JsonValue baseline{nullptr};
    if (!current_recipe_json_.empty())
    {
        auto parsed = parse_json(current_recipe_json_);
        if (parsed)
            current = std::move(parsed).value();
    }
    if (!saved_recipe_json_.empty())
    {
        auto parsed = parse_json(saved_recipe_json_);
        if (parsed)
            saved = std::move(parsed).value();
    }
    if (!baseline_recipe_json_.empty())
    {
        auto parsed = parse_json(baseline_recipe_json_);
        if (parsed)
            baseline = std::move(parsed).value();
    }
    const bool selected = !presenter_.selected_asset_id_.isEmpty();
    const bool available =
        selected && presenter_.develop()->state().develop_loaded_ && current.object_if() != nullptr;
    const bool pending = available && current_recipe_json_ != saved_recipe_json_;
    JsonValue::Object recipe{
        {"current", current},
        {"modified_operations",
         available ? changed_operations(current, baseline) : JsonValue{JsonValue::Array{}}},
        {"pending_changes", pending},
        {"pending_operations",
         available ? changed_operations(current, saved) : JsonValue{JsonValue::Array{}}},
        {"revision", JsonValue::number(std::to_string(recipe_revision_))},
        {"saved", saved},
        {"saved_revision", JsonValue::number(std::to_string(saved_recipe_revision_))},
        {"state", !selected ? "none" :
                  presenter_.selectedMediaType().startsWith(QLatin1String("video/")) ?
                              "not_applicable" :
                  !recipe_error_.empty()                         ? "error" :
                  !presenter_.develop()->state().develop_loaded_ ? "loading" :
                  pending                                        ? "pending" :
                                                                   "saved"},
    };
    if (!recipe_error_.empty())
        recipe.emplace("error", recipe_error_);

    const bool preview_ready = !presenter_.inspect()->pixelSha256().isEmpty() &&
                               presenter_.inspect()->frameWidth() > 0U &&
                               presenter_.inspect()->frameHeight() > 0U;
    JsonValue::Array mask_handles;
    for (const auto &value : presenter_.develop()->localMaskHandles())
    {
        const auto handle = value.toMap();
        mask_handles.emplace_back(JsonValue::Object{
            {"id", utf8_from_qstring(handle.value("id").toString())},
            {"x", JsonValue::number(
                      QByteArray::number(handle.value("x").toDouble(), 'g', 17).toStdString())},
            {"y", JsonValue::number(
                      QByteArray::number(handle.value("y").toDouble(), 'g', 17).toStdString())}});
    }
    JsonValue::Object preview{
        {"color_profile", utf8_from_qstring(presenter_.inspect()->frameColorProfile())},
        {"height", JsonValue::number(std::to_string(presenter_.inspect()->frameHeight()))},
        {"matches_current_recipe",
         available && presenter_.inspect()->displayedDevelop().has_value() &&
             *presenter_.inspect()->displayedDevelop() == presenter_.develop()->state().develop_ &&
             !presenter_.develop()->state().before_after_ &&
             !presenter_.develop()->state().crop_tool_active_ &&
             !presenter_.develop()->state().mask_overlay_visible_},
        {"pixel_format", "rgb8"},
        {"pixel_sha256", utf8_from_qstring(presenter_.inspect()->pixelSha256())},
        {"resource_id",
         preview_ready ? descriptor_.session_id +
                             ":preview:" + std::to_string(presenter_.inspect()->frameRevision()) +
                             ":" + utf8_from_qstring(presenter_.inspect()->pixelSha256()) :
                         std::string{}},
        {"revision", JsonValue::number(std::to_string(preview_state_revision_))},
        {"source_revision",
         JsonValue::number(std::to_string(presenter_.inspect()->frameRevision()))},
        {"state",
         presenter_.inspect()->previewLoading() || presenter_.inspect()->identityPending() ?
             "loading" :
         preview_ready ? "ready" :
                         "none"},
        {"width", JsonValue::number(std::to_string(presenter_.inspect()->frameWidth()))},
    };

    return JsonValue::Object{
        {"browse_mode", utf8_from_qstring(presenter_.browse_mode_)},
        {"busy", presenter_.busy() || presenter_.import_workspace_->importWorkActive() ||
                     presenter_.develop()->historyLoading() ||
                     presenter_.develop()->state().develop_job_in_flight_ ||
                     presenter_.develop()->state().pending_save_.has_value() ||
                     presenter_.develop()->state().pending_preview_.has_value()},
        {"catalog", std::move(catalog)},
        {"command_revision", JsonValue::number(std::to_string(commands_.revision()))},
        {"error", utf8_from_qstring(presenter_.error_text_)},
        {"executable_path", descriptor_.executable_path},
        {"preview", std::move(preview)},
        {"video", presenter_.video()->jsonSnapshot()},
        {"import", presenter_.imports()->jsonSnapshot()},
        {"settings", JsonValue::Object{{"schema", "ravo.studio.settings/v1"},
                                       {"open", commands_.settingsOpen()},
                                       {"section", utf8_from_qstring(commands_.settingsSection())},
                                       {"backup", commands_.backupSettings()->jsonSnapshot()}}},
        {"process_id", JsonValue::number(std::to_string(descriptor_.process_id))},
        {"protocol", descriptor_.protocol},
        {"recipe", std::move(recipe)},
        {"revision", JsonValue::number(std::to_string(session_revision_))},
        {"schema_version", JsonValue::number("1")},
        {"editing_scope",
         JsonValue::Object{
             {"schema_version", JsonValue::number("1")},
             {"kind", presenter_.develop()->localEditing() ? "local" : "global"},
             {"local_id", utf8_from_qstring(presenter_.develop()->activeLocalId())},
             {"done_pending", presenter_.develop()->localDonePending()},
             {"component_index",
              JsonValue::number(std::to_string(
                  presenter_.develop()->state().local_projection_.local_mask_child_index))},
             {"point_index",
              JsonValue::number(std::to_string(
                  presenter_.develop()->state().local_projection_.local_mask_point_index))},
             {"drawing", presenter_.develop()->maskDrawingActive()},
             {"overlay_requested", presenter_.develop()->maskOverlayVisible()},
             {"overlay_active", presenter_.develop()->maskOverlayActive()},
             {"gesture_token",
              utf8_from_qstring(presenter_.develop()->state().mask_gesture_token_)},
             {"coordinate_space", "normalized_developed_image"},
             {"handles", std::move(mask_handles)}}},
        {"selection", std::move(selection)},
        {"selection_resolving", presenter_.selectionResolving()},
        {"binding",
         JsonValue::Object{
             {"session_id", descriptor_.session_id},
             {"asset_id", utf8_from_qstring(presenter_.selected_asset_id_)},
             {"expected_session_revision", JsonValue::number(std::to_string(session_revision_))},
             {"expected_selection_revision",
              JsonValue::number(std::to_string(selection_revision_))},
             {"expected_recipe_revision", JsonValue::number(std::to_string(recipe_revision_))},
             {"expected_catalog_revision", JsonValue::number(std::to_string(std::max<std::int64_t>(
                                               0, presenter_.observed_catalog_revision_)))}}},
        {"session_id", descriptor_.session_id},
        {"status", utf8_from_qstring(presenter_.status_text_)},
        {"type", "ravo.studio.session"},
        {"workspace_root", descriptor_.workspace_root},
    };
}

Result<JsonValue> StudioLiveSessionController::handle(const LiveControlRequest &request)
{
    if (request.method == "state")
    {
        auto params = require_object(request.params, "params");
        if (!params)
            return params.error();
        auto known =
            reject_unknown(*params.value(), {"selection_offset", "selection_limit"}, "params");
        if (!known)
            return known.error();
        std::uint64_t offset = 0, limit = 512;
        if (params.value()->contains("selection_offset"))
        {
            auto value = integer_field<std::uint64_t>(*params.value(), "selection_offset");
            if (!value)
                return value.error();
            offset = value.value();
        }
        if (params.value()->contains("selection_limit"))
        {
            auto value = integer_field<std::uint64_t>(*params.value(), "selection_limit");
            if (!value)
                return value.error();
            limit = value.value();
        }
        if (limit > 512 || offset > static_cast<std::uint64_t>(SIZE_MAX))
            return make_error(ErrorCode::kInvalidArgument,
                              "Selection projection is outside its bound");
        return snapshot(static_cast<std::size_t>(offset), static_cast<std::size_t>(limit));
    }
    if (request.method == "operation" || request.method == "commit" || request.method == "history")
        return handleAutomation(request);
    if (request.method == "video")
    {
        auto params = require_object(request.params, "params");
        if (!params)
            return params.error();
        auto known = reject_unknown(*params.value(),
                                    {"asset_id", "action", "value", "expected_session_revision",
                                     "expected_selection_revision", "expected_video_generation"},
                                    "params");
        if (!known)
            return known.error();
        auto asset = string_field(*params.value(), "asset_id", 256);
        auto action = string_field(*params.value(), "action", 32);
        auto session = integer_field<std::uint64_t>(*params.value(), "expected_session_revision");
        auto selection =
            integer_field<std::uint64_t>(*params.value(), "expected_selection_revision");
        auto generation =
            integer_field<std::uint64_t>(*params.value(), "expected_video_generation");
        if (!asset || !action || !session || !selection || !generation)
            return make_error(ErrorCode::kInvalidArgument,
                              "Video request requires target, action and revisions",
                              {{"reason", "invalid_video_request"}});
        if (asset.value() != utf8_from_qstring(presenter_.selected_asset_id_) ||
            session.value() != session_revision_ || selection.value() != selection_revision_ ||
            generation.value() != presenter_.video()->snapshot().value("generation").toULongLong())
            return make_error(ErrorCode::kConflict, "Video request is stale",
                              {{"reason", "stale_video_request"}});
        const auto found = params.value()->find("value");
        QVariant value;
        if (found != params.value()->end())
        {
            if (const auto *boolean = found->second.boolean_if())
                value = *boolean;
            else
            {
                auto number = number_value(found->second, "value");
                if (!number)
                    return number.error();
                value = number.value();
            }
        }
        const char *id = nullptr;
        if (action.value() == "play")
            id = command::kVideoPlay;
        else if (action.value() == "pause")
            id = command::kVideoPause;
        else if (action.value() == "seek")
            id = command::kVideoSeek;
        else if (action.value() == "volume")
            id = command::kVideoVolume;
        else if (action.value() == "mute")
            id = command::kVideoMute;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown video action",
                              {{"reason", "invalid_video_action"}});
        const auto dispatched =
            commands_.executeCommand(QLatin1String(id), value, QStringLiteral("control"));
        if (!dispatched.value("accepted").toBool())
            return make_error(
                dispatched.value("code").toString() == QLatin1String("invalid_argument") ?
                    ErrorCode::kInvalidArgument :
                    ErrorCode::kConflict,
                utf8_from_qstring(dispatched.value("message").toString()),
                {{"reason", "video_command_rejected"},
                 {"command_code", utf8_from_qstring(dispatched.value("code").toString())}});
        auto state = snapshot();
        auto response = *state.object_if();
        response.emplace("mutation",
                         JsonValue::Object{{"accepted", true}, {"action", action.value()}});
        return JsonValue{std::move(response)};
    }
    if ((request.method == "mask" || request.method == "develop") &&
        presenter_.selectedMediaType().startsWith(QLatin1String("video/")))
        return make_error(ErrorCode::kUnsupported, "Video does not support photo editing",
                          {{"reason", "video_photo_operation_unsupported"}});
    if (request.method == "mask")
    {
        auto params = require_object(request.params, "params");
        if (!params)
            return params.error();
        auto known =
            reject_unknown(*params.value(),
                           {"asset_id", "expected_session_revision", "expected_selection_revision",
                            "expected_recipe_revision", "action", "arguments"},
                           "params");
        if (!known)
            return known.error();
        auto asset = string_field(*params.value(), "asset_id", 256U);
        auto action = string_field(*params.value(), "action", 64U);
        auto session = integer_field<std::uint64_t>(*params.value(), "expected_session_revision");
        auto selection =
            integer_field<std::uint64_t>(*params.value(), "expected_selection_revision");
        auto recipe = integer_field<std::uint64_t>(*params.value(), "expected_recipe_revision");
        const auto arguments = params.value()->find("arguments");
        if (!asset || !action || !session || !selection || !recipe ||
            arguments == params.value()->end() || !arguments->second.object_if())
            return make_error(
                ErrorCode::kInvalidArgument,
                "Mask request requires explicit target, revisions, action and arguments",
                {{"reason", "invalid_mask_request"}});
        if (asset.value() != utf8_from_qstring(presenter_.selected_asset_id_) ||
            session.value() != session_revision_ || selection.value() != selection_revision_ ||
            recipe.value() != recipe_revision_)
            return make_error(ErrorCode::kConflict, "Mask request is stale",
                              {{"reason", "stale_mask_request"}});
        const auto text = serialize_json(arguments->second);
        if (text.size() > 65536)
            return make_error(ErrorCode::kInvalidArgument, "Mask arguments exceed the limit");
        auto map = QJsonDocument::fromJson(QByteArray::fromStdString(text)).toVariant().toMap();
        auto applied = commands_.applyLocalAdjustment(qstring_from_utf8(action.value()), map);
        if (!applied)
            return applied.error();
        auto state = snapshot();
        auto object = *state.object_if();
        object.emplace("mutation",
                       JsonValue::Object{{"applied", applied.value()}, {"action", action.value()}});
        return JsonValue{std::move(object)};
    }
    if (request.method != "develop")
    {
        return make_error(ErrorCode::kUnsupported, "Studio live-control method is unsupported",
                          {{"method", request.method}});
    }
    auto params = require_object(request.params, "params");
    if (!params)
        return params.error();
    auto known = reject_unknown(*params.value(),
                                {"asset_id", "expected_selection_revision",
                                 "expected_session_revision", "fields", "operation_id"},
                                "params");
    if (!known)
        return known.error();
    auto asset_id = string_field(*params.value(), "asset_id", 256U);
    auto expected_session =
        integer_field<std::uint64_t>(*params.value(), "expected_session_revision");
    auto expected_selection =
        integer_field<std::uint64_t>(*params.value(), "expected_selection_revision");
    const auto fields_value = params.value()->find("fields");
    if (!asset_id)
        return asset_id.error();
    if (!expected_session)
        return expected_session.error();
    if (!expected_selection)
        return expected_selection.error();
    if (fields_value == params.value()->end())
        return make_error(ErrorCode::kInvalidArgument, "Studio Develop fields are missing");
    if (!fields_value->second.array_if())
        return make_error(ErrorCode::kInvalidArgument, "Develop fields must be an array");

    if (expected_session.value() != session_revision_)
    {
        return make_error(ErrorCode::kConflict, "Studio session revision is stale",
                          {{"reason", "stale_session"},
                           {"expected", std::to_string(expected_session.value())},
                           {"actual", std::to_string(session_revision_)}});
    }
    if (expected_selection.value() != selection_revision_)
    {
        return make_error(ErrorCode::kConflict, "Studio selection revision is stale",
                          {{"reason", "stale_selection"},
                           {"expected", std::to_string(expected_selection.value())},
                           {"actual", std::to_string(selection_revision_)}});
    }
    if (asset_id.value() != utf8_from_qstring(presenter_.selected_asset_id_))
    {
        return make_error(ErrorCode::kConflict, "Studio selected asset does not match",
                          {{"reason", "wrong_asset"},
                           {"expected", asset_id.value()},
                           {"actual", utf8_from_qstring(presenter_.selected_asset_id_)}});
    }
    if (presenter_.busy() || presenter_.import_workspace_->importWorkActive() ||
        presenter_.develop()->state().develop_job_in_flight_ ||
        presenter_.develop()->state().pending_save_ ||
        presenter_.develop()->state().pending_preview_ ||
        !presenter_.develop()->state().develop_loaded_)
    {
        return make_error(ErrorCode::kConflict, "Studio Develop state is busy",
                          {{"reason", "busy"}});
    }
    std::string operation_id;
    if (params.value()->contains("operation_id"))
    {
        auto value = string_field(*params.value(), "operation_id", 200);
        if (!value)
            return value.error();
        operation_id = value.value();
    }
    else
    {
        auto reserved = operations_->reserve(descriptor_.session_id);
        if (!reserved)
            return reserved.error();
        operation_id = *reserved.value().find("operation_id")->string_if();
    }
    auto begun = operations_->begin(operation_id, request.params);
    if (!begun)
        return begun.error();
    if (!begun.value())
    {
        auto receipt = operations_->get(operation_id);
        auto object = *snapshot().object_if();
        object.emplace("operation", receipt.value());
        object.emplace("mutation", JsonValue::Object{{"operation_id", operation_id}});
        return JsonValue{std::move(object)};
    }
    auto token = operations_->token(operation_id);
    if (!token)
        return token.error();
    auto applied =
        commands_.applyDevelopPatch(fields_value->second, {}, operation_id, token.value(),
                                    presenter_.observed_catalog_revision_);
    if (!applied)
    {
        operations_->finish(operation_id, false, 0, 0,
                            std::string(error_code_name(applied.error().code)),
                            applied.error().message);
        auto error = applied.error();
        error.context.insert_or_assign("operation_id", operation_id);
        error.context.insert_or_assign("mutation_applied", "false");
        return error;
    }
    if (applied.value() && presenter_.develop()->controlSaveQueued(operation_id))
    {
        single_active_id_ = operation_id;
        presenter_.setBusy(true);
    }
    else
        operations_->finish(operation_id, true, presenter_.observed_catalog_revision_,
                            presenter_.develop()->state().loaded_recipe_history_head_);

    auto state = snapshot();
    auto object = *state.object_if();
    object.emplace("operation", operations_->get(operation_id).value());
    object.emplace("mutation",
                   JsonValue::Object{
                       {"applied", applied.value()},
                       {"fields", fields_value->second},
                       {"operation_id", operation_id},
                       {"recipe_revision", JsonValue::number(std::to_string(recipe_revision_))}});
    return JsonValue{std::move(object)};
}

} // namespace ravo
