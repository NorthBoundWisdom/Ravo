#include "application_internal.h"

#include "ravo/services/external_editor.h"

#include <QProcess>
#include <QString>
#include <QStringList>

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue fingerprint_json(const ExternalEditorFileFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] JsonValue provenance_to_json(const ExternalEditorProvenance &provenance)
{
    JsonValue::Object object{
        {"schema", provenance.schema},
        {"schema_version", JsonValue::number(std::to_string(provenance.schema_version))},
        {"derived_asset_id", provenance.derived_asset_id},
        {"source_asset_id", provenance.source_asset_id},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(provenance.observed_catalog_revision))},
        {"source_recovery_generation",
         JsonValue::number(std::to_string(provenance.source_recovery_generation))},
        {"source_original", fingerprint_json(provenance.source_original)},
        {"derived", fingerprint_json(provenance.derived)},
        {"editor_id", provenance.editor_id},
        {"registered_unix_ms", JsonValue::number(std::to_string(provenance.registered_unix_ms))},
        {"derived_path", provenance.derived_path},
        {"source_original_path", provenance.source_original_path},
    };
    if (provenance.editor_version)
        object.emplace("editor_version", *provenance.editor_version);
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue open_intent_to_json(const ExternalEditorOpenIntent &intent)
{
    JsonValue::Object object{
        {"schema", intent.schema},
        {"schema_version", JsonValue::number(std::to_string(intent.schema_version))},
        {"intent_id", intent.intent_id},
        {"asset_id", intent.asset_id},
        {"source_asset_id", intent.source_asset_id},
        {"open_path", intent.open_path},
        {"open_uri", intent.open_uri},
        {"open_kind", std::string(external_editor_open_kind_name(intent.open_kind))},
        {"recorded_unix_ms", JsonValue::number(std::to_string(intent.recorded_unix_ms))},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(intent.observed_catalog_revision))},
    };
    if (intent.editor_id)
        object.emplace("editor_id", *intent.editor_id);
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<bool> invoke_platform_os_open(const std::string &absolute_path)
{
    if (absolute_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "OS open path is empty",
                          {{"reason", "missing_open_path"}});
    }
    QString program;
    QStringList arguments;
#if defined(Q_OS_MACOS)
    program = QStringLiteral("open");
    arguments = {QString::fromStdString(absolute_path)};
#elif defined(Q_OS_WIN)
    program = QStringLiteral("cmd");
    arguments = {QStringLiteral("/C"), QStringLiteral("start"), QStringLiteral(""),
                 QString::fromStdString(absolute_path)};
#else
    program = QStringLiteral("xdg-open");
    arguments = {QString::fromStdString(absolute_path)};
#endif
    if (!QProcess::startDetached(program, arguments))
    {
        return make_error(ErrorCode::kIo, "Unable to invoke platform open-with",
                          {{"path", absolute_path},
                           {"program", program.toStdString()},
                           {"reason", "os_open_invoke_failed"}});
    }
    return true;
}

[[nodiscard]] JsonValue working_copy_session_json(const ExternalEditorWorkingCopySession &session)
{
    JsonValue::Object object{
        {"schema", session.schema},
        {"schema_version", JsonValue::number(std::to_string(session.schema_version))},
        {"working_copy_id", session.working_copy_id},
        {"source_asset_id", session.source_asset_id},
        {"editor_id", session.editor_id},
        {"working_path", session.working_path},
        {"working_uri", session.working_uri},
        {"source_original", fingerprint_json(session.source_original)},
        {"working_copy", fingerprint_json(session.working_copy)},
        {"tiff_sample_type", std::string(tiff_sample_type_name(session.tiff_sample_type))},
        {"profile", session.profile},
        {"auto_stack", session.auto_stack},
        {"created_unix_ms", JsonValue::number(std::to_string(session.created_unix_ms))},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(session.observed_catalog_revision))},
    };
    if (session.editor_version)
        object.emplace("editor_version", *session.editor_version);
    if (session.max_edge)
        object.emplace("max_edge", JsonValue::number(std::to_string(*session.max_edge)));
    if (session.open_intent_id)
        object.emplace("open_intent_id", *session.open_intent_id);
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue working_copy_status_json(const ExternalEditorWorkingCopyStatus &status)
{
    return JsonValue{JsonValue::Object{
        {"schema", status.schema},
        {"machine_state",
         std::string(external_editor_working_copy_machine_state_name(status.machine_state))},
        {"working_copy_present", status.working_copy_present},
        {"working_copy_modified", status.working_copy_modified},
        {"source_original_unchanged", status.source_original_unchanged},
        {"catalog_revision_current", status.catalog_revision_current},
        {"reason", status.reason},
        {"session", working_copy_session_json(status.session)},
    }};
}

} // namespace

