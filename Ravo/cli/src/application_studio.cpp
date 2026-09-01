#include "ravo/cli/application.h"
#include "application_internal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>

#ifdef emit
#undef emit
#endif

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/camera_noise_profile.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/control/live_control.h"
#include "ravo/foundation/json.h"
#include "ravo/engine/noise_calibration.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/style.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/artifact_publication.h"

namespace ravo::cli_internal
{
struct StudioCliArguments
{
    std::string_view session_id;
    std::string_view workspace_root;
    std::string_view asset_id;
    std::optional<std::uint64_t> expected_session_revision;
    std::optional<std::uint64_t> expected_selection_revision;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::string_view output;
    std::optional<std::uint32_t> max_edge;
    int timeout_ms = 120000;
};
[[nodiscard]] Result<StudioCliArguments>
parse_studio_flags(const std::span<const std::string_view> positional)
{
    StudioCliArguments result;
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Studio option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--session-id")
        {
            if (!result.session_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio session ID was specified twice");
            result.session_id = value;
        }
        else if (option == "--workspace-root")
        {
            if (!result.workspace_root.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio workspace root was specified twice");
            result.workspace_root = value;
        }
        else if (option == "--asset-id")
        {
            if (!result.asset_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio asset ID was specified twice");
            result.asset_id = value;
        }
        else if (option == "--expect-session-revision")
        {
            if (result.expected_session_revision)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Expected session revision was specified twice");
            auto parsed = parse_uint64_flag(value, option);
            if (!parsed)
                return parsed.error();
            result.expected_session_revision = parsed.value();
        }
        else if (option == "--expect-selection-revision")
        {
            if (result.expected_selection_revision)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Expected selection revision was specified twice");
            auto parsed = parse_uint64_flag(value, option);
            if (!parsed)
                return parsed.error();
            result.expected_selection_revision = parsed.value();
        }
        else if (option == "--set")
        {
            const auto owned = std::string(value);
            const auto split = owned.find('=');
            if (split == std::string::npos || split == 0 || split + 1 == owned.size())
                return make_error(ErrorCode::kInvalidArgument, "--set requires name=value",
                                  {{"value", owned}});
            auto parsed = parse_double_flag(owned.substr(split + 1), option);
            if (!parsed)
                return parsed.error();
            if (std::find_if(
                    result.develop_sets.begin(), result.develop_sets.end(), [&](const auto &item)
                    { return item.first == owned.substr(0, split); }) != result.develop_sets.end())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio Develop field was specified twice",
                                  {{"name", owned.substr(0, split)}});
            result.develop_sets.emplace_back(owned.substr(0, split), parsed.value());
        }
        else if (option == "--output")
        {
            if (!result.output.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio preview output was specified twice");
            result.output = value;
        }
        else if (option == "--max-edge")
        {
            if (result.max_edge)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio preview size was specified twice");
            auto parsed = parse_dimension(value, option);
            if (!parsed)
                return parsed.error();
            result.max_edge = parsed.value();
        }
        else if (option == "--timeout-ms")
        {
            auto parsed = parse_int_flag(value, option);
            if (!parsed)
                return parsed.error();
            if (parsed.value() < 100 || parsed.value() > 120000)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio timeout must be between 100 and 120000 milliseconds",
                                  {{"value", std::string(value)}});
            result.timeout_ms = parsed.value();
        }
        else
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown Studio option",
                              {{"option", std::string(option)}});
        }
    }
    return result;
}

[[nodiscard]] bool ends_with_png(const std::string_view path) noexcept
{
    if (path.size() < 4U)
    {
        return false;
    }
    const auto suffix = path.substr(path.size() - 4U);
    return (suffix[0] == '.' && (suffix[1] == 'p' || suffix[1] == 'P') &&
            (suffix[2] == 'n' || suffix[2] == 'N') && (suffix[3] == 'g' || suffix[3] == 'G'));
}

[[nodiscard]] Result<const JsonValue::Object *> studio_object(const JsonValue &value,
                                                              const std::string_view location)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response value must be an object",
                          {{"location", std::string(location)}});
    }
    return object;
}

[[nodiscard]] Result<std::string> studio_string(const JsonValue::Object &object,
                                                const std::string_view key,
                                                const std::string_view location)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response string is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return *found->second.string_if();
}

