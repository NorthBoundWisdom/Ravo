#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"
#include "catalog_service_internal.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/services/external_editor.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string path_text(const std::filesystem::path &path)
{
    return catalog_service_internal::utf8_string(path.generic_u8string());
}

[[nodiscard]] std::string derived_store_root(const std::string_view database_path,
                                             const std::string_view source_asset_id)
{
    return std::string(database_path) + ".ravo/derived/" + std::string(source_asset_id);
}

[[nodiscard]] std::string provenance_root(const std::string_view database_path)
{
    return std::string(database_path) + ".ravo/external-editor";
}

[[nodiscard]] std::string provenance_path(const std::string_view database_path,
                                          const std::string_view derived_asset_id)
{
    return provenance_root(database_path) + "/" + std::string(derived_asset_id) + ".json";
}

[[nodiscard]] std::string open_intent_root(const std::string_view database_path)
{
    return provenance_root(database_path) + "/open-intents";
}

[[nodiscard]] std::string open_intent_path(const std::string_view database_path,
                                           const std::string_view intent_id)
{
    return open_intent_root(database_path) + "/" + std::string(intent_id) + ".json";
}

[[nodiscard]] bool path_under_derived_store(const std::string_view database_path,
                                            const std::string_view absolute_path) noexcept
{
    const std::string prefix = std::string(database_path) + ".ravo/derived/";
    if (absolute_path.size() < prefix.size())
        return false;
    return absolute_path.compare(0, prefix.size(), prefix) == 0;
}

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
    if (value == "." || value == "..")
        return false;
    return true;
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

[[nodiscard]] bool catalog_identity_matches(const AssetRecord &asset,
                                            const ExternalEditorFileFingerprint &fp) noexcept
{
    if (asset.size_bytes != fp.size_bytes || asset.mtime_unix_ms != fp.mtime_unix_ms)
        return false;
    if (!asset.content_fingerprint)
        return true;
    const auto expected = make_content_fingerprint(FileIdentity{fp.size_bytes, fp.mtime_unix_ms});
    return *asset.content_fingerprint == expected;
}

[[nodiscard]] Result<void> ensure_directory(const std::string_view path_utf8,
                                            const std::string_view reason)
{
    std::error_code error;
    std::filesystem::create_directories(utf8_path(path_utf8), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create external-editor directory",
                          {{"path", std::string(path_utf8)},
                           {"reason", std::string(reason)},
                           {"detail", error.message()}});
    }
    return {};
}

void best_effort_remove(const std::string_view path_utf8)
{
    std::error_code error;
    std::filesystem::remove(utf8_path(path_utf8), error);
}

[[nodiscard]] Result<std::string> sanitize_filename(const std::filesystem::path &source)
{
    auto name = path_text(source.filename());
    if (name.empty() || name == "." || name == "..")
        name = "editor-output.bin";
    for (char &ch : name)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.')
            continue;
        ch = '_';
    }
    if (name.size() > 180U)
        name.resize(180U);
    return name;
}

[[nodiscard]] JsonValue fingerprint_to_json(const ExternalEditorFileFingerprint &value)
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
            ErrorCode::kValidation, "External-editor fingerprint is not an object",
            {{"field", std::string(field)}, {"reason", "invalid_external_editor_provenance"}});
    }
    const auto *sha = value.find("sha256");
    const auto *size = value.find("size_bytes");
    const auto *mtime = value.find("mtime_unix_ms");
    if (sha == nullptr || sha->string_if() == nullptr || size == nullptr ||
        size->number_if() == nullptr || mtime == nullptr || mtime->number_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "External-editor fingerprint is incomplete",
            {{"field", std::string(field)}, {"reason", "invalid_external_editor_provenance"}});
    }
    ExternalEditorFileFingerprint fingerprint;
    fingerprint.sha256 = *sha->string_if();
    fingerprint.size_bytes = static_cast<std::uint64_t>(std::stoull(size->number_if()->text));
    fingerprint.mtime_unix_ms = std::stoll(mtime->number_if()->text);
    return fingerprint;
}

