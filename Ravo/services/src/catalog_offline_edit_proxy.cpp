#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
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

[[nodiscard]] Result<std::uint64_t> parse_manifest_u64(const std::string_view text,
                                                       const std::string_view path,
                                                       const std::string_view field)
{
    if (text.empty() || text.size() > 20U)
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest number out of range",
                          {{"path", std::string(path)},
                           {"field", std::string(field)},
                           {"reason", "invalid_offline_edit_proxy_manifest_number"}});
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest number is malformed",
                          {{"path", std::string(path)},
                           {"field", std::string(field)},
                           {"reason", "invalid_offline_edit_proxy_manifest_number"}});
    }
    return value;
}

[[nodiscard]] Result<std::int64_t> parse_manifest_i64(const std::string_view text,
                                                      const std::string_view path,
                                                      const std::string_view field)
{
    if (text.empty() || text.size() > 20U)
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest number out of range",
                          {{"path", std::string(path)},
                           {"field", std::string(field)},
                           {"reason", "invalid_offline_edit_proxy_manifest_number"}});
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest number is malformed",
                          {{"path", std::string(path)},
                           {"field", std::string(field)},
                           {"reason", "invalid_offline_edit_proxy_manifest_number"}});
    }
    return value;
}

[[nodiscard]] bool is_sha256_hex(const std::string_view value) noexcept
{
    if (value.size() != 64U)
        return false;
    for (const char raw : value)
    {
        const auto ch = static_cast<unsigned char>(raw);
        const bool hex =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hex)
            return false;
    }
    return true;
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
        {"pixel_provenance", manifest.pixel_provenance},
        {"pinned", manifest.pinned},
    };
    const auto path = offline_edit_proxy_manifest_path(root.generic_string());
    return write_utf8_text_file_replace_atomically(path,
                                                   serialize_json(JsonValue{std::move(object)}));
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
        return parse_manifest_u64(value->number_if()->text, path, field);
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
        return parse_manifest_i64(value->number_if()->text, path, field);
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
    if (manifest.schema != kOfflineEditProxyContractVersion)
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest schema mismatch",
                          {{"path", path},
                           {"schema", manifest.schema},
                           {"reason", "invalid_offline_edit_proxy_schema"}});
    }
    if (manifest.schema_version != kOfflineEditProxySchemaVersion)
    {
        return make_error(ErrorCode::kValidation,
                          "Offline-edit proxy manifest schema_version unsupported",
                          {{"path", path},
                           {"schema_version", std::to_string(manifest.schema_version)},
                           {"reason", "invalid_offline_edit_proxy_schema_version"}});
    }
    if (!safe_path_component(manifest.asset_id))
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest asset_id is unsafe",
                          {{"path", path},
                           {"asset_id", manifest.asset_id},
                           {"reason", "invalid_offline_edit_proxy_asset_id"}});
    }
    if (!is_sha256_hex(manifest.source_sha256))
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy source hash is malformed",
                          {{"path", path}, {"reason", "invalid_offline_edit_proxy_source_hash"}});
    }
    if (!is_sha256_hex(manifest.proxy_sha256))
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy hash is malformed",
                          {{"path", path}, {"reason", "invalid_offline_edit_proxy_hash"}});
    }
    if (manifest.max_edge == 0U || manifest.profile != "srgb" || manifest.width == 0U ||
        manifest.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Offline-edit proxy manifest fields out of range",
                          {{"path", path}, {"reason", "invalid_offline_edit_proxy_manifest"}});
    }
    {
        std::error_code path_error;
        const auto proxy = utf8_path(manifest.proxy_path).lexically_normal().generic_string();
        const auto support = utf8_path(root).lexically_normal().generic_string();
        if (!(proxy == support ||
              (proxy.size() > support.size() && proxy.compare(0, support.size(), support) == 0 &&
               proxy[support.size()] == '/')))
        {
            return make_error(ErrorCode::kValidation,
                              "Offline-edit proxy path escapes support root",
                              {{"path", manifest.proxy_path},
                               {"root", std::string(root)},
                               {"reason", "offline_edit_proxy_path_escape"}});
        }
    }
    const auto *provenance = parsed.value().find("pixel_provenance");
    if (provenance != nullptr && provenance->string_if() != nullptr)
        manifest.pixel_provenance = *provenance->string_if();
    else
        manifest.pixel_provenance = std::string(kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8);
    if (manifest.pixel_provenance != kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8)
    {
        return make_error(ErrorCode::kValidation,
                          "Offline-edit proxy pixel_provenance is unsupported",
                          {{"path", path},
                           {"pixel_provenance", manifest.pixel_provenance},
                           {"reason", "invalid_offline_edit_proxy_pixel_provenance"}});
    }
    const auto *pinned = parsed.value().find("pinned");
    if (pinned != nullptr)
    {
        if (pinned->boolean_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Offline-edit proxy pinned flag is invalid",
                              {{"path", path}, {"reason", "invalid_offline_edit_proxy_pinned"}});
        }
        manifest.pinned = *pinned->boolean_if();
    }
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