[[nodiscard]] Result<std::uint64_t> studio_uint64(const JsonValue::Object &object,
                                                  const std::string_view key,
                                                  const std::string_view location)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response revision is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    const auto &text = found->second.number_if()->text;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Studio response revision is invalid",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return value;
}

struct ObservedStudioState
{
    JsonValue json;
    std::string session_id;
    std::uint64_t session_revision = 0;
    std::string asset_id;
    std::uint64_t selection_revision = 0;
    std::uint64_t recipe_revision = 0;
    std::string recipe_state;
    std::string current_recipe_json;
    std::string preview_state;
    bool preview_matches_recipe = false;
    bool busy = false;
    std::string catalog_path;
    std::string error_text;
};

[[nodiscard]] Result<ObservedStudioState> observed_studio_state(JsonValue value)
{
    auto root = studio_object(value, "session");
    if (!root)
        return root.error();
    auto session_id = studio_string(*root.value(), "session_id", "session");
    auto session_revision = studio_uint64(*root.value(), "revision", "session");
    auto error_text = studio_string(*root.value(), "error", "session");
    if (!session_id)
        return session_id.error();
    if (!session_revision)
        return session_revision.error();
    if (!error_text)
        return error_text.error();
    const auto busy = root.value()->find("busy");
    if (busy == root.value()->end() || busy->second.boolean_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Studio busy state is missing");

    const auto selection_value = root.value()->find("selection");
    const auto recipe_value = root.value()->find("recipe");
    const auto preview_value = root.value()->find("preview");
    const auto catalog_value = root.value()->find("catalog");
    if (selection_value == root.value()->end() || recipe_value == root.value()->end() ||
        preview_value == root.value()->end() || catalog_value == root.value()->end())
        return make_error(ErrorCode::kValidation, "Studio state sections are incomplete");
    auto selection = studio_object(selection_value->second, "selection");
    auto recipe = studio_object(recipe_value->second, "recipe");
    auto preview = studio_object(preview_value->second, "preview");
    auto catalog = studio_object(catalog_value->second, "catalog");
    if (!selection)
        return selection.error();
    if (!recipe)
        return recipe.error();
    if (!preview)
        return preview.error();
    if (!catalog)
        return catalog.error();
    auto asset_id = studio_string(*selection.value(), "primary_asset_id", "selection");
    auto selection_revision = studio_uint64(*selection.value(), "revision", "selection");
    auto recipe_revision = studio_uint64(*recipe.value(), "revision", "recipe");
    auto recipe_state = studio_string(*recipe.value(), "state", "recipe");
    auto preview_state = studio_string(*preview.value(), "state", "preview");
    auto catalog_path = studio_string(*catalog.value(), "path", "catalog");
    if (!asset_id)
        return asset_id.error();
    if (!selection_revision)
        return selection_revision.error();
    if (!recipe_revision)
        return recipe_revision.error();
    if (!recipe_state)
        return recipe_state.error();
    if (!preview_state)
        return preview_state.error();
    if (!catalog_path)
        return catalog_path.error();
    const auto current = recipe.value()->find("current");
    if (current == recipe.value()->end())
        return make_error(ErrorCode::kValidation, "Studio current recipe is missing");
    const auto matches = preview.value()->find("matches_current_recipe");
    if (matches == preview.value()->end() || matches->second.boolean_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Studio preview match state is missing");
    ObservedStudioState result;
    result.json = std::move(value);
    result.session_id = std::move(session_id).value();
    result.session_revision = session_revision.value();
    result.asset_id = std::move(asset_id).value();
    result.selection_revision = selection_revision.value();
    result.recipe_revision = recipe_revision.value();
    result.recipe_state = std::move(recipe_state).value();
    result.current_recipe_json =
        current->second.is_null() ? std::string{} : serialize_json(current->second);
    result.preview_state = std::move(preview_state).value();
    result.preview_matches_recipe = *matches->second.boolean_if();
    result.busy = *busy->second.boolean_if();
    result.catalog_path = std::move(catalog_path).value();
    result.error_text = std::move(error_text).value();
    return result;
}

[[nodiscard]] Result<std::filesystem::path> canonical_workspace(const std::string_view path)
{
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(filesystem_path_from_utf8(path), error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot resolve the Studio workspace root",
                          {{"path", std::string(path)}, {"reason", error.message()}});
    return canonical;
}

[[nodiscard]] Result<bool> studio_output_exists(const std::string_view path)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(filesystem_path_from_utf8(path), error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot inspect the Studio output path",
                          {{"path", std::string(path)}, {"reason", error.message()}});
    return exists;
}

[[nodiscard]] Result<std::optional<std::filesystem::path>> default_cli_workspace()
{
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot read the current working directory",
                          {{"reason", error.message()}});
    return find_ravo_workspace_root(current);
}

