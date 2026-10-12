#include "studio_client_internal.h"
#include "application_internal.h"
#include "mcp_internal.h"

#include <chrono>
#include <thread>
#include <set>
#include "ravo/adapters/text_file.h"
#include "ravo/recipe/develop_patch.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/services/asset_annotation.h"

namespace ravo::cli_internal
{
namespace
{
Result<JsonValue> read_request(const std::string_view path)
{
    if (path.empty())
        return make_error(ErrorCode::kInvalidArgument, "--request <JSON file> is required");
    auto text = read_utf8_text_file(path, kLiveControlMaxMessageBytes);
    if (!text)
        return text.error();
    auto json = parse_json(text.value());
    if (!json)
        return json.error();
    if (!json.value().object_if())
        return make_error(ErrorCode::kInvalidArgument, "Request must be a JSON object");
    return json;
}
Result<std::uint64_t> revision_field(const JsonValue &value, std::string_view field)
{
    const auto *item = value.find(field);
    if (!item || !item->number_if())
        return make_error(ErrorCode::kInvalidArgument, "Expected revision is missing",
                          {{"field", std::string(field)}});
    return parse_uint64_flag(item->number_if()->text, field);
}
Result<void> check_binding(const JsonValue &request, const ObservedStudioState &state)
{
    const auto *asset = request.find("asset_id");
    if (!asset || !asset->string_if())
        return make_error(ErrorCode::kInvalidArgument, "Request asset_id is missing");
    auto session = revision_field(request, "expected_session_revision");
    auto selection = revision_field(request, "expected_selection_revision");
    auto recipe = revision_field(request, "expected_recipe_revision");
    auto catalog = revision_field(request, "expected_catalog_revision");
    if (!session)
        return session.error();
    if (!selection)
        return selection.error();
    if (!recipe)
        return recipe.error();
    if (!catalog)
        return catalog.error();
    const auto *session_id = request.find("session_id");
    if (!session_id || !session_id->string_if() || *session_id->string_if() != state.session_id ||
        *asset->string_if() != state.asset_id || session.value() != state.session_revision ||
        selection.value() != state.selection_revision || recipe.value() != state.recipe_revision ||
        catalog.value() != static_cast<std::uint64_t>(state.catalog_revision))
        return make_error(ErrorCode::kConflict, "Evaluation binding is stale",
                          {{"reason", "stale_binding"}});
    return {};
}
JsonValue::Object binding(const ObservedStudioState &state)
{
    return {
        {"session_id", state.session_id},
        {"asset_id", state.asset_id},
        {"expected_session_revision", JsonValue::number(std::to_string(state.session_revision))},
        {"expected_selection_revision",
         JsonValue::number(std::to_string(state.selection_revision))},
        {"expected_recipe_revision", JsonValue::number(std::to_string(state.recipe_revision))},
        {"expected_catalog_revision", JsonValue::number(std::to_string(state.catalog_revision))}};
}
Result<JsonValue> wait_operation(const LiveSessionDescriptor &session, std::string_view id,
                                 JsonValue current,
                                 const std::chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        const auto *phase = current.find("phase");
        if (!phase || !phase->string_if())
            return make_error(ErrorCode::kValidation, "Operation receipt is invalid");
        if (*phase->string_if() != "pending" && *phase->string_if() != "reserved")
            return current;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0)
            return make_error(ErrorCode::kCancelled, "Timed out waiting for operation receipt",
                              {{"operation_id", std::string(id)}, {"mutation_applied", "unknown"}});
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min<std::int64_t>(remaining, 25)));
        remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
        if (remaining <= 0)
            continue;
        auto response = LocalControlClient::request(
            session, "operation",
            JsonValue::Object{{"action", "get"}, {"operation_id", std::string(id)}},
            static_cast<int>(std::min<std::int64_t>(remaining, 5000)));
        if (!response)
        {
            auto error = response.error();
            error.context.insert_or_assign("operation_id", std::string(id));
            error.context.insert_or_assign("mutation_applied", "unknown");
            return error;
        }
        current = std::move(response).value();
    }
}
} // namespace