[[nodiscard]] Result<void> publish_proxy_tree_atomically(const std::string_view final_root,
                                                         const std::string_view staging_root)
{
    std::error_code error;
    const auto final_path = utf8_path(final_root);
    const auto staging_path = utf8_path(staging_root);
    const auto backup_path = utf8_path(std::string(final_root) + ".prev");
    const bool had_previous = std::filesystem::exists(final_path, error) && !error;
    if (had_previous)
    {
        std::filesystem::remove_all(backup_path, error);
        std::filesystem::rename(final_path, backup_path, error);
        if (error)
        {
            return make_error(ErrorCode::kIo, "Unable to quarantine previous offline-edit proxy",
                              {{"path", std::string(final_root)},
                               {"reason", "offline_edit_proxy_publish_quarantine_failed"},
                               {"detail", error.message()}});
        }
    }
    std::filesystem::rename(staging_path, final_path, error);
    if (error)
    {
        if (had_previous)
        {
            std::error_code restore_error;
            std::filesystem::rename(backup_path, final_path, restore_error);
        }
        return make_error(ErrorCode::kIo, "Unable to publish offline-edit proxy staging tree",
                          {{"path", std::string(final_root)},
                           {"staging", std::string(staging_root)},
                           {"reason", "offline_edit_proxy_publish_failed"},
                           {"detail", error.message()}});
    }
    if (had_previous)
        std::filesystem::remove_all(backup_path, error);
    return {};
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

    const auto final_root =
        offline_edit_proxy_root(snapshot.value().database_path, request.asset_id);
    const auto staging_root = final_root + ".staging-" + std::to_string(now_unix_ms());
    best_effort_remove_tree(staging_root);
    auto created_dir = ensure_directory(staging_root, "offline_edit_proxy_create_failed");
    if (!created_dir)
        return created_dir.error();

    auto recipe_key = recipe_cache_key_for(*this, request.asset_id);
    if (!recipe_key)
    {
        best_effort_remove_tree(staging_root);
        return recipe_key.error();
    }

    const auto proxy_path = offline_edit_proxy_raster_path(staging_root);
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
        best_effort_remove_tree(staging_root);
        return exported.error();
    }

    auto proxy_fp = fingerprint_file(proxy_path);
    if (!proxy_fp)
    {
        best_effort_remove_tree(staging_root);
        return proxy_fp.error();
    }

    auto after = fingerprint_file(original_path.value());
    if (!after)
    {
        best_effort_remove_tree(staging_root);
        return after.error();
    }
    if (after.value().sha256 != before.value().sha256 ||
        after.value().size_bytes != before.value().size_bytes ||
        after.value().mtime_unix_ms != before.value().mtime_unix_ms)
    {
        best_effort_remove_tree(staging_root);
        return make_error(ErrorCode::kConflict,
                          "Source original changed during offline-edit proxy create",
                          {{"asset_id", request.asset_id},
                           {"path", original_path.value()},
                           {"reason", "source_mutated_during_proxy_create"}});
    }

    const auto published_proxy_path = offline_edit_proxy_raster_path(final_root);
    OfflineEditProxyManifest manifest;
    manifest.asset_id = request.asset_id;
    manifest.source_sha256 = before.value().sha256;
    manifest.source_size_bytes = before.value().size_bytes;
    manifest.source_mtime_unix_ms = before.value().mtime_unix_ms;
    manifest.recipe_cache_key = std::move(recipe_key).value();
    manifest.max_edge = request.max_edge;
    manifest.profile = request.profile;
    manifest.proxy_path = published_proxy_path;
    manifest.proxy_sha256 = proxy_fp.value().sha256;
    manifest.width = exported.value().width;
    manifest.height = exported.value().height;
    manifest.created_unix_ms = now_unix_ms();
    manifest.pixel_provenance = std::string(kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8);

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
        {"pixel_provenance", manifest.pixel_provenance},
        {"pinned", manifest.pinned},
    };
    auto written = write_utf8_text_file_atomically(offline_edit_proxy_manifest_path(staging_root),
                                                   serialize_json(JsonValue{std::move(object)}));
    if (!written)
    {
        best_effort_remove_tree(staging_root);
        return written.error();
    }

    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        best_effort_remove_tree(staging_root);
        auto error = cancelled.error();
        error.context.insert_or_assign("reason", "offline_edit_proxy_cancelled_before_publish");
        return error;
    }

    if (testing_before_offline_proxy_publish_)
    {
        auto inject = testing_before_offline_proxy_publish_(final_root, staging_root);
        if (!inject)
        {
            best_effort_remove_tree(staging_root);
            return inject.error();
        }
    }
    auto published = publish_proxy_tree_atomically(final_root, staging_root);
    if (!published)
    {
        best_effort_remove_tree(staging_root);
        return published.error();
    }

    OfflineEditProxyCreateResult result;
    result.manifest = std::move(manifest);
    result.originals_unchanged = true;
    return result;
}