[[nodiscard]] Result<LiveSessionDescriptor> resolve_live_session(const StudioCliArguments &flags)
{
    if (!flags.session_id.empty())
    {
        return LocalControlClient::find_descriptor(flags.session_id);
    }

    auto discovered = LocalControlClient::discover();
    if (!discovered)
        return discovered.error();

    std::optional<std::filesystem::path> workspace;
    if (!flags.workspace_root.empty())
    {
        auto canonical = canonical_workspace(flags.workspace_root);
        if (!canonical)
            return canonical.error();
        workspace = std::move(canonical).value();
    }
    else
    {
        auto detected = default_cli_workspace();
        if (!detected)
            return detected.error();
        workspace = std::move(detected).value();
    }

    std::vector<LiveSessionDescriptor> candidates;
    for (const auto &session : discovered.value())
    {
        if (!workspace)
        {
            candidates.push_back(session);
            continue;
        }
        if (session.workspace_root.empty())
            continue;
        auto session_workspace = canonical_workspace(session.workspace_root);
        if (session_workspace && session_workspace.value() == *workspace)
            candidates.push_back(session);
    }
    if (candidates.empty())
    {
        std::map<std::string, std::string, std::less<>> context;
        if (workspace)
            context.emplace("workspace_root", filesystem_path_to_utf8(*workspace));
        return make_error(ErrorCode::kNotFound, "No matching Studio live session is running",
                          std::move(context));
    }
    if (candidates.size() != 1U)
    {
        std::string ids;
        for (const auto &candidate : candidates)
        {
            if (!ids.empty())
                ids.push_back(',');
            ids.append(candidate.session_id);
        }
        return make_error(ErrorCode::kConflict, "More than one matching Studio session is running",
                          {{"count", std::to_string(candidates.size())}, {"session_ids", ids}});
    }
    return candidates.front();
}

[[nodiscard]] Result<ObservedStudioState> read_studio_state(const LiveSessionDescriptor &session,
                                                            const int timeout_ms)
{
    auto response = LocalControlClient::request(session, "state", JsonValue::Object{},
                                                std::min(timeout_ms, 5000));
    if (!response)
        return response.error();
    return observed_studio_state(std::move(response).value());
}

