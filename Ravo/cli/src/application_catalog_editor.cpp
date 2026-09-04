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
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog editor subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