Result<JsonValue> run_catalog_editor_command(CatalogService &service,
                                             const std::string_view subcommand,
                                             const CatalogCliArguments &flags)
{
    if (subcommand == "editor-register")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires --asset-id");
        }
        if (flags.inputs.size() != 1U)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires exactly one --input");
        }
        if (flags.editor_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-register requires --editor");
        }
        ExternalEditorRegisterRequest request;
        request.source_asset_id = std::string(flags.asset_id);
        request.editor_output_path = std::string(flags.inputs.front());
        request.editor_id = std::string(flags.editor_id);
        if (!flags.editor_version.empty())
            request.editor_version = std::string(flags.editor_version);
        if (!flags.import_destination.empty())
            request.destination_directory = std::string(flags.import_destination);
        request.expected_catalog_revision = flags.expected_revision;
        request.auto_stack = flags.editor_auto_stack;
        auto registered = service.register_external_editor_output(request);
        if (!registered)
            return registered.error();
        auto asset_json = asset_to_json(registered.value().derived_asset);
        JsonValue::Object object =
            asset_json.object_if() != nullptr ? *asset_json.object_if() : JsonValue::Object{};
        object.emplace("provenance", provenance_to_json(registered.value().provenance));
        object.emplace("source_original_unchanged", registered.value().source_original_unchanged);
        object.emplace("auto_stacked", registered.value().auto_stacked);
        if (registered.value().stack)
            object.emplace("stack", library_stack_mutation_to_json(*registered.value().stack));
        return JsonValue{std::move(object)};
    }
    if (subcommand == "editor-show")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-show requires --asset-id <derived-id>");
        }
        auto provenance = service.external_editor_provenance(flags.asset_id);
        if (!provenance)
            return provenance.error();
        return provenance_to_json(provenance.value());
    }
    if (subcommand == "editor-open")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-open requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-open requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        ExternalEditorOpenRequest request;
        request.asset_id = std::string(flags.asset_id);
        if (!flags.editor_id.empty())
            request.editor_id = std::string(flags.editor_id);
        request.expected_catalog_revision = flags.expected_revision;
        request.user_initiated = true;
        auto opened = service.prepare_external_editor_open(request);
        if (!opened)
            return opened.error();

        bool os_open_invoked = false;
        if (flags.editor_invoke_os_open)
        {
            auto invoked = invoke_platform_os_open(opened.value().intent.open_path);
            if (!invoked)
                return invoked.error();
            os_open_invoked = true;
        }

        auto intent_json = open_intent_to_json(opened.value().intent);
        JsonValue::Object object =
            intent_json.object_if() != nullptr ? *intent_json.object_if() : JsonValue::Object{};
        object.emplace("os_open_invoked", os_open_invoked);
        return JsonValue{std::move(object)};
    }

    if (subcommand == "editor-prepare-working-copy")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-prepare-working-copy requires --asset-id");
        }
        if (flags.editor_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-prepare-working-copy requires --editor");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-prepare-working-copy requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        ExternalEditorWorkingCopyRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.editor_id = std::string(flags.editor_id);
        if (!flags.editor_version.empty())
            request.editor_version = std::string(flags.editor_version);
        if (!flags.application_path.empty())
            request.application_path = std::string(flags.application_path);
        request.user_initiated = true;
        request.auto_stack = true;
        if (!flags.tiff_sample_type.empty())
        {
            auto sample = parse_tiff_sample_type(flags.tiff_sample_type);
            if (!sample)
                return sample.error();
            request.tiff_sample_type = sample.value();
        }
        if (flags.max_edge)
            request.max_edge = *flags.max_edge;
        request.expected_catalog_revision = flags.expected_revision;
        auto prepared = service.create_external_editor_working_copy(request);
        if (!prepared)
            return prepared.error();

        bool os_open_invoked = false;
        if (flags.editor_invoke_os_open)
        {
            auto invoked = invoke_platform_os_open(prepared.value().session.working_path);
            if (!invoked)
                return invoked.error();
            os_open_invoked = true;
        }
        return JsonValue{JsonValue::Object{
            {"session", working_copy_session_json(prepared.value().session)},
            {"originals_unchanged", prepared.value().originals_unchanged},
            {"os_open_invoked", os_open_invoked},
        }};
    }
    if (subcommand == "editor-check-returned")
    {
        if (flags.working_copy_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-check-returned requires --working-copy-id");
        }
        ExternalEditorCheckReturnedRequest request;
        request.working_copy_id = std::string(flags.working_copy_id);
        if (!flags.inputs.empty())
            request.returned_path = std::string(flags.inputs.front());
        request.expected_catalog_revision = flags.expected_revision;
        auto checked = service.check_external_editor_returned(request);
        if (!checked)
            return checked.error();
        auto asset_json = asset_to_json(checked.value().registration.derived_asset);
        JsonValue::Object object =
            asset_json.object_if() != nullptr ? *asset_json.object_if() : JsonValue::Object{};
        object.emplace("provenance", provenance_to_json(checked.value().registration.provenance));
        object.emplace("source_original_unchanged",
                       checked.value().registration.source_original_unchanged);
        object.emplace("auto_stacked", checked.value().registration.auto_stacked);
        if (checked.value().registration.stack)
            object.emplace("stack",
                           library_stack_mutation_to_json(*checked.value().registration.stack));
        object.emplace("session", working_copy_session_json(checked.value().session));
        return JsonValue{std::move(object)};
    }
    if (subcommand == "editor-working-copy-status")
    {
        if (flags.working_copy_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-working-copy-status requires --working-copy-id");
        }
        auto status = service.external_editor_working_copy_status(flags.working_copy_id);
        if (!status)
            return status.error();
        return working_copy_status_json(status.value());
    }
    if (subcommand == "editor-working-copy-list")
    {
        std::optional<std::string_view> asset_filter;
        if (!flags.asset_id.empty())
            asset_filter = flags.asset_id;
        auto listed = service.list_external_editor_working_copies(asset_filter);
        if (!listed)
            return listed.error();
        JsonValue::Array items;
        items.reserve(listed.value().size());
        for (const auto &session : listed.value())
            items.push_back(working_copy_session_json(session));
        const auto count = items.size();
        return JsonValue{JsonValue::Object{
            {"sessions", JsonValue{std::move(items)}},
            {"count", JsonValue::number(std::to_string(count))},
        }};
    }
    if (subcommand == "editor-abandon-working-copy")
    {
        if (flags.working_copy_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-abandon-working-copy requires --working-copy-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-abandon-working-copy requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        ExternalEditorAbandonRequest request;
        request.working_copy_id = std::string(flags.working_copy_id);
        request.user_initiated = true;
        request.expected_catalog_revision = flags.expected_revision;
        auto abandoned = service.abandon_external_editor_working_copy(request);
        if (!abandoned)
            return abandoned.error();
        return JsonValue{JsonValue::Object{
            {"working_copy_id", abandoned.value().working_copy_id},
            {"session_removed", abandoned.value().session_removed},
            {"working_copy_removed", abandoned.value().working_copy_removed},
            {"originals_unchanged", abandoned.value().originals_unchanged},
        }};
    }
    if (subcommand == "editor-reopen-working-copy")
    {
        if (flags.working_copy_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-reopen-working-copy requires --working-copy-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog editor-reopen-working-copy requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        ExternalEditorReopenRequest request;
        request.working_copy_id = std::string(flags.working_copy_id);
        request.user_initiated = true;
        auto reopened = service.reopen_external_editor_working_copy(request);
        if (!reopened)
            return reopened.error();
        bool os_open_invoked = false;
        const auto state = reopened.value().status.machine_state;
        const bool openable = state == ExternalEditorWorkingCopyMachineState::kPending ||
                              state == ExternalEditorWorkingCopyMachineState::kModified ||
                              state == ExternalEditorWorkingCopyMachineState::kStaleCatalog;
        if (flags.editor_invoke_os_open)
        {
            if (!openable || !reopened.value().status.working_copy_present)
            {
                return make_error(
                    ErrorCode::kConflict,
                    "Working copy cannot be reopened in the current machine state",
                    {{"working_copy_id", request.working_copy_id},
                     {"reason", reopened.value().status.reason},
                     {"machine_state",
                      std::string(external_editor_working_copy_machine_state_name(state))}});
            }
            auto invoked = invoke_platform_os_open(reopened.value().status.session.working_path);
            if (!invoked)
                return invoked.error();
            os_open_invoked = true;
        }
        auto status_json = working_copy_status_json(reopened.value().status);
        JsonValue::Object object =
            status_json.object_if() != nullptr ? *status_json.object_if() : JsonValue::Object{};
        object.emplace("os_open_invoked", os_open_invoked);
        return JsonValue{std::move(object)};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog editor subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