[[nodiscard]] Result<ObservedStudioState>
wait_for_studio_state(const LiveSessionDescriptor &session, const std::string_view asset_id,
                      const std::optional<std::string_view> expected_recipe,
                      const bool require_saved, const bool require_matching_preview,
                      const int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        auto state = read_studio_state(session, timeout_ms);
        if (!state)
            return state.error();
        if (state.value().asset_id != asset_id)
            return make_error(ErrorCode::kConflict, "Studio selection changed while waiting",
                              {{"reason", "selection_changed"},
                               {"expected", std::string(asset_id)},
                               {"actual", state.value().asset_id}});
        if (expected_recipe && state.value().current_recipe_json != *expected_recipe)
            return state.value().error_text.empty() ?
                       make_error(
                           ErrorCode::kConflict, "Studio recipe changed while waiting",
                           {{"reason", "recipe_changed"}, {"asset_id", state.value().asset_id}}) :
                       make_error(ErrorCode::kIo, "Studio Develop mutation failed",
                                  {{"reason", "develop_failed"},
                                   {"asset_id", state.value().asset_id},
                                   {"detail", state.value().error_text}});
        if (state.value().recipe_state == "error")
            return make_error(ErrorCode::kValidation, "Studio recipe state is invalid",
                              {{"reason", "recipe_error"},
                               {"asset_id", state.value().asset_id},
                               {"detail", state.value().error_text}});
        const bool recipe_ready = !state.value().current_recipe_json.empty() &&
                                  state.value().recipe_state != "loading" &&
                                  state.value().recipe_state != "error";
        const bool saved = !require_saved || state.value().recipe_state == "saved";
        const bool preview = !require_matching_preview || (state.value().preview_state == "ready" &&
                                                           state.value().preview_matches_recipe);
        if (!state.value().busy && recipe_ready && saved && preview)
            return state;
        if (std::chrono::steady_clock::now() >= deadline)
            return make_error(
                ErrorCode::kCancelled, "Timed out waiting for Studio to settle",
                {{"asset_id", std::string(asset_id)}, {"timeout_ms", std::to_string(timeout_ms)}});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

[[nodiscard]] Result<JsonValue>
render_studio_artifact(const EngineFacade &engine, const LiveSessionDescriptor &session,
                       const ObservedStudioState &observed, const std::string_view output,
                       const std::uint32_t max_edge, const int timeout_ms)
{
    if (observed.asset_id.empty() || observed.catalog_path.empty() ||
        observed.current_recipe_json.empty())
        return make_error(ErrorCode::kConflict, "Studio has no selected editable photo",
                          {{"reason", "no_selection"}});
    if (!ends_with_png(output))
        return make_error(ErrorCode::kInvalidArgument,
                          "Studio preview --output must be a .png path",
                          {{"path", std::string(output)}});
    auto output_exists = studio_output_exists(output);
    if (!output_exists)
        return output_exists.error();
    if (output_exists.value())
        return make_error(ErrorCode::kConflict, "Output path already exists",
                          {{"path", std::string(output)}});

    auto recipe = parse_recipe_json(observed.current_recipe_json);
    if (!recipe)
        return recipe.error();
    if (recipe.value().asset.id != observed.asset_id)
        return make_error(ErrorCode::kConflict, "Studio recipe asset does not match selection",
                          {{"reason", "wrong_asset"}});
    auto params = develop_from_recipe(recipe.value());
    if (!params)
        return params.error();
    auto service = open_catalog_session(engine, observed.catalog_path, false);
    if (!service)
        return service.error();
    PreviewRequest request;
    request.asset_id = observed.asset_id;
    request.max_edge = max_edge;
    request.prefer_embedded_preview = false;
    request.persist_preview_record = false;
    request.request_revision = observed.recipe_revision;
    request.correlation_id = "cli-live-studio-preview";
    auto cancellation = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                          std::chrono::milliseconds(timeout_ms));
    request.cancellation = cancellation.token();
    auto preview = service.value()->request_preview(request, params.value());
    if (!preview)
        return preview.error();
    if (!preview.value().cache_path.empty() || preview.value().rgb.empty())
        return make_error(ErrorCode::kIo,
                          "Live Studio preview did not return immutable memory pixels");

    auto current = read_studio_state(session, timeout_ms);
    if (!current)
        return current.error();
    if (current.value().asset_id != observed.asset_id ||
        current.value().selection_revision != observed.selection_revision ||
        current.value().recipe_revision != observed.recipe_revision ||
        current.value().current_recipe_json != observed.current_recipe_json)
        return make_error(ErrorCode::kConflict,
                          "Studio selection or recipe changed before preview publication",
                          {{"reason", "stale_preview"}, {"asset_id", observed.asset_id}});

    auto statistics = probe_statistics_json(preview.value());
    if (!statistics)
        return statistics.error();
    RenderedImage image;
    image.width = preview.value().width;
    image.height = preview.value().height;
    image.rgb = preview.value().rgb;
    image.color_profile = preview.value().color_profile;
    auto encoded = engine.encode_png(image);
    if (!encoded)
        return encoded.error();
    auto written = write_file_bytes_atomically(output, encoded.value());
    if (!written)
        return written.error();
    const QByteArrayView bytes(reinterpret_cast<const char *>(encoded.value().data()),
                               static_cast<qsizetype>(encoded.value().size()));
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    return JsonValue{JsonValue::Object{
        {"artifact",
         JsonValue::Object{
             {"bytes", JsonValue::number(std::to_string(encoded.value().size()))},
             {"color_profile", preview.value().color_profile.identifier},
             {"content_hash", digest.toStdString()},
             {"hash_algorithm", "sha256"},
             {"height", JsonValue::number(std::to_string(preview.value().height))},
             {"lifecycle", "caller_owned"},
             {"mime_type", "image/png"},
             {"path", std::string(output)},
             {"width", JsonValue::number(std::to_string(preview.value().width))},
         }},
        {"asset_id", observed.asset_id},
        {"recipe_revision", JsonValue::number(std::to_string(observed.recipe_revision))},
        {"selection_revision", JsonValue::number(std::to_string(observed.selection_revision))},
        {"session_id", observed.session_id},
        {"statistics", std::move(statistics).value()},
    }};
}

