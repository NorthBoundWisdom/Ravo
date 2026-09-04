#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/services/external_editor.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool safe_path_component(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > 128U)
        return false;
    for (const char raw : value)
    {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0 || raw == '_' || raw == '-' || raw == '.')
            continue;
        return false;
    }
    return value != "." && value != "..";
}

[[nodiscard]] std::string working_copy_root(const std::string_view database_path)
{
    return std::string(database_path) + ".ravo/external-editor/working-copies";
}

[[nodiscard]] std::string working_copy_dir(const std::string_view database_path,
                                           const std::string_view working_copy_id)
{
    return working_copy_root(database_path) + "/" + std::string(working_copy_id);
}

[[nodiscard]] std::string working_copy_session_path(const std::string_view database_path,
                                                    const std::string_view working_copy_id)
{
    return working_copy_dir(database_path, working_copy_id) + "/session.json";
}

[[nodiscard]] std::string working_copy_raster_path(const std::string_view database_path,
                                                   const std::string_view working_copy_id)
{
    return working_copy_dir(database_path, working_copy_id) + "/working.tif";
}

[[nodiscard]] Result<void> ensure_directory(const std::string_view path_utf8,
                                            const std::string_view reason)
{
    std::error_code error;
    std::filesystem::create_directories(utf8_path(path_utf8), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create external-editor working-copy directory",
                          {{"path", std::string(path_utf8)},
                           {"reason", std::string(reason)},
                           {"detail", error.message()}});
    }
    return {};
}

void best_effort_remove_tree(const std::string_view path_utf8)
{
    std::error_code error;
    std::filesystem::remove_all(utf8_path(path_utf8), error);
}

[[nodiscard]] Result<ExternalEditorFileFingerprint> fingerprint_file(const std::string_view path)
{
    auto digest = sha256_file_hex(path);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
    ExternalEditorFileFingerprint fingerprint;
    fingerprint.sha256 = std::move(digest).value();
    fingerprint.size_bytes = identity.value().size_bytes;
    fingerprint.mtime_unix_ms = identity.value().mtime_unix_ms;
    return fingerprint;
}

[[nodiscard]] JsonValue fingerprint_json(const ExternalEditorFileFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] Result<ExternalEditorFileFingerprint>
fingerprint_from_json(const JsonValue &value, const std::string_view field)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Working-copy fingerprint is not an object",
            {{"field", std::string(field)}, {"reason", "invalid_external_editor_working_copy"}});
    }
    const auto *sha = value.find("sha256");
    const auto *size = value.find("size_bytes");
    const auto *mtime = value.find("mtime_unix_ms");
    if (sha == nullptr || sha->string_if() == nullptr || size == nullptr ||
        size->number_if() == nullptr || mtime == nullptr || mtime->number_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Working-copy fingerprint is incomplete",
            {{"field", std::string(field)}, {"reason", "invalid_external_editor_working_copy"}});
    }
    ExternalEditorFileFingerprint fingerprint;
    fingerprint.sha256 = *sha->string_if();
    fingerprint.size_bytes = static_cast<std::uint64_t>(std::stoull(size->number_if()->text));
    fingerprint.mtime_unix_ms = std::stoll(mtime->number_if()->text);
    return fingerprint;
}

[[nodiscard]] Result<void> write_session(const std::string_view database_path,
                                         const ExternalEditorWorkingCopySession &session)
{
    auto created = ensure_directory(working_copy_dir(database_path, session.working_copy_id),
                                    "external_editor_working_copy_create_failed");
    if (!created)
        return created.error();

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
    const auto path = working_copy_session_path(database_path, session.working_copy_id);
    return write_utf8_text_file_atomically(path, serialize_json(JsonValue{std::move(object)}));
}