Result<OfflineEditProxyListReport> CatalogService::list_offline_edit_proxies() const
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
    OfflineEditProxyListReport report;
    std::error_code error;
    if (!std::filesystem::is_directory(utf8_path(root), error) || error)
        return report;

    for (const auto &entry : std::filesystem::directory_iterator(utf8_path(root), error))
    {
        if (error)
            break;
        if (!entry.is_directory(error) || error)
            continue;
        const auto asset_id = entry.path().filename().generic_string();
        if (asset_id.find(".staging-") != std::string::npos ||
            (asset_id.size() >= 5U && asset_id.compare(asset_id.size() - 5U, 5U, ".prev") == 0))
        {
            continue;
        }
        if (!safe_path_component(asset_id))
        {
            OfflineEditProxyCorruptEntry corrupt;
            corrupt.asset_id = asset_id;
            corrupt.path = entry.path().generic_string();
            corrupt.reason = "unsafe_proxy_directory_name";
            report.corrupt.push_back(std::move(corrupt));
            continue;
        }
        auto loaded = load_manifest(entry.path().generic_string());
        if (!loaded)
        {
            OfflineEditProxyCorruptEntry corrupt;
            corrupt.asset_id = asset_id;
            corrupt.path = offline_edit_proxy_manifest_path(entry.path().generic_string());
            corrupt.reason = loaded.error().context.count("reason") ?
                                 loaded.error().context.at("reason") :
                                 "invalid_offline_edit_proxy_manifest";
            report.corrupt.push_back(std::move(corrupt));
            continue;
        }
        if (loaded.value().asset_id != asset_id)
        {
            OfflineEditProxyCorruptEntry corrupt;
            corrupt.asset_id = asset_id;
            corrupt.path = offline_edit_proxy_manifest_path(entry.path().generic_string());
            corrupt.reason = "invalid_offline_edit_proxy_asset_id";
            report.corrupt.push_back(std::move(corrupt));
            continue;
        }
        report.manifests.push_back(std::move(loaded).value());
    }
    return report;
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

    auto original_path = original_path_for_asset(*source.value());
    const bool original_present = original_path && file_is_regular(original_path.value());
    // COR-01: file presence alone must not mark export-usable. Require catalog
    // identity (size/mtime/fingerprint) to match the on-disk original, or an
    // explicit reconnect verification path.
    bool original_identity_verified = false;
    if (original_present)
    {
        auto identity = read_file_identity(original_path.value());
        if (identity)
        {
            const bool size_mtime_match =
                identity.value().size_bytes == source.value()->size_bytes &&
                identity.value().mtime_unix_ms == source.value()->mtime_unix_ms;
            if (size_mtime_match)
            {
                if (!source.value()->content_fingerprint)
                {
                    original_identity_verified = true;
                }
                else
                {
                    original_identity_verified = make_content_fingerprint(identity.value()) ==
                                                 *source.value()->content_fingerprint;
                }
            }
        }
    }
    status.usable_for_export = original_identity_verified;

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
        else if (original_identity_verified)
            status.reason = "original_present_with_proxy";
        else if (original_present)
            status.reason = "original_identity_unverified";
        else
            status.reason = "proxy_ready";
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

    if (original_present && !original_identity_verified && status.reason == "proxy_absent")
        status.reason = "original_identity_unverified";

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
        // Without a proxy manifest, still require catalog identity match before
        // marking export-usable (COR-01).
        const bool identity_ok = current.value().size_bytes == source.value()->size_bytes &&
                                 current.value().mtime_unix_ms == source.value()->mtime_unix_ms &&
                                 (!source.value()->content_fingerprint ||
                                  make_content_fingerprint(FileIdentity{
                                      current.value().size_bytes, current.value().mtime_unix_ms}) ==
                                      *source.value()->content_fingerprint);
        if (!identity_ok)
        {
            return make_error(ErrorCode::kConflict,
                              "Restored original does not match catalog identity",
                              {{"asset_id", request.asset_id},
                               {"path", original_path.value()},
                               {"reason", "source_identity_mismatch"}});
        }
        result.source_hash_matched = false;
        result.status = std::move(status).value();
        result.status.media_state = OfflineEditMediaState::kOriginal;
        result.status.reason = "reconnect_without_proxy_manifest";
        result.status.usable_for_export = true;
        result.offline_states_cleared = true;
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
    refreshed.value().usable_for_export = true;
    // Original is authority again; proxy may remain on disk for reuse but must
    // not keep machine-visible offline/export-blocked signals.
    result.offline_states_cleared = true;
    result.status = std::move(refreshed).value();
    if (request.clear_proxy)
    {
        if (result.status.manifest && result.status.manifest->pinned)
        {
            result.proxy_cleared = false;
            result.status.reason = "reconnect_verified_proxy_pinned";
        }
        else
        {
            OfflineEditProxyDeleteRequest del;
            del.asset_id = request.asset_id;
            del.user_initiated = true;
            del.force = false;
            auto cleared = delete_offline_edit_proxy(del);
            if (!cleared)
                return cleared.error();
            result.proxy_cleared = cleared.value().deleted;
            auto after = verify_offline_edit_proxy(request.asset_id);
            if (!after)
                return after.error();
            after.value().media_state = OfflineEditMediaState::kOriginal;
            after.value().reason =
                result.proxy_cleared ? "reconnect_verified_proxy_cleared" : "reconnect_verified";
            after.value().usable_for_export = true;
            result.status = std::move(after).value();
        }
    }
    return result;
}

