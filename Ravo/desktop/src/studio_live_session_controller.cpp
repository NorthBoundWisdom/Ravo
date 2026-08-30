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

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/recipe.h"

#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue &value,
                                                               const std::string_view location)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Studio value must be an object",
                          {{"location", std::string(location)}});
    }
    return object;
}

[[nodiscard]] Result<void> reject_unknown(const JsonValue::Object &object,
                                          const std::initializer_list<std::string_view> allowed,
                                          const std::string_view location)
{
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            return make_error(ErrorCode::kValidation, "Unknown live Studio request field",
                              {{"field", key}, {"location", std::string(location)}});
        }
    }
    return {};
}

[[nodiscard]] Result<std::string> string_field(const JsonValue::Object &object,
                                               const std::string_view key,
                                               const std::size_t maximum = 4096U)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr ||
        found->second.string_if()->empty() || found->second.string_if()->size() > maximum)
    {
        return make_error(ErrorCode::kValidation, "Live Studio string field is invalid",
                          {{"field", std::string(key)}});
    }
    return *found->second.string_if();
}

template <typename Integer>
[[nodiscard]] Result<Integer> integer_field(const JsonValue::Object &object,
                                            const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Studio integer field is missing",
                          {{"field", std::string(key)}});
    }
    const auto &text = found->second.number_if()->text;
    Integer result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Live Studio integer field is invalid",
                          {{"field", std::string(key)}});
    }
    return result;
}

[[nodiscard]] Result<double> number_value(const JsonValue &value, const std::string_view name)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live Develop value must be numeric",
                          {{"name", std::string(name)}});
    }
    char *end = nullptr;
    const double parsed = std::strtod(number->text.c_str(), &end);
    if (end != number->text.c_str() + number->text.size() || !std::isfinite(parsed))
    {
        return make_error(ErrorCode::kValidation, "Live Develop value must be finite",
                          {{"name", std::string(name)}});
    }
    return parsed;
}

[[nodiscard]] Result<std::vector<StudioDevelopField>>
develop_fields_from_json(const JsonValue &value)
{
    const auto *array = value.array_if();
    if (array == nullptr || array->empty() || array->size() > 256U)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Live Develop fields must contain 1 through 256 entries");
    }
    std::vector<StudioDevelopField> result;
    result.reserve(array->size());
    std::set<std::string, std::less<>> names;
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        auto object = require_object((*array)[index], "params.fields[]");
        if (!object)
            return object.error();
        auto known = reject_unknown(*object.value(), {"name", "value"}, "params.fields[]");
        if (!known)
            return known.error();
        auto name = string_field(*object.value(), "name", 256U);
        if (!name)
            return name.error();
        const auto value_found = object.value()->find("value");
        if (value_found == object.value()->end())
        {
            return make_error(ErrorCode::kValidation, "Live Develop value is missing",
                              {{"name", name.value()}});
        }
        auto number = number_value(value_found->second, name.value());
        if (!number)
            return number.error();
        if (!names.insert(name.value()).second)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Live Develop field was specified more than once",
                              {{"name", name.value()}});
        }
        result.push_back({std::move(name).value(), number.value()});
    }
    return result;
}

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
{
}