[[nodiscard]] Result<ExternalEditorWorkingCopySession>
load_session(const std::string_view database_path, const std::string_view working_copy_id)
{
    const auto path = working_copy_session_path(database_path, working_copy_id);
    auto text = read_utf8_text_file(path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Working-copy session is not an object",
                          {{"path", path}, {"reason", "invalid_external_editor_working_copy"}});
    }
    const auto require_string = [&](const char *field) -> Result<std::string>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->string_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Working-copy session field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_external_editor_working_copy"}});
        }
        return *value->string_if();
    };
    const auto require_i64 = [&](const char *field) -> Result<std::int64_t>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->number_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Working-copy session field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_external_editor_working_copy"}});
        }
        return std::stoll(value->number_if()->text);
    };
    const auto require_bool = [&](const char *field) -> Result<bool>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->boolean_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Working-copy session field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_external_editor_working_copy"}});
        }
        return *value->boolean_if();
    };

    ExternalEditorWorkingCopySession session;
    auto schema = require_string("schema");
    if (!schema)
        return schema.error();
    session.schema = std::move(schema).value();
    auto schema_version = require_i64("schema_version");
    if (!schema_version)
        return schema_version.error();
    session.schema_version = schema_version.value();
    auto working_copy_id_value = require_string("working_copy_id");
    if (!working_copy_id_value)
        return working_copy_id_value.error();
    session.working_copy_id = std::move(working_copy_id_value).value();
    auto source_asset_id = require_string("source_asset_id");
    if (!source_asset_id)
        return source_asset_id.error();
    session.source_asset_id = std::move(source_asset_id).value();
    auto editor_id = require_string("editor_id");
    if (!editor_id)
        return editor_id.error();
    session.editor_id = std::move(editor_id).value();
    auto working_path = require_string("working_path");
    if (!working_path)
        return working_path.error();
    session.working_path = std::move(working_path).value();
    auto working_uri = require_string("working_uri");
    if (!working_uri)
        return working_uri.error();
    session.working_uri = std::move(working_uri).value();
    const auto *source_fp = parsed.value().find("source_original");
    const auto *working_fp = parsed.value().find("working_copy");
    if (source_fp == nullptr || working_fp == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Working-copy session fingerprints missing",
                          {{"path", path}, {"reason", "invalid_external_editor_working_copy"}});
    }
    auto source_original = fingerprint_from_json(*source_fp, "source_original");
    if (!source_original)
        return source_original.error();
    session.source_original = std::move(source_original).value();
    auto working_copy = fingerprint_from_json(*working_fp, "working_copy");
    if (!working_copy)
        return working_copy.error();
    session.working_copy = std::move(working_copy).value();
    auto sample = require_string("tiff_sample_type");
    if (!sample)
        return sample.error();
    auto sample_type = parse_tiff_sample_type(sample.value());
    if (!sample_type)
        return sample_type.error();
    session.tiff_sample_type = sample_type.value();
    auto profile = require_string("profile");
    if (!profile)
        return profile.error();
    session.profile = std::move(profile).value();
    auto auto_stack = require_bool("auto_stack");
    if (!auto_stack)
        return auto_stack.error();
    session.auto_stack = auto_stack.value();
    auto created = require_i64("created_unix_ms");
    if (!created)
        return created.error();
    session.created_unix_ms = created.value();
    auto revision = require_i64("observed_catalog_revision");
    if (!revision)
        return revision.error();
    session.observed_catalog_revision = revision.value();
    if (const auto *editor_version = parsed.value().find("editor_version");
        editor_version != nullptr && editor_version->string_if() != nullptr)
        session.editor_version = *editor_version->string_if();
    if (const auto *max_edge = parsed.value().find("max_edge");
        max_edge != nullptr && max_edge->number_if() != nullptr)
        session.max_edge = static_cast<std::uint32_t>(std::stoul(max_edge->number_if()->text));
    if (const auto *open_intent = parsed.value().find("open_intent_id");
        open_intent != nullptr && open_intent->string_if() != nullptr)
        session.open_intent_id = *open_intent->string_if();
    return session;
}

[[nodiscard]] bool file_is_regular(const std::string_view path) noexcept
{
    std::error_code error;
    return std::filesystem::is_regular_file(utf8_path(path), error) && !error;
}

} // namespace