[[nodiscard]] Result<void> write_provenance(const std::string_view database_path,
                                            const ExternalEditorProvenance &provenance)
{
    auto created = ensure_directory(provenance_root(database_path),
                                    "external_editor_provenance_create_failed");
    if (!created)
        return created.error();

    JsonValue::Object object{
        {"schema", provenance.schema},
        {"schema_version", JsonValue::number(std::to_string(provenance.schema_version))},
        {"derived_asset_id", provenance.derived_asset_id},
        {"source_asset_id", provenance.source_asset_id},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(provenance.observed_catalog_revision))},
        {"source_recovery_generation",
         JsonValue::number(std::to_string(provenance.source_recovery_generation))},
        {"source_original", fingerprint_to_json(provenance.source_original)},
        {"derived", fingerprint_to_json(provenance.derived)},
        {"editor_id", provenance.editor_id},
        {"registered_unix_ms", JsonValue::number(std::to_string(provenance.registered_unix_ms))},
        {"derived_path", provenance.derived_path},
        {"source_original_path", provenance.source_original_path},
    };
    if (provenance.editor_version)
        object.emplace("editor_version", *provenance.editor_version);

    const auto path = provenance_path(database_path, provenance.derived_asset_id);
    return write_utf8_text_file_atomically(path, serialize_json(JsonValue{std::move(object)}));
}

[[nodiscard]] Result<ExternalEditorProvenance>
load_provenance(const std::string_view database_path, const std::string_view derived_asset_id)
{
    const auto path = provenance_path(database_path, derived_asset_id);
    std::error_code error;
    if (!std::filesystem::is_regular_file(utf8_path(path), error))
    {
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect external-editor provenance",
                              {{"path", path},
                               {"reason", "external_editor_provenance_inspect_failed"},
                               {"detail", error.message()}});
        }
        return make_error(ErrorCode::kNotFound, "External-editor provenance was not found",
                          {{"derived_asset_id", std::string(derived_asset_id)},
                           {"path", path},
                           {"reason", "external_editor_provenance_missing"}});
    }
    auto text = read_utf8_text_file(path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "External-editor provenance is not an object",
                          {{"path", path}, {"reason", "invalid_external_editor_provenance"}});
    }
    const auto *schema = parsed.value().find("schema");
    const auto *schema_version = parsed.value().find("schema_version");
    if (schema == nullptr || schema->string_if() == nullptr ||
        *schema->string_if() != kExternalEditorDerivedContractVersion ||
        schema_version == nullptr || schema_version->number_if() == nullptr ||
        schema_version->number_if()->text != std::to_string(kExternalEditorDerivedSchemaVersion))
    {
        return make_error(ErrorCode::kValidation,
                          "External-editor provenance schema is unsupported",
                          {{"path", path}, {"reason", "unsupported_external_editor_provenance"}});
    }

    ExternalEditorProvenance provenance;
    const auto require_string = [&](const char *key) -> Result<std::string>
    {
        const auto *value = parsed.value().find(key);
        if (value == nullptr || value->string_if() == nullptr)
        {
            return make_error(
                ErrorCode::kValidation, "External-editor provenance is incomplete",
                {{"path", path}, {"field", key}, {"reason", "invalid_external_editor_provenance"}});
        }
        return *value->string_if();
    };
    const auto require_i64 = [&](const char *key) -> Result<std::int64_t>
    {
        const auto *value = parsed.value().find(key);
        if (value == nullptr || value->number_if() == nullptr)
        {
            return make_error(
                ErrorCode::kValidation, "External-editor provenance is incomplete",
                {{"path", path}, {"field", key}, {"reason", "invalid_external_editor_provenance"}});
        }
        return std::stoll(value->number_if()->text);
    };

    auto derived_id = require_string("derived_asset_id");
    if (!derived_id)
        return derived_id.error();
    auto source_id = require_string("source_asset_id");
    if (!source_id)
        return source_id.error();
    auto revision = require_i64("observed_catalog_revision");
    if (!revision)
        return revision.error();
    auto generation = require_i64("source_recovery_generation");
    if (!generation)
        return generation.error();
    auto editor_id = require_string("editor_id");
    if (!editor_id)
        return editor_id.error();
    auto registered = require_i64("registered_unix_ms");
    if (!registered)
        return registered.error();
    auto derived_path = require_string("derived_path");
    if (!derived_path)
        return derived_path.error();
    auto source_path = require_string("source_original_path");
    if (!source_path)
        return source_path.error();

    const auto *source_fp = parsed.value().find("source_original");
    const auto *derived_fp = parsed.value().find("derived");
    if (source_fp == nullptr || derived_fp == nullptr)
    {
        return make_error(ErrorCode::kValidation, "External-editor provenance is incomplete",
                          {{"path", path}, {"reason", "invalid_external_editor_provenance"}});
    }
    auto source_fingerprint = fingerprint_from_json(*source_fp, "source_original");
    if (!source_fingerprint)
        return source_fingerprint.error();
    auto derived_fingerprint = fingerprint_from_json(*derived_fp, "derived");
    if (!derived_fingerprint)
        return derived_fingerprint.error();

    provenance.derived_asset_id = std::move(derived_id).value();
    provenance.source_asset_id = std::move(source_id).value();
    provenance.observed_catalog_revision = revision.value();
    provenance.source_recovery_generation = generation.value();
    provenance.source_original = std::move(source_fingerprint).value();
    provenance.derived = std::move(derived_fingerprint).value();
    provenance.editor_id = std::move(editor_id).value();
    provenance.registered_unix_ms = registered.value();
    provenance.derived_path = std::move(derived_path).value();
    provenance.source_original_path = std::move(source_path).value();
    const auto *editor_version = parsed.value().find("editor_version");
    if (editor_version != nullptr && editor_version->string_if() != nullptr)
        provenance.editor_version = *editor_version->string_if();
    return provenance;
}

