#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/offline_edit_proxy.h"

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

[[nodiscard]] std::string offline_edit_proxy_root(const std::string_view database_path,
                                                  const std::string_view asset_id)
{
    return std::string(database_path) + ".ravo/offline-edit-proxies/" + std::string(asset_id);
}

[[nodiscard]] std::string offline_edit_proxy_manifest_path(const std::string_view root)
{
    return std::string(root) + "/manifest.json";
}

[[nodiscard]] std::string offline_edit_proxy_raster_path(const std::string_view root)
{
    return std::string(root) + "/proxy.tif";
}

struct FileFingerprint
{
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

[[nodiscard]] Result<std::string> original_path_for_asset(const AssetRecord &asset)
{
    auto location = normalize_local_input(asset.normalized_uri);
    if (!location)
        return location.error();
    return location.value().path;
}

[[nodiscard]] Result<FileFingerprint> fingerprint_file(const std::string_view path)
{
    auto digest = sha256_file_hex(path);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
    FileFingerprint fingerprint;
    fingerprint.sha256 = std::move(digest).value();
    fingerprint.size_bytes = identity.value().size_bytes;
    fingerprint.mtime_unix_ms = identity.value().mtime_unix_ms;
    return fingerprint;
}

[[nodiscard]] Result<void> ensure_directory(const std::string_view path_utf8,
                                            const std::string_view reason)
{
    std::error_code error;
    std::filesystem::create_directories(utf8_path(path_utf8), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create offline-edit proxy directory",
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

[[nodiscard]] JsonValue fingerprint_fields(const FileFingerprint &value)
{
    return JsonValue{JsonValue::Object{
        {"sha256", value.sha256},
        {"size_bytes", JsonValue::number(std::to_string(value.size_bytes))},
        {"mtime_unix_ms", JsonValue::number(std::to_string(value.mtime_unix_ms))},
    }};
}

[[nodiscard]] Result<void> write_manifest(const OfflineEditProxyManifest &manifest)
{
    const auto root = std::filesystem::path(manifest.proxy_path).parent_path();
    auto created = ensure_directory(root.generic_string(), "offline_edit_proxy_create_failed");
    if (!created)
        return created.error();

    JsonValue::Object object{
        {"schema", manifest.schema},
        {"schema_version", JsonValue::number(std::to_string(manifest.schema_version))},
        {"asset_id", manifest.asset_id},
        {"source_sha256", manifest.source_sha256},
        {"source_size_bytes", JsonValue::number(std::to_string(manifest.source_size_bytes))},
        {"source_mtime_unix_ms", JsonValue::number(std::to_string(manifest.source_mtime_unix_ms))},
        {"recipe_cache_key", manifest.recipe_cache_key},
        {"max_edge", JsonValue::number(std::to_string(manifest.max_edge))},
        {"profile", manifest.profile},
        {"proxy_path", manifest.proxy_path},
        {"proxy_sha256", manifest.proxy_sha256},
        {"width", JsonValue::number(std::to_string(manifest.width))},
        {"height", JsonValue::number(std::to_string(manifest.height))},
        {"created_unix_ms", JsonValue::number(std::to_string(manifest.created_unix_ms))},
    };
    const auto path = offline_edit_proxy_manifest_path(root.generic_string());
    return write_utf8_text_file_atomically(path, serialize_json(JsonValue{std::move(object)}));
}

[[nodiscard]] Result<OfflineEditProxyManifest> load_manifest(const std::string_view root)
{
    const auto path = offline_edit_proxy_manifest_path(root);
    auto text = read_utf8_text_file(path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest is not an object",
                          {{"path", path}, {"reason", "invalid_offline_edit_proxy_manifest"}});
    }
    const auto require_string = [&](const char *field) -> Result<std::string>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->string_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_offline_edit_proxy_manifest"}});
        }
        return *value->string_if();
    };
    const auto require_u64 = [&](const char *field) -> Result<std::uint64_t>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->number_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_offline_edit_proxy_manifest"}});
        }
        return static_cast<std::uint64_t>(std::stoull(value->number_if()->text));
    };
    const auto require_i64 = [&](const char *field) -> Result<std::int64_t>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr || value->number_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest field missing",
                              {{"path", path},
                               {"field", field},
                               {"reason", "invalid_offline_edit_proxy_manifest"}});
        }
        return std::stoll(value->number_if()->text);
    };

    OfflineEditProxyManifest manifest;
    auto schema = require_string("schema");
    if (!schema)
        return schema.error();
    manifest.schema = std::move(schema).value();
    auto schema_version = require_i64("schema_version");
    if (!schema_version)
        return schema_version.error();
    manifest.schema_version = schema_version.value();
    auto asset_id = require_string("asset_id");
    if (!asset_id)
        return asset_id.error();
    manifest.asset_id = std::move(asset_id).value();
    auto source_sha = require_string("source_sha256");
    if (!source_sha)
        return source_sha.error();
    manifest.source_sha256 = std::move(source_sha).value();
    auto source_size = require_u64("source_size_bytes");
    if (!source_size)
        return source_size.error();
    manifest.source_size_bytes = source_size.value();
    auto source_mtime = require_i64("source_mtime_unix_ms");
    if (!source_mtime)
        return source_mtime.error();
    manifest.source_mtime_unix_ms = source_mtime.value();
    auto recipe_key = require_string("recipe_cache_key");
    if (!recipe_key)
        return recipe_key.error();
    manifest.recipe_cache_key = std::move(recipe_key).value();
    auto max_edge = require_u64("max_edge");
    if (!max_edge)
        return max_edge.error();
    manifest.max_edge = static_cast<std::uint32_t>(max_edge.value());
    auto profile = require_string("profile");
    if (!profile)
        return profile.error();
    manifest.profile = std::move(profile).value();
    auto proxy_path = require_string("proxy_path");
    if (!proxy_path)
        return proxy_path.error();
    manifest.proxy_path = std::move(proxy_path).value();
    auto proxy_sha = require_string("proxy_sha256");
    if (!proxy_sha)
        return proxy_sha.error();
    manifest.proxy_sha256 = std::move(proxy_sha).value();
    auto width = require_u64("width");
    if (!width)
        return width.error();
    manifest.width = static_cast<std::uint32_t>(width.value());
    auto height = require_u64("height");
    if (!height)
        return height.error();
    manifest.height = static_cast<std::uint32_t>(height.value());
    auto created = require_i64("created_unix_ms");
    if (!created)
        return created.error();
    manifest.created_unix_ms = created.value();
    (void)fingerprint_fields;
    return manifest;
}