Result<OfflineEditProxyDeleteResult>
CatalogService::delete_offline_edit_proxy(const OfflineEditProxyDeleteRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy delete requires --user-initiated",
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

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    const auto root = offline_edit_proxy_root(snapshot.value().database_path, request.asset_id);

    OfflineEditProxyDeleteResult result;
    result.originals_unchanged = true;
    auto loaded = load_manifest(root);
    if (!loaded)
    {
        if (loaded.error().code == ErrorCode::kNotFound || loaded.error().code == ErrorCode::kIo)
        {
            result.deleted = false;
            result.reason = "proxy_absent";
            return result;
        }
        return loaded.error();
    }
    if (loaded.value().pinned && !request.force)
    {
        return make_error(ErrorCode::kConflict, "Offline-edit proxy is pinned",
                          {{"asset_id", request.asset_id}, {"reason", "proxy_pinned"}});
    }
    std::error_code error;
    std::filesystem::remove_all(utf8_path(root), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to delete offline-edit proxy",
                          {{"path", root},
                           {"asset_id", request.asset_id},
                           {"reason", "offline_edit_proxy_delete_failed"},
                           {"detail", error.message()}});
    }
    result.deleted = true;
    result.reason = loaded.value().pinned ? "proxy_force_deleted" : "proxy_deleted";
    return result;
}