Result<ExternalEditorWorkingCopyResult>
CatalogService::create_external_editor_working_copy(const ExternalEditorWorkingCopyRequest &request)
{
    if (repository_ == nullptr || engine_ == nullptr || raster_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "External-editor working copy requires explicit user initiation",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.asset_id.empty() || !safe_path_component(request.asset_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Asset id is invalid",
                          {{"asset_id", request.asset_id}, {"reason", "invalid_asset_id"}});
    }
    if (request.editor_id.empty() || !safe_path_component(request.editor_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "External-editor id is invalid",
                          {{"editor_id", request.editor_id}, {"reason", "invalid_editor_id"}});
    }
    if (request.profile != "srgb")
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "External-editor working copy v1 only supports profile=srgb",
            {{"profile", request.profile}, {"reason", "unsupported_working_copy_profile"}});
    }
    if (request.application_path)
    {
        if (request.application_path->empty() || !file_is_regular(*request.application_path))
        {
            return make_error(ErrorCode::kNotFound, "External editor application is missing",
                              {{"application_path", *request.application_path},
                               {"reason", "missing_application"}});
        }
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (request.expected_catalog_revision &&
        *request.expected_catalog_revision != snapshot.value().revision)
    {
        return make_error(
            ErrorCode::kConflict, "Catalog revision does not match",
            {{"expected_revision", std::to_string(*request.expected_catalog_revision)},
             {"actual_revision", std::to_string(snapshot.value().revision)},
             {"reason", "stale_catalog_revision"}});
    }

    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", request.asset_id}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
        return location.error();
    if (!file_is_regular(location.value().path))
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"asset_id", request.asset_id},
                           {"path", location.value().path},
                           {"reason", "original_missing"}});
    }

    auto before = fingerprint_file(location.value().path);
    if (!before)
        return before.error();

    const auto working_copy_id = generate_external_editor_open_intent_id();
    const auto dir = working_copy_dir(snapshot.value().database_path, working_copy_id);
    best_effort_remove_tree(dir);
    auto created_dir = ensure_directory(dir, "external_editor_working_copy_create_failed");
    if (!created_dir)
        return created_dir.error();

    const auto raster_path =
        working_copy_raster_path(snapshot.value().database_path, working_copy_id);
    ExportRequest export_request;
    export_request.asset_id = request.asset_id;
    export_request.output_path = raster_path;
    export_request.format = ExportFormat::kTiff;
    export_request.tiff_options.sample_type = request.tiff_sample_type;
    export_request.tiff_options.compression = TiffCompression::kDeflatePredictor;
    export_request.metadata_mode = ExportMetadataMode::kFull;
    export_request.output_color.enabled = true;
    export_request.output_color.output_profile = request.profile;
    if (request.max_edge)
        export_request.max_edge = *request.max_edge;
    export_request.cancellation = request.cancellation;

    auto exported = export_asset(export_request);
    if (!exported)
    {
        best_effort_remove_tree(dir);
        return exported.error();
    }

    auto working_fp = fingerprint_file(raster_path);
    if (!working_fp)
    {
        best_effort_remove_tree(dir);
        return working_fp.error();
    }
    auto after = fingerprint_file(location.value().path);
    if (!after)
    {
        best_effort_remove_tree(dir);
        return after.error();
    }
    if (after.value().sha256 != before.value().sha256 ||
        after.value().size_bytes != before.value().size_bytes ||
        after.value().mtime_unix_ms != before.value().mtime_unix_ms)
    {
        best_effort_remove_tree(dir);
        return make_error(ErrorCode::kConflict,
                          "Source original changed during working-copy create",
                          {{"asset_id", request.asset_id},
                           {"path", location.value().path},
                           {"reason", "source_mutated_during_working_copy"}});
    }

    auto open_location = normalize_local_input(raster_path);
    if (!open_location)
    {
        best_effort_remove_tree(dir);
        return open_location.error();
    }

    ExternalEditorWorkingCopySession session;
    session.working_copy_id = working_copy_id;
    session.source_asset_id = request.asset_id;
    session.editor_id = request.editor_id;
    session.editor_version = request.editor_version;
    session.working_path = open_location.value().path;
    session.working_uri = open_location.value().uri;
    session.source_original = std::move(before).value();
    session.working_copy = std::move(working_fp).value();
    session.tiff_sample_type = request.tiff_sample_type;
    session.profile = request.profile;
    session.max_edge = request.max_edge;
    session.auto_stack = request.auto_stack;
    session.created_unix_ms = now_unix_ms();
    session.observed_catalog_revision = snapshot.value().revision;

    // Record open intent against the working copy path (derived kind).
    ExternalEditorOpenRequest open_request;
    open_request.asset_id = request.asset_id;
    open_request.editor_id = request.editor_id;
    open_request.user_initiated = true;
    open_request.expected_catalog_revision = snapshot.value().revision;
    open_request.cancellation = request.cancellation;
    // prepare_external_editor_open opens the original for non-derived assets.
    // For EDITIN we open the working TIFF path directly via a synthetic intent.
    ExternalEditorOpenIntent intent;
    intent.intent_id = generate_external_editor_open_intent_id();
    intent.asset_id = request.asset_id;
    intent.source_asset_id = request.asset_id;
    intent.open_path = session.working_path;
    intent.open_uri = session.working_uri;
    intent.open_kind = ExternalEditorOpenKind::kDerivedWorkingCopy;
    intent.editor_id = request.editor_id;
    intent.recorded_unix_ms = session.created_unix_ms;
    intent.observed_catalog_revision = snapshot.value().revision;
    // Persist intent beside other open intents using existing helper by writing
    // through prepare path is wrong for originals; write session open_intent_id only.
    session.open_intent_id = intent.intent_id;

    auto written = write_session(snapshot.value().database_path, session);
    if (!written)
    {
        best_effort_remove_tree(dir);
        return written.error();
    }

    // Also persist open-intent document for audit (best-effort via JSON write).
    {
        const auto intent_root =
            std::string(snapshot.value().database_path) + ".ravo/external-editor/open-intents";
        static_cast<void>(
            ensure_directory(intent_root, "external_editor_open_intent_create_failed"));
        JsonValue::Object intent_object{
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
            intent_object.emplace("editor_id", *intent.editor_id);
        static_cast<void>(
            write_utf8_text_file_atomically(intent_root + "/" + intent.intent_id + ".json",
                                            serialize_json(JsonValue{std::move(intent_object)})));
    }

    ExternalEditorWorkingCopyResult result;
    result.session = std::move(session);
    result.originals_unchanged = true;
    (void)exported;
    return result;
}