[[nodiscard]] Result<std::string> recipe_cache_key_for(CatalogService &service,
                                                       const std::string_view asset_id)
{
    auto recipe = service.load_recipe(asset_id);
    if (!recipe)
        return recipe.error();
    auto serialized = serialize_recipe(recipe.value());
    if (!serialized)
        return serialized.error();
    return sha256_utf8_hex(serialized.value());
}

[[nodiscard]] bool file_is_regular(const std::string_view path) noexcept
{
    std::error_code error;
    return std::filesystem::is_regular_file(utf8_path(path), error) && !error;
}

} // namespace

Result<OfflineEditProxyCreateResult>
CatalogService::create_offline_edit_proxy(const OfflineEditProxyCreateRequest &request)
{
    if (repository_ == nullptr || engine_ == nullptr || raster_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy create requires --user-initiated",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.asset_id.empty() || !safe_path_component(request.asset_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Offline-edit proxy requires an asset id",
                          {{"reason", "missing_asset_id"}, {"asset_id", request.asset_id}});
    }
    if (request.max_edge == 0U)
    {
        return make_error(ErrorCode::kInvalidArgument, "Offline-edit proxy max edge must be > 0",
                          {{"reason", "invalid_max_edge"}});
    }
    if (request.profile.empty() || request.profile != "srgb")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy v1 only supports profile=srgb",
                          {{"reason", "unsupported_proxy_profile"}, {"profile", request.profile}});
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto source = repository_->find_asset_by_id(request.asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", request.asset_id}, {"reason", "asset_not_found"}});
    }

    auto original_path = original_path_for_asset(*source.value());
    if (!original_path)
        return original_path.error();
    if (!file_is_regular(original_path.value()))
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing; cannot create proxy",
                          {{"asset_id", request.asset_id},
                           {"path", original_path.value()},
                           {"reason", "original_missing"}});
    }

    auto before = fingerprint_file(original_path.value());
    if (!before)
        return before.error();

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();

    const auto root = offline_edit_proxy_root(snapshot.value().database_path, request.asset_id);
    best_effort_remove_tree(root);
    auto created_dir = ensure_directory(root, "offline_edit_proxy_create_failed");
    if (!created_dir)
        return created_dir.error();

    auto recipe_key = recipe_cache_key_for(*this, request.asset_id);
    if (!recipe_key)
    {
        best_effort_remove_tree(root);
        return recipe_key.error();
    }

    const auto proxy_path = offline_edit_proxy_raster_path(root);
    ExportRequest export_request;
    export_request.asset_id = request.asset_id;
    export_request.output_path = proxy_path;
    export_request.format = ExportFormat::kTiff;
    export_request.max_edge = request.max_edge;
    export_request.tiff_options.sample_type = TiffSampleType::kUint8;
    export_request.tiff_options.compression = TiffCompression::kDeflatePredictor;
    export_request.metadata_mode = ExportMetadataMode::kNone;
    export_request.output_color.enabled = true;
    export_request.output_color.output_profile = request.profile;
    export_request.cancellation = request.cancellation;

    auto exported = export_asset(export_request);
    if (!exported)
    {
        best_effort_remove_tree(root);
        return exported.error();
    }

    auto proxy_fp = fingerprint_file(proxy_path);
    if (!proxy_fp)
    {
        best_effort_remove_tree(root);
        return proxy_fp.error();
    }

    auto after = fingerprint_file(original_path.value());
    if (!after)
    {
        best_effort_remove_tree(root);
        return after.error();
    }
    if (after.value().sha256 != before.value().sha256 ||
        after.value().size_bytes != before.value().size_bytes ||
        after.value().mtime_unix_ms != before.value().mtime_unix_ms)
    {
        best_effort_remove_tree(root);
        return make_error(ErrorCode::kConflict,
                          "Source original changed during offline-edit proxy create",
                          {{"asset_id", request.asset_id},
                           {"path", original_path.value()},
                           {"reason", "source_mutated_during_proxy_create"}});
    }

    OfflineEditProxyManifest manifest;
    manifest.asset_id = request.asset_id;
    manifest.source_sha256 = before.value().sha256;
    manifest.source_size_bytes = before.value().size_bytes;
    manifest.source_mtime_unix_ms = before.value().mtime_unix_ms;
    manifest.recipe_cache_key = std::move(recipe_key).value();
    manifest.max_edge = request.max_edge;
    manifest.profile = request.profile;
    manifest.proxy_path = proxy_path;
    manifest.proxy_sha256 = proxy_fp.value().sha256;
    manifest.width = exported.value().width;
    manifest.height = exported.value().height;
    manifest.created_unix_ms = now_unix_ms();

    auto written = write_manifest(manifest);
    if (!written)
    {
        best_effort_remove_tree(root);
        return written.error();
    }

    OfflineEditProxyCreateResult result;
    result.manifest = std::move(manifest);
    result.originals_unchanged = true;
    return result;
}