JsonValue cli_annotation_capabilities()
{
    return asset_annotation_capabilities_json();
}

Result<JsonValue> run_studio_automation(const EngineFacade &engine, const std::string_view command,
                                        const StudioCliArguments &flags)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(flags.timeout_ms);
    const auto remaining = [&]() -> int
    {
        return static_cast<int>(
            std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                                          deadline - std::chrono::steady_clock::now())
                                          .count()));
    };
    if (command == "capabilities")
    {
        auto fields = develop_fields_json();
        auto object = *fields.object_if();
        object.erase("watermark_text"); // catalog-only command shorthand
        object.emplace("type", "ravo.studio.capabilities");
        object.emplace("version", JsonValue::number("1"));
        object.emplace("patch_values", JsonValue::Array{"number", "text"});
        object.emplace("scopes", JsonValue::Array{"global", "local_id"});
        object.emplace("availability", "validated_on_evaluate");
        object.emplace("units", "strict_field_native_units");
        object.emplace("max_patch_fields", JsonValue::number("256"));
        object.emplace("max_batch_assets", JsonValue::number("256"));
        object.emplace("max_selection_page", JsonValue::number("512"));
        object.emplace("annotations", cli_annotation_capabilities());
        object.emplace("operation_lifecycle", "studio_session_bounded");
        JsonValue::Array kinds;
        for (std::int64_t index = 1; index <= 8; ++index)
        {
            auto kind = develop_mask_kind_from_index(index);
            kinds.emplace_back(JsonValue::Object{
                {"index", JsonValue::number(std::to_string(index))},
                {"name", std::string(mask_kind_name(*kind))},
                {"drawn", local_mask_drawable(*kind)},
                {"create_action", *kind == MaskKind::kParametric || local_mask_drawable(*kind)}});
        }
        object.emplace("mask_kinds", std::move(kinds));
        object.emplace("mask_coordinates", "normalized_developed_image");
        object.emplace("mask_coordinate_bounds",
                       JsonValue::Array{JsonValue::number("0"), JsonValue::number("1")});
        object.emplace("mask_gesture_actions", JsonValue::Array{"gesture_begin", "gesture_update",
                                                                "gesture_end", "gesture_cancel"});
        return JsonValue{std::move(object)};
    }
    auto session = resolve_live_session(flags);
    if (!session)
        return session.error();
    if (command == "operation")
    {
        if (flags.mask_action.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "operation requires --action reserve|get|cancel");
        JsonValue::Object params{{"action", std::string(flags.mask_action)}};
        if (!flags.operation_id.empty())
            params.emplace("operation_id", std::string(flags.operation_id));
        auto receipt =
            LocalControlClient::request(session.value(), "operation", JsonValue{std::move(params)},
                                        std::min(remaining(), 5000));
        if (!receipt)
            return receipt.error();
        if (!flags.wait)
            return receipt;
        const auto *id = receipt.value().find("operation_id");
        if (!id || !id->string_if())
            return make_error(ErrorCode::kValidation, "Operation ID is missing");
        const auto owned_id = *id->string_if();
        return wait_operation(session.value(), owned_id, std::move(receipt).value(), deadline);
    }
    if (command == "commit")
    {
        if (flags.operation_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "commit requires a reserved --operation-id");
        auto request = read_request(flags.request_file);
        if (!request)
            return request.error();
        if (const auto *type = request.value().find("type"))
        {
            for (const auto &[key, value] : *request.value().object_if())
            {
                static_cast<void>(value);
                if (key != "type" && key != "version" && key != "session_id" && key != "asset_id" &&
                    key != "commit_request")
                    return make_error(ErrorCode::kInvalidArgument, "Unknown candidate field",
                                      {{"field", key}});
            }
            const auto *version = request.value().find("version");
            const auto *target = request.value().find("session_id");
            const auto *body = request.value().find("commit_request");
            if (!type->string_if() || *type->string_if() != "ravo.develop.candidate" || !version ||
                !version->number_if() || version->number_if()->text != "1" || !target ||
                !target->string_if() || *target->string_if() != session.value().session_id ||
                !body || !body->object_if())
                return make_error(ErrorCode::kInvalidArgument, "Candidate identity is invalid");
            const auto *kind = body->find("kind"), *asset = request.value().find("asset_id"),
                       *body_asset = body->find("asset_id");
            if (!kind || !kind->string_if() || *kind->string_if() != "patch" || !asset ||
                !asset->string_if() || !body_asset || !body_asset->string_if() ||
                *body_asset->string_if() != *asset->string_if())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Candidate command identity is invalid");
            request = *body;
        }
        auto object = *request.value().object_if();
        if (object.contains("operation_id"))
            return make_error(ErrorCode::kInvalidArgument,
                              "Request file must not override the reserved operation ID");
        object.emplace("operation_id", std::string(flags.operation_id));
        auto receipt = LocalControlClient::request(
            session.value(), "commit", JsonValue{std::move(object)}, std::min(remaining(), 5000));
        if (!receipt)
        {
            auto error = receipt.error();
            error.context.insert_or_assign("operation_id", std::string(flags.operation_id));
            error.context.try_emplace("mutation_applied", "unknown");
            return error;
        }
        if (!flags.wait)
            return receipt;
        return wait_operation(session.value(), flags.operation_id, std::move(receipt).value(),
                              deadline);
    }
    if (command == "history")
    {
        if (flags.asset_id.empty() || !flags.expected_selection_revision)
            return make_error(ErrorCode::kInvalidArgument,
                              "history requires explicit asset and selection revision");
        JsonValue::Object params{
            {"asset_id", std::string(flags.asset_id)},
            {"expected_selection_revision",
             JsonValue::number(std::to_string(*flags.expected_selection_revision))},
            {"offset", JsonValue::number(std::to_string(flags.offset.value_or(0)))},
            {"limit", JsonValue::number(std::to_string(flags.limit.value_or(200)))}};
        if (flags.expected_session_revision)
            params.emplace("expected_session_revision",
                           JsonValue::number(std::to_string(*flags.expected_session_revision)));
        if (flags.expected_recipe_revision)
            params.emplace("expected_recipe_revision",
                           JsonValue::number(std::to_string(*flags.expected_recipe_revision)));
        if (flags.expected_catalog_revision)
            params.emplace("expected_catalog_revision",
                           JsonValue::number(std::to_string(*flags.expected_catalog_revision)));
        return LocalControlClient::request(session.value(), "history", JsonValue{std::move(params)},
                                           std::min(remaining(), 5000));
    }

    if (flags.output.empty() || flags.max_edge.value_or(960) > 1600)
        return make_error(ErrorCode::kInvalidArgument,
                          "evaluate requires --output PNG and max-edge at most 1600");
    auto initial = read_studio_state(session.value(), std::min(remaining(), 5000));
    if (!initial)
        return initial.error();
    if (initial.value().current_recipe_json.empty())
        return make_error(ErrorCode::kConflict, "Recipe is not ready", {{"reason", "busy"}});
    JsonValue request;
    if (!flags.request_file.empty())
    {
        if (!flags.fields.empty() || !flags.asset_id.empty() || !flags.local_id.empty() ||
            flags.expected_session_revision || flags.expected_selection_revision ||
            flags.expected_recipe_revision || flags.expected_catalog_revision)
            return make_error(ErrorCode::kInvalidArgument,
                              "Request file and inline evaluation binding cannot be mixed");
        auto parsed = read_request(flags.request_file);
        if (!parsed)
            return parsed.error();
        request = std::move(parsed).value();
    }
    else
    {
        auto object = binding(initial.value());
        if (!flags.asset_id.empty())
            object["asset_id"] = std::string(flags.asset_id);
        if (flags.expected_session_revision)
            object["expected_session_revision"] =
                JsonValue::number(std::to_string(*flags.expected_session_revision));
        if (flags.expected_selection_revision)
            object["expected_selection_revision"] =
                JsonValue::number(std::to_string(*flags.expected_selection_revision));
        if (flags.expected_recipe_revision)
            object["expected_recipe_revision"] =
                JsonValue::number(std::to_string(*flags.expected_recipe_revision));
        if (flags.expected_catalog_revision)
            object["expected_catalog_revision"] =
                JsonValue::number(std::to_string(*flags.expected_catalog_revision));
        object.emplace("fields", flags.fields);
        if (!flags.local_id.empty())
            object.emplace("local_id", std::string(flags.local_id));
        request = JsonValue{std::move(object)};
    }
    static const std::set<std::string, std::less<>> allowed{"session_id",
                                                            "asset_id",
                                                            "fields",
                                                            "local_id",
                                                            "expected_session_revision",
                                                            "expected_selection_revision",
                                                            "expected_recipe_revision",
                                                            "expected_catalog_revision"};
    for (const auto &[key, value] : *request.object_if())
    {
        static_cast<void>(value);
        if (!allowed.contains(key))
            return make_error(ErrorCode::kInvalidArgument, "Unknown evaluation field",
                              {{"field", key}});
    }
    auto checked = check_binding(request, initial.value());
    if (!checked)
        return checked.error();
    const auto *fields = request.find("fields");
    if (!fields)
        return make_error(ErrorCode::kInvalidArgument, "Evaluation fields are missing");
    std::string local_id;
    if (const auto *local = request.find("local_id"))
    {
        if (!local->string_if() || local->string_if()->empty())
            return make_error(ErrorCode::kInvalidArgument, "local_id is invalid");
        local_id = *local->string_if();
    }
    auto ready = wait_for_studio_state(
        session.value(), initial.value().asset_id, initial.value().current_recipe_json, false,
        false, remaining(), initial.value().selection_revision, initial.value().recipe_revision);
    if (!ready)
        return ready.error();
    checked = check_binding(request, ready.value());
    if (!checked)
        return checked.error();
    const auto *scope = ready.value().json.find("editing_scope");
    const auto *active_id = scope ? scope->find("local_id") : nullptr;
    if (!active_id || !active_id->string_if() || *active_id->string_if() != local_id)
        return make_error(ErrorCode::kConflict, "Evaluation editing scope changed",
                          {{"reason", "stale_editing_scope"}});
    auto base = parse_recipe_json(ready.value().current_recipe_json);
    if (!base)
        return base.error();
    auto params = develop_from_recipe(base.value());
    if (!params)
        return params.error();
    auto trial = apply_develop_patch(params.value(), *fields, local_id);
    if (!trial)
        return trial.error();
    auto target = recipe_from_develop(base.value().asset, trial.value());
    if (!target)
        return target.error();
    auto serialized = serialize_recipe(target.value());
    if (!serialized)
        return serialized.error();
    auto rendered =
        render_studio_artifact(engine, session.value(), ready.value(), flags.output,
                               flags.max_edge.value_or(960), remaining(), &target.value());
    if (!rendered)
        return rendered.error();
    auto commit = binding(ready.value());
    commit.emplace("kind", "patch");
    commit.emplace("fields", *fields);
    if (!local_id.empty())
        commit.emplace("local_id", local_id);
    commit.emplace("base_recipe_sha256", sha256_utf8_hex(ready.value().current_recipe_json));
    commit.emplace("target_recipe_sha256", sha256_utf8_hex(serialized.value()));
    auto result = *rendered.value().object_if();
    result.emplace("type", "ravo.develop.evaluation");
    result.emplace("version", JsonValue::number("1"));
    result.emplace("candidate", JsonValue::Object{{"type", "ravo.develop.candidate"},
                                                  {"version", JsonValue::number("1")},
                                                  {"session_id", session.value().session_id},
                                                  {"asset_id", ready.value().asset_id},
                                                  {"commit_request", std::move(commit)}});
    return JsonValue{std::move(result)};
}
} // namespace ravo::cli_internal