Result<ExternalEditorWorkingCopySession>
CatalogService::external_editor_working_copy_session(const std::string_view working_copy_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (working_copy_id.empty() || !safe_path_component(working_copy_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Working-copy id is invalid",
                          {{"working_copy_id", std::string(working_copy_id)},
                           {"reason", "invalid_working_copy_id"}});
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    return load_session(snapshot.value().database_path, working_copy_id);
}

Result<ExternalEditorCheckReturnedResult>
CatalogService::check_external_editor_returned(const ExternalEditorCheckReturnedRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    if (request.working_copy_id.empty() || !safe_path_component(request.working_copy_id))
    {
        return make_error(
            ErrorCode::kInvalidArgument, "Working-copy id is invalid",
            {{"working_copy_id", request.working_copy_id}, {"reason", "invalid_working_copy_id"}});
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (request.expected_catalog_revision &&
        *request.expected_catalog_revision != snapshot.value().revision)
    {
        return make_error(
            ErrorCode::kConflict, "Catalog revision does not match",
            {{"expected_revision", std::to_string(*request.expected_catalog_revision)},
             {"actual_revision", std::to_string(snapshot.value().revision)},
             {"reason", "stale_catalog_revision"}});
    }

    auto session = load_session(snapshot.value().database_path, request.working_copy_id);
    if (!session)
        return session.error();

    std::string returned_path = request.returned_path.value_or(session.value().working_path);
    auto returned_location = normalize_local_input(returned_path);
    if (!returned_location)
        return returned_location.error();
    if (!file_is_regular(returned_location.value().path))
    {
        return make_error(ErrorCode::kNotFound, "Returned editor file is missing",
                          {{"path", returned_location.value().path},
                           {"working_copy_id", request.working_copy_id},
                           {"reason", "editor_output_missing"}});
    }

    auto returned_fp = fingerprint_file(returned_location.value().path);
    if (!returned_fp)
        return returned_fp.error();
    if (returned_fp.value().sha256 == session.value().working_copy.sha256 &&
        returned_fp.value().size_bytes == session.value().working_copy.size_bytes)
    {
        return make_error(ErrorCode::kConflict, "Returned editor file is unchanged",
                          {{"path", returned_location.value().path},
                           {"working_copy_id", request.working_copy_id},
                           {"reason", "editor_output_unchanged"}});
    }

    ExternalEditorRegisterRequest register_request;
    register_request.source_asset_id = session.value().source_asset_id;
    register_request.editor_output_path = returned_location.value().path;
    register_request.editor_id = session.value().editor_id;
    register_request.editor_version = session.value().editor_version;
    register_request.expected_catalog_revision = request.expected_catalog_revision;
    register_request.auto_stack = session.value().auto_stack;
    register_request.cancellation = request.cancellation;

    auto registered = register_external_editor_output(register_request);
    if (!registered)
        return registered.error();

    ExternalEditorCheckReturnedResult result;
    result.registration = std::move(registered).value();
    result.session = std::move(session).value();
    return result;
}

} // namespace ravo