[[nodiscard]] Result<void> write_open_intent(const std::string_view database_path,
                                             const ExternalEditorOpenIntent &intent)
{
    auto created = ensure_directory(open_intent_root(database_path),
                                    "external_editor_open_intent_create_failed");
    if (!created)
        return created.error();

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

    const auto path = open_intent_path(database_path, intent.intent_id);
    return write_utf8_text_file_atomically(path, serialize_json(JsonValue{std::move(object)}));
}

} // namespace

Result<ExternalEditorRegisterResult>
CatalogService::register_external_editor_output(const ExternalEditorRegisterRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    if (request.source_asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "External-editor register requires a source asset",
                          {{"reason", "missing_source_asset_id"}});
    }
    if (request.editor_output_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "External-editor register requires an editor output path",
                          {{"reason", "missing_editor_output_path"}});
    }
    if (request.editor_id.empty() || !safe_path_component(request.editor_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "External-editor id is invalid",
                          {{"editor_id", request.editor_id}, {"reason", "invalid_editor_id"}});
    }
    if (request.editor_version && !safe_path_component(*request.editor_version))
    {
        return make_error(
            ErrorCode::kInvalidArgument, "External-editor version is invalid",
            {{"editor_version", *request.editor_version}, {"reason", "invalid_editor_version"}});
    }
    if (!safe_path_component(request.source_asset_id))
    {
        return make_error(
            ErrorCode::kInvalidArgument, "Source asset id is invalid",
            {{"asset_id", request.source_asset_id}, {"reason", "invalid_source_asset_id"}});
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

    auto source = repository_->find_asset_by_id(request.source_asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", request.source_asset_id}});
    }

    auto source_location = normalize_local_input(source.value()->normalized_uri);
    if (!source_location)
        return source_location.error();
    auto source_fp = fingerprint_file(source_location.value().path);
    if (!source_fp)
        return source_fp.error();
    if (!catalog_identity_matches(*source.value(), source_fp.value()))
    {
        return make_error(ErrorCode::kConflict,
                          "Source original no longer matches catalog identity",
                          {{"asset_id", request.source_asset_id},
                           {"path", source_location.value().path},
                           {"reason", "source_identity_mismatch"}});
    }

    auto output_location = normalize_local_input(request.editor_output_path);
    if (!output_location)
        return output_location.error();
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(utf8_path(output_location.value().path), status_error))
    {
        return make_error(ErrorCode::kNotFound, "Editor output file was not found",
                          {{"path", output_location.value().path},
                           {"reason", "editor_output_missing"},
                           {"detail", status_error ? status_error.message() : ""}});
    }

    std::error_code equivalent_error;
    if (std::filesystem::equivalent(utf8_path(source_location.value().path),
                                    utf8_path(output_location.value().path), equivalent_error) &&
        !equivalent_error)
    {
        return make_error(ErrorCode::kConflict, "Editor output must not be the source original",
                          {{"path", output_location.value().path},
                           {"reason", "editor_output_is_source_original"}});
    }

    auto output_fp = fingerprint_file(output_location.value().path);
    if (!output_fp)
        return output_fp.error();

    cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    std::string destination_root;
    if (request.destination_directory && !request.destination_directory->empty())
    {
        auto destination = normalize_local_input(*request.destination_directory);
        if (!destination)
            return destination.error();
        destination_root = destination.value().path;
    }
    else
    {
        destination_root =
            derived_store_root(snapshot.value().database_path, request.source_asset_id);
    }

    auto dir_ok = ensure_directory(destination_root, "external_editor_derived_create_failed");
    if (!dir_ok)
        return dir_ok.error();

    auto filename = sanitize_filename(utf8_path(output_location.value().path));
    if (!filename)
        return filename.error();
    const std::string derived_name =
        output_fp.value().sha256.substr(0, 16) + "-" + filename.value();
    const std::string derived_path = destination_root + "/" + derived_name;

    std::error_code exists_error;
    if (std::filesystem::exists(utf8_path(derived_path), exists_error) || exists_error)
    {
        if (exists_error)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect derived destination",
                              {{"path", derived_path},
                               {"reason", "derived_destination_inspect_failed"},
                               {"detail", exists_error.message()}});
        }
        return make_error(ErrorCode::kConflict, "Derived destination already exists",
                          {{"path", derived_path}, {"reason", "derived_destination_exists"}});
    }

    auto copied =
        copy_file_atomically(output_location.value().path, derived_path, request.cancellation);
    if (!copied)
    {
        best_effort_remove(derived_path);
        return copied.error();
    }

    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        best_effort_remove(derived_path);
        return cancelled.error();
    }

    auto imported =
        import_one(derived_path, request.cancellation, ImportPreviewPolicy::kMinimal, true);
    if (!imported)
    {
        best_effort_remove(derived_path);
        return imported.error();
    }
    if (imported.value().status != ImportItemStatus::kImported || !imported.value().asset)
    {
        best_effort_remove(derived_path);
        if (imported.value().error)
            return *imported.value().error;
        return make_error(ErrorCode::kConflict, "Editor output was not imported as a new asset",
                          {{"path", derived_path},
                           {"status", std::to_string(static_cast<int>(imported.value().status))},
                           {"reason", "editor_output_not_imported"}});
    }

    auto recovery = repository_->recovery_state(request.source_asset_id);
    if (!recovery)
    {
        (void)remove_from_catalog(imported.value().asset->id);
        best_effort_remove(derived_path);
        return recovery.error();
    }

    ExternalEditorProvenance provenance;
    provenance.derived_asset_id = imported.value().asset->id;
    provenance.source_asset_id = request.source_asset_id;
    provenance.observed_catalog_revision = snapshot.value().revision;
    provenance.source_recovery_generation = recovery.value().generation;
    provenance.source_original = source_fp.value();
    provenance.derived = std::move(output_fp).value();
    // Re-fingerprint the durable derived copy (may differ in mtime from source output).
    auto derived_fp = fingerprint_file(derived_path);
    if (!derived_fp)
        return derived_fp.error();
    provenance.derived = std::move(derived_fp).value();
    provenance.editor_id = request.editor_id;
    provenance.editor_version = request.editor_version;
    provenance.registered_unix_ms = now_unix_ms();
    provenance.derived_path = derived_path;
    provenance.source_original_path = source_location.value().path;

    cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto written = write_provenance(snapshot.value().database_path, provenance);
    if (!written)
    {
        (void)remove_from_catalog(provenance.derived_asset_id);
        best_effort_remove(derived_path);
        return written.error();
    }

    auto after_source = fingerprint_file(source_location.value().path);
    if (!after_source)
        return after_source.error();
    if (after_source.value().sha256 != provenance.source_original.sha256 ||
        after_source.value().size_bytes != provenance.source_original.size_bytes)
    {
        auto error = make_error(ErrorCode::kConflict,
                                "Source original changed during external-editor register",
                                {{"asset_id", request.source_asset_id},
                                 {"path", source_location.value().path},
                                 {"derived_asset_id", provenance.derived_asset_id},
                                 {"catalog_committed", "true"},
                                 {"reason", "source_mutated_during_register"}});
        return error;
    }

    ExternalEditorRegisterResult result;
    result.derived_asset = std::move(*imported.value().asset);
    result.provenance = std::move(provenance);
    result.source_original_unchanged = true;

    if (request.auto_stack)
    {
        // Re-read source for current stack membership after import revision bump.
        auto source_after = repository_->find_asset_by_id(request.source_asset_id);
        if (!source_after)
            return source_after.error();
        if (!source_after.value())
        {
            return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                              {{"asset_id", request.source_asset_id},
                               {"derived_asset_id", result.derived_asset.id},
                               {"reason", "editor_auto_stack_conflict"}});
        }
        if (source_after.value()->stack_id || result.derived_asset.stack_id)
        {
            return make_error(
                ErrorCode::kConflict,
                "External-editor auto-stack conflict: an asset already belongs to a stack",
                {{"asset_id", request.source_asset_id},
                 {"derived_asset_id", result.derived_asset.id},
                 {"reason", "editor_auto_stack_conflict"},
                 {"detail", "asset_already_stacked"}});
        }

        std::vector<std::string> members{request.source_asset_id, result.derived_asset.id};
        auto stacked =
            stack_assets(members, result.derived_asset.id, /*expected_revision=*/std::nullopt);
        if (!stacked)
        {
            const auto &stack_error = stacked.error();
            const auto detail = stack_error.context.count("reason") != 0 ?
                                    stack_error.context.at("reason") :
                                    stack_error.message;
            return make_error(ErrorCode::kConflict,
                              "External-editor auto-stack failed after derived publication",
                              {{"asset_id", request.source_asset_id},
                               {"derived_asset_id", result.derived_asset.id},
                               {"reason", "editor_auto_stack_conflict"},
                               {"detail", detail}});
        }
        result.auto_stacked = true;
        result.stack = std::move(stacked).value();
        // Refresh derived asset row so stack fields are current for callers.
        auto refreshed = repository_->find_asset_by_id(result.derived_asset.id);
        if (refreshed && refreshed.value())
            result.derived_asset = std::move(*refreshed.value());
    }

    return result;
}