StudioLiveSessionController::~StudioLiveSessionController() = default;

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
    connect(&presenter_, &StudioPresenter::selectionChanged, this, changed);
    connect(&presenter_, &StudioPresenter::browseModeChanged, this, changed);
    connect(&presenter_, &StudioPresenter::editChanged, this, changed);
    connect(&presenter_, &StudioPresenter::previewChanged, this, changed);
    connect(&commands_, &StudioCommandController::commandsChanged, this, changed);
    refresh();

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
    ++session_revision_;
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
    if (asset && presenter_.develop_loaded_)
    {
        const AssetDescriptor descriptor{asset->id, asset->normalized_uri,
                                         asset->content_fingerprint};
        auto current_recipe = recipe_from_develop(descriptor, presenter_.develop_);
        auto saved_recipe = recipe_from_develop(descriptor, presenter_.saved_develop_);
        auto baseline_recipe = recipe_from_develop(descriptor, presenter_.baseline_develop());
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
    else if (!presenter_.develop_load_error_.isEmpty())
    {
        failure = utf8_from_qstring(presenter_.develop_load_error_);
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

    const std::string preview = std::to_string(presenter_.live_preview_revision_) + "\n" +
                                utf8_from_qstring(presenter_.live_preview_pixel_sha256_) + "\n" +
                                std::to_string(presenter_.preview_loading_ ? 1 : 0);
    if (preview != preview_identity_)
    {
        preview_identity_ = preview;
        ++preview_state_revision_;
    }
}

JsonValue StudioLiveSessionController::snapshot() const
{
    JsonValue::Object catalog{{"open", presenter_.catalogOpen()},
                              {"path", utf8_from_qstring(presenter_.catalog_path_)},
                              {"revision", JsonValue::number(std::to_string(std::max<std::int64_t>(
                                               presenter_.observed_catalog_revision_, 0)))}};

    const auto selected_ids = presenter_.selected_asset_ids();
    JsonValue::Object selection{
        {"asset_ids", string_array(selected_ids)},
        {"primary_asset_id", utf8_from_qstring(presenter_.selected_asset_id_)},
        {"revision", JsonValue::number(std::to_string(selection_revision_))},
    };
    if (const auto asset = presenter_.assets_.assetById(presenter_.selected_asset_id_); asset)
    {
        selection.emplace("display_name", utf8_from_qstring(presenter_.selectedDisplayName()));
        selection.emplace("has_edits", asset->has_edits);
        selection.emplace("media_type", asset->media_type);
        selection.emplace("uri", asset->normalized_uri);
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
    const bool available = selected && presenter_.develop_loaded_ && current.object_if() != nullptr;
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
        {"state", !selected                   ? "none" :
                  !recipe_error_.empty()      ? "error" :
                  !presenter_.develop_loaded_ ? "loading" :
                  pending                     ? "pending" :
                                                "saved"},
    };
    if (!recipe_error_.empty())
        recipe.emplace("error", recipe_error_);

    const bool preview_ready = !presenter_.live_preview_pixel_sha256_.isEmpty() &&
                               presenter_.live_preview_width_ > 0U &&
                               presenter_.live_preview_height_ > 0U;
    JsonValue::Object preview{
        {"color_profile", utf8_from_qstring(presenter_.live_preview_color_profile_id_)},
        {"height", JsonValue::number(std::to_string(presenter_.live_preview_height_))},
        {"matches_current_recipe", available && presenter_.displayed_develop_.has_value() &&
                                       *presenter_.displayed_develop_ == presenter_.develop_ &&
                                       !presenter_.before_after_ && !presenter_.crop_tool_active_ &&
                                       !presenter_.mask_overlay_visible_},
        {"pixel_format", "rgb8"},
        {"pixel_sha256", utf8_from_qstring(presenter_.live_preview_pixel_sha256_)},
        {"resource_id", preview_ready ?
                            descriptor_.session_id +
                                ":preview:" + std::to_string(presenter_.live_preview_revision_) +
                                ":" + utf8_from_qstring(presenter_.live_preview_pixel_sha256_) :
                            std::string{}},
        {"revision", JsonValue::number(std::to_string(preview_state_revision_))},
        {"source_revision", JsonValue::number(std::to_string(presenter_.live_preview_revision_))},
        {"state", presenter_.preview_loading_ ? "loading" :
                  preview_ready               ? "ready" :
                                                "none"},
        {"width", JsonValue::number(std::to_string(presenter_.live_preview_width_))},
    };

    return JsonValue::Object{
        {"browse_mode", utf8_from_qstring(presenter_.browse_mode_)},
        {"busy", presenter_.busy_ || presenter_.import_work_active_ ||
                     presenter_.develop_job_in_flight_ || presenter_.pending_save_.has_value() ||
                     presenter_.pending_preview_.has_value()},
        {"catalog", std::move(catalog)},
        {"command_revision", JsonValue::number(std::to_string(commands_.revision()))},
        {"error", utf8_from_qstring(presenter_.error_text_)},
        {"executable_path", descriptor_.executable_path},
        {"preview", std::move(preview)},
        {"process_id", JsonValue::number(std::to_string(descriptor_.process_id))},
        {"protocol", descriptor_.protocol},
        {"recipe", std::move(recipe)},
        {"revision", JsonValue::number(std::to_string(session_revision_))},
        {"schema_version", JsonValue::number("1")},
        {"selection", std::move(selection)},
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
        if (request.params.object_if() == nullptr || !request.params.object_if()->empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Studio state request does not accept parameters");
        return snapshot();
    }
    if (request.method != "develop")
    {
        return make_error(ErrorCode::kUnsupported, "Studio live-control method is unsupported",
                          {{"method", request.method}});
    }
    auto params = require_object(request.params, "params");
    if (!params)
        return params.error();
    auto known = reject_unknown(
        *params.value(),
        {"asset_id", "expected_selection_revision", "expected_session_revision", "fields"},
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
    auto fields = develop_fields_from_json(fields_value->second);
    if (!fields)
        return fields.error();

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
    if (presenter_.busy_ || presenter_.import_work_active_ || presenter_.develop_job_in_flight_ ||
        presenter_.pending_save_ || presenter_.pending_preview_ || !presenter_.develop_loaded_)
    {
        return make_error(ErrorCode::kConflict, "Studio Develop state is busy",
                          {{"reason", "busy"}});
    }
    auto applied = commands_.applyDevelopFields(fields.value());
    if (!applied)
        return applied.error();

    auto state = snapshot();
    auto object = *state.object_if();
    JsonValue::Array applied_fields;
    for (const auto &field : fields.value())
    {
        applied_fields.push_back(JsonValue::Object{
            {"name", field.name}, {"value", JsonValue::number(std::to_string(field.value))}});
    }
    object.emplace("mutation",
                   JsonValue::Object{
                       {"applied", applied.value()},
                       {"fields", std::move(applied_fields)},
                       {"recipe_revision", JsonValue::number(std::to_string(recipe_revision_))}});
    return JsonValue{std::move(object)};
}

} // namespace ravo