Result<OfflineEditProxyPinResult>
CatalogService::pin_offline_edit_proxy(const OfflineEditProxyPinRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy pin requires --user-initiated",
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
    if (!status.value().proxy_verified || !status.value().manifest)
    {
        return make_error(
            ErrorCode::kNotFound, "Offline-edit proxy is not verified",
            {{"asset_id", request.asset_id},
             {"reason", status.value().reason.empty() ? "proxy_absent" : status.value().reason}});
    }
    OfflineEditProxyManifest manifest = *status.value().manifest;
    if (manifest.pinned == request.pinned)
    {
        OfflineEditProxyPinResult result;
        result.manifest = std::move(manifest);
        return result;
    }
    manifest.pinned = request.pinned;
    auto written = write_manifest(manifest);
    if (!written)
        return written.error();
    OfflineEditProxyPinResult result;
    result.manifest = std::move(manifest);
    return result;
}

Result<OfflineEditProxyEvictResult>
CatalogService::evict_offline_edit_proxies(const OfflineEditProxyEvictRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy evict requires --user-initiated",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.max_total_bytes == 0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Offline-edit proxy evict requires max_total_bytes > 0",
                          {{"reason", "missing_max_total_bytes"}});
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto listed = list_offline_edit_proxies();
    if (!listed)
        return listed.error();

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();

    struct Candidate
    {
        std::string asset_id;
        std::uint64_t bytes = 0;
        std::int64_t created_unix_ms = 0;
        bool pinned = false;
        bool corrupt = false;
    };
    std::vector<Candidate> candidates;
    std::uint64_t total_bytes = 0;
    const auto add_bytes = [&](const std::string &root) -> std::uint64_t
    {
        std::uint64_t bytes = 0;
        std::error_code error;
        if (!std::filesystem::exists(utf8_path(root), error) || error)
            return 0;
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(utf8_path(root), error))
        {
            if (error)
                break;
            if (!entry.is_regular_file(error) || error)
                continue;
            bytes += static_cast<std::uint64_t>(entry.file_size(error));
            if (error)
                error.clear();
        }
        return bytes;
    };

    for (const auto &manifest : listed.value().manifests)
    {
        Candidate row;
        row.asset_id = manifest.asset_id;
        row.created_unix_ms = manifest.created_unix_ms;
        row.pinned = manifest.pinned;
        const auto root =
            offline_edit_proxy_root(snapshot.value().database_path, manifest.asset_id);
        row.bytes = add_bytes(root);
        total_bytes += row.bytes;
        candidates.push_back(std::move(row));
    }
    for (const auto &corrupt : listed.value().corrupt)
    {
        Candidate row;
        row.asset_id = corrupt.asset_id;
        row.corrupt = true;
        if (safe_path_component(corrupt.asset_id))
        {
            const auto root =
                offline_edit_proxy_root(snapshot.value().database_path, corrupt.asset_id);
            row.bytes = add_bytes(root);
            total_bytes += row.bytes;
        }
        candidates.push_back(std::move(row));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right)
              {
                  if (left.pinned != right.pinned)
                      return !left.pinned && right.pinned;
                  if (left.corrupt != right.corrupt)
                      return left.corrupt && !right.corrupt;
                  return left.created_unix_ms < right.created_unix_ms;
              });

    OfflineEditProxyEvictResult result;
    for (const auto &row : candidates)
    {
        if (row.pinned)
        {
            ++result.retained_pinned;
            result.retained_pinned_asset_ids.push_back(row.asset_id);
            result.bytes_retained += row.bytes;
            continue;
        }
        if (total_bytes <= request.max_total_bytes)
        {
            result.bytes_retained += row.bytes;
            continue;
        }
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();
        OfflineEditProxyDeleteRequest del;
        del.asset_id = row.asset_id;
        del.user_initiated = true;
        del.force = row.corrupt;
        auto deleted = delete_offline_edit_proxy(del);
        if (!deleted)
            return deleted.error();
        if (deleted.value().deleted)
        {
            ++result.evicted;
            result.evicted_asset_ids.push_back(row.asset_id);
            if (total_bytes >= row.bytes)
                total_bytes -= row.bytes;
            else
                total_bytes = 0;
        }
        else
        {
            result.bytes_retained += row.bytes;
        }
    }
    return result;
}

} // namespace ravo