Result<ExternalEditorProvenance>
CatalogService::external_editor_provenance(const std::string_view derived_asset_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (derived_asset_id.empty() || !safe_path_component(derived_asset_id))
    {
        return make_error(
            ErrorCode::kInvalidArgument, "Derived asset id is invalid",
            {{"asset_id", std::string(derived_asset_id)}, {"reason", "invalid_derived_asset_id"}});
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto asset = repository_->find_asset_by_id(derived_asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Catalog asset was not found",
                          {{"asset_id", std::string(derived_asset_id)}});
    }
    return load_provenance(snapshot.value().database_path, derived_asset_id);
}

Result<ExternalEditorOpenResult>
CatalogService::prepare_external_editor_open(const ExternalEditorOpenRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "External-editor open requires explicit user initiation",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.asset_id.empty() || !safe_path_component(request.asset_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Asset id is invalid",
                          {{"asset_id", request.asset_id}, {"reason", "invalid_asset_id"}});
    }
    if (request.editor_id && !safe_path_component(*request.editor_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "External-editor id is invalid",
                          {{"editor_id", *request.editor_id}, {"reason", "invalid_editor_id"}});
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

    std::error_code status_error;
    if (!std::filesystem::is_regular_file(utf8_path(location.value().path), status_error))
    {
        return make_error(ErrorCode::kNotFound, "Asset file was not found for external-editor open",
                          {{"asset_id", request.asset_id},
                           {"path", location.value().path},
                           {"reason", "open_path_missing"},
                           {"detail", status_error ? status_error.message() : ""}});
    }

    ExternalEditorOpenKind open_kind = ExternalEditorOpenKind::kOriginal;
    std::string open_path = location.value().path;
    std::string source_asset_id = request.asset_id;

    auto provenance = load_provenance(snapshot.value().database_path, request.asset_id);
    if (provenance)
    {
        open_kind = ExternalEditorOpenKind::kDerivedWorkingCopy;
        source_asset_id = provenance.value().source_asset_id;
        if (!provenance.value().derived_path.empty())
        {
            std::error_code derived_error;
            if (std::filesystem::is_regular_file(utf8_path(provenance.value().derived_path),
                                                 derived_error))
            {
                open_path = provenance.value().derived_path;
            }
        }
    }
    else if (provenance.error().code != ErrorCode::kNotFound)
    {
        return provenance.error();
    }
    else if (path_under_derived_store(snapshot.value().database_path, open_path))
    {
        open_kind = ExternalEditorOpenKind::kDerivedWorkingCopy;
    }

    cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto open_location = normalize_local_input(open_path);
    if (!open_location)
        return open_location.error();

    ExternalEditorOpenIntent intent;
    intent.intent_id = generate_external_editor_open_intent_id();
    intent.asset_id = request.asset_id;
    intent.source_asset_id = std::move(source_asset_id);
    intent.open_path = open_location.value().path;
    intent.open_uri = open_location.value().uri;
    intent.open_kind = open_kind;
    intent.editor_id = request.editor_id;
    intent.recorded_unix_ms = now_unix_ms();
    intent.observed_catalog_revision = snapshot.value().revision;

    auto written = write_open_intent(snapshot.value().database_path, intent);
    if (!written)
        return written.error();

    ExternalEditorOpenResult result;
    result.intent = std::move(intent);
    return result;
}

} // namespace ravo