Result<std::vector<OfflineEditProxyManifest>> CatalogService::list_offline_edit_proxies() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();

    const auto root = std::string(snapshot.value().database_path) + ".ravo/offline-edit-proxies";
    std::vector<OfflineEditProxyManifest> manifests;
    std::error_code error;
    if (!std::filesystem::is_directory(utf8_path(root), error) || error)
        return manifests;

    for (const auto &entry : std::filesystem::directory_iterator(utf8_path(root), error))
    {
        if (error)
            break;
        if (!entry.is_directory(error) || error)
            continue;
        const auto asset_id = entry.path().filename().generic_string();
        if (!safe_path_component(asset_id))
            continue;
        auto loaded = load_manifest(entry.path().generic_string());
        if (!loaded)
            continue;
        manifests.push_back(std::move(loaded).value());
    }
    return manifests;
}

Result<OfflineEditProxyStatus>
CatalogService::verify_offline_edit_proxy(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (asset_id.empty() || !safe_path_component(asset_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Offline-edit proxy requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }

    auto source = repository_->find_asset_by_id(asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", std::string(asset_id)}, {"reason", "asset_not_found"}});
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();

    OfflineEditProxyStatus status;
    status.asset_id = std::string(asset_id);
    status.usable_for_export = false;

    auto original_path = original_path_for_asset(*source.value());
    const bool original_present = original_path && file_is_regular(original_path.value());

    const auto root = offline_edit_proxy_root(snapshot.value().database_path, asset_id);
    auto loaded = load_manifest(root);
    if (loaded)
    {
        status.proxy_present = true;
        status.manifest = loaded.value();
        const bool raster_ok = file_is_regular(loaded.value().proxy_path);
        auto proxy_fp =
            raster_ok ?
                fingerprint_file(loaded.value().proxy_path) :
                Result<FileFingerprint>{make_error(ErrorCode::kNotFound, "Proxy raster missing",
                                                   {{"reason", "proxy_raster_missing"}})};
        status.proxy_verified =
            raster_ok && proxy_fp && proxy_fp.value().sha256 == loaded.value().proxy_sha256;
        status.usable_for_develop = status.proxy_verified;
        if (!status.proxy_verified)
            status.reason = "proxy_corrupt_or_stale";
        else
            status.reason = original_present ? "original_present_with_proxy" : "proxy_ready";
    }
    else if (loaded.error().code == ErrorCode::kNotFound || loaded.error().code == ErrorCode::kIo)
    {
        status.proxy_present = false;
        status.proxy_verified = false;
        status.usable_for_develop = false;
        status.reason = "proxy_absent";
    }
    else
    {
        return loaded.error();
    }

    if (original_present)
        status.media_state = OfflineEditMediaState::kOriginal;
    else if (status.proxy_verified)
        status.media_state = OfflineEditMediaState::kProxy;
    else if (status.proxy_present)
        status.media_state = OfflineEditMediaState::kPlaceholder;
    else
        status.media_state = OfflineEditMediaState::kMissing;

    return status;
}

Result<OfflineEditProxyStatus>
CatalogService::offline_edit_media_status(const std::string_view asset_id) const
{
    return verify_offline_edit_proxy(asset_id);
}

Result<OfflineEditProxyReconnectResult>
CatalogService::reconnect_offline_edit_proxy(const OfflineEditProxyReconnectRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy reconnect requires --user-initiated",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.asset_id.empty() || !safe_path_component(request.asset_id))
    {
        return make_error(ErrorCode::kInvalidArgument, "Offline-edit proxy requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto status = verify_offline_edit_proxy(request.asset_id);
    if (!status)
        return status.error();

    auto source = repository_->find_asset_by_id(request.asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", request.asset_id}, {"reason", "asset_not_found"}});
    }

    auto original_path = original_path_for_asset(*source.value());
    if (!original_path)
        return original_path.error();
    if (!file_is_regular(original_path.value()))
    {
        return make_error(ErrorCode::kNotFound, "Original file is still missing",
                          {{"asset_id", request.asset_id},
                           {"path", original_path.value()},
                           {"reason", "original_missing"}});
    }

    auto current = fingerprint_file(original_path.value());
    if (!current)
        return current.error();

    OfflineEditProxyReconnectResult result;
    result.originals_unchanged = true;
    if (!status.value().manifest)
    {
        result.source_hash_matched = false;
        result.status = std::move(status).value();
        result.status.media_state = OfflineEditMediaState::kOriginal;
        result.status.reason = "reconnect_without_proxy_manifest";
        return result;
    }

    const auto &manifest = *status.value().manifest;
    result.source_hash_matched = current.value().sha256 == manifest.source_sha256;
    if (!result.source_hash_matched)
    {
        return make_error(ErrorCode::kConflict,
                          "Restored original does not match offline-edit proxy source hash",
                          {{"asset_id", request.asset_id},
                           {"path", original_path.value()},
                           {"expected_sha256", manifest.source_sha256},
                           {"actual_sha256", current.value().sha256},
                           {"reason", "source_hash_mismatch"}});
    }

    // Refresh catalog identity when size/mtime drifted but content hash matched.
    AssetRecord updated = *source.value();
    updated.size_bytes = current.value().size_bytes;
    updated.mtime_unix_ms = current.value().mtime_unix_ms;
    updated.content_fingerprint = make_content_fingerprint(
        FileIdentity{current.value().size_bytes, current.value().mtime_unix_ms});
    if (updated.import_state == kImportStateMissing)
    {
        updated.import_state = std::string(kImportStateImported);
        updated.error_code.reset();
        updated.error_message.reset();
    }
    auto saved = repository_->update_asset(updated);
    if (!saved)
        return saved.error();

    auto refreshed = verify_offline_edit_proxy(request.asset_id);
    if (!refreshed)
        return refreshed.error();
    refreshed.value().media_state = OfflineEditMediaState::kOriginal;
    refreshed.value().reason = "reconnect_verified";
    result.status = std::move(refreshed).value();
    return result;
}

} // namespace ravo