[[nodiscard]] Result<JsonValue>
run_studio_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
        return make_error(ErrorCode::kInvalidArgument,
                          "Usage: ravo studio <sessions|state|develop|preview> [options]");
    const auto subcommand = positional[1];
    auto flags = parse_studio_flags(positional);
    if (!flags)
        return flags.error();

    if (subcommand == "sessions")
    {
        if (!flags.value().session_id.empty() || !flags.value().asset_id.empty() ||
            flags.value().expected_session_revision || flags.value().expected_selection_revision ||
            !flags.value().develop_sets.empty() || !flags.value().output.empty() ||
            flags.value().max_edge)
            return make_error(ErrorCode::kInvalidArgument,
                              "studio sessions accepts only --workspace-root and --timeout-ms");
        auto sessions = LocalControlClient::discover(std::min(flags.value().timeout_ms, 5000));
        if (!sessions)
            return sessions.error();
        std::optional<std::filesystem::path> workspace;
        if (!flags.value().workspace_root.empty())
        {
            auto canonical = canonical_workspace(flags.value().workspace_root);
            if (!canonical)
                return canonical.error();
            workspace = std::move(canonical).value();
        }
        else
        {
            auto detected = default_cli_workspace();
            if (!detected)
                return detected.error();
            workspace = std::move(detected).value();
        }
        JsonValue::Array values;
        for (const auto &session : sessions.value())
        {
            auto json = live_session_descriptor_to_json(session);
            if (!json)
                return json.error();
            auto object = *json.value().object_if();
            bool matches = !workspace;
            if (workspace && !session.workspace_root.empty())
            {
                auto session_workspace = canonical_workspace(session.workspace_root);
                matches = session_workspace && session_workspace.value() == *workspace;
            }
            object.emplace("matches_workspace", matches);
            values.emplace_back(std::move(object));
        }
        return JsonValue{JsonValue::Object{
            {"sessions", std::move(values)},
            {"workspace_root", workspace ? filesystem_path_to_utf8(*workspace) : std::string{}},
        }};
    }

    auto session = resolve_live_session(flags.value());
    if (!session)
        return session.error();
    if (subcommand == "state")
    {
        if (!flags.value().asset_id.empty() || flags.value().expected_session_revision ||
            flags.value().expected_selection_revision || !flags.value().develop_sets.empty() ||
            !flags.value().output.empty() || flags.value().max_edge)
            return make_error(ErrorCode::kInvalidArgument,
                              "studio state accepts only session/workspace and timeout options");
        auto state = read_studio_state(session.value(), flags.value().timeout_ms);
        return state ? Result<JsonValue>{std::move(state).value().json} :
                       Result<JsonValue>{state.error()};
    }
    if (subcommand == "develop")
    {
        if (flags.value().develop_sets.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio develop requires at least one --set name=value");
        if (!flags.value().output.empty() && !ends_with_png(flags.value().output))
            return make_error(ErrorCode::kInvalidArgument,
                              "studio develop --output must be a .png path",
                              {{"path", std::string(flags.value().output)}});
        if (!flags.value().output.empty())
        {
            auto output_exists = studio_output_exists(flags.value().output);
            if (!output_exists)
                return output_exists.error();
            if (output_exists.value())
                return make_error(ErrorCode::kConflict, "Output path already exists",
                                  {{"path", std::string(flags.value().output)}});
        }
        auto initial = read_studio_state(session.value(), flags.value().timeout_ms);
        if (!initial)
            return initial.error();
        const std::string asset_id = flags.value().asset_id.empty() ?
                                         initial.value().asset_id :
                                         std::string(flags.value().asset_id);
        if (asset_id.empty())
            return make_error(ErrorCode::kConflict, "Studio has no selected photo",
                              {{"reason", "no_selection"}});
        if (initial.value().busy || initial.value().current_recipe_json.empty())
        {
            auto idle = wait_for_studio_state(session.value(), asset_id, std::nullopt, false, false,
                                              flags.value().timeout_ms);
            if (!idle)
                return idle.error();
            initial = std::move(idle);
        }
        const auto observed_selection_revision = initial.value().selection_revision;
        const std::string observed_recipe = initial.value().current_recipe_json;
        const bool strict_expectation = flags.value().expected_session_revision.has_value() ||
                                        flags.value().expected_selection_revision.has_value();
        std::optional<ObservedStudioState> mutated;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            JsonValue::Array fields;
            for (const auto &[name, value] : flags.value().develop_sets)
                fields.push_back(JsonValue::Object{
                    {"name", name}, {"value", JsonValue::number(std::to_string(value))}});
            const auto expected_session =
                flags.value().expected_session_revision.value_or(initial.value().session_revision);
            const auto expected_selection = flags.value().expected_selection_revision.value_or(
                initial.value().selection_revision);
            auto response = LocalControlClient::request(
                session.value(), "develop",
                JsonValue::Object{
                    {"asset_id", asset_id},
                    {"expected_selection_revision",
                     JsonValue::number(std::to_string(expected_selection))},
                    {"expected_session_revision",
                     JsonValue::number(std::to_string(expected_session))},
                    {"fields", std::move(fields)},
                },
                std::min(flags.value().timeout_ms, 5000));
            if (response)
            {
                auto parsed = observed_studio_state(std::move(response).value());
                if (!parsed)
                    return parsed.error();
                mutated = std::move(parsed).value();
                break;
            }
            const auto reason = response.error().context.find("reason");
            const bool stale_session =
                reason != response.error().context.end() && reason->second == "stale_session";
            if (strict_expectation || !stale_session || attempt == 3)
                return response.error();
            auto refreshed = read_studio_state(session.value(), flags.value().timeout_ms);
            if (!refreshed)
                return refreshed.error();
            if (refreshed.value().asset_id != asset_id ||
                refreshed.value().selection_revision != observed_selection_revision ||
                refreshed.value().current_recipe_json != observed_recipe)
                return response.error();
            if (refreshed.value().busy)
            {
                auto idle = wait_for_studio_state(session.value(), asset_id, observed_recipe, false,
                                                  false, flags.value().timeout_ms);
                if (!idle)
                    return idle.error();
                refreshed = std::move(idle);
            }
            initial = std::move(refreshed);
        }
        if (!mutated)
            return make_error(ErrorCode::kInternal, "Studio Develop retry ended without a result");
        const std::string target_recipe = mutated->current_recipe_json;
        auto settled = wait_for_studio_state(session.value(), asset_id, target_recipe, true, true,
                                             flags.value().timeout_ms);
        if (!settled)
            return settled.error();
        if (!flags.value().output.empty())
        {
            auto artifact = render_studio_artifact(
                engine, session.value(), settled.value(), flags.value().output,
                flags.value().max_edge.value_or(kDefaultPreviewMaxEdge), flags.value().timeout_ms);
            if (!artifact)
            {
                auto error = artifact.error();
                error.context.insert_or_assign("mutation_applied", "true");
                error.context.insert_or_assign("asset_id", settled.value().asset_id);
                error.context.insert_or_assign("recipe_revision",
                                               std::to_string(settled.value().recipe_revision));
                return error;
            }
            return artifact;
        }
        return settled.value().json;
    }
    if (subcommand == "preview")
    {
        if (!flags.value().develop_sets.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio preview does not accept --set; use studio develop");
        if (flags.value().output.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio preview requires --output <file.png>");
        auto initial = read_studio_state(session.value(), flags.value().timeout_ms);
        if (!initial)
            return initial.error();
        const std::string asset_id = flags.value().asset_id.empty() ?
                                         initial.value().asset_id :
                                         std::string(flags.value().asset_id);
        if (asset_id.empty() || initial.value().asset_id != asset_id)
            return make_error(ErrorCode::kConflict, "Studio selected asset does not match",
                              {{"reason", "wrong_asset"},
                               {"expected", asset_id},
                               {"actual", initial.value().asset_id}});
        if (flags.value().expected_session_revision &&
            *flags.value().expected_session_revision != initial.value().session_revision)
            return make_error(ErrorCode::kConflict, "Studio session revision is stale",
                              {{"reason", "stale_session"}});
        if (flags.value().expected_selection_revision &&
            *flags.value().expected_selection_revision != initial.value().selection_revision)
            return make_error(ErrorCode::kConflict, "Studio selection revision is stale",
                              {{"reason", "stale_selection"}});
        auto ready = wait_for_studio_state(session.value(), asset_id, std::nullopt, false, false,
                                           flags.value().timeout_ms);
        if (!ready)
            return ready.error();
        return render_studio_artifact(engine, session.value(), ready.value(), flags.value().output,
                                      flags.value().max_edge.value_or(kDefaultPreviewMaxEdge),
                                      flags.value().timeout_ms);
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown Studio subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
