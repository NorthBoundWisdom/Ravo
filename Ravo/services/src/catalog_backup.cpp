#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atomic_publication_internal.h"
#include "catalog_backup_trees.h"
#include "catalog_internal.h"
#include "ravo/foundation/json.h"
#include "ravo/services/artifact_publication.h"

namespace ravo
{
namespace
{

constexpr std::size_t kBackupMaximumAssets = 1'000'000U;
constexpr std::size_t kManifestReadChunkBytes = 64U * 1024U;

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] TaskError backup_error(const ErrorCode code, std::string message, std::string reason,
                                     std::string path = {}, std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] bool safe_asset_id(const std::string_view value)
{
    if (value.empty() || value.size() > 180U)
        return false;
    return std::all_of(value.begin(), value.end(),
                       [](const char character)
                       {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9') || character == '-' ||
                                  character == '_';
                       });
}

[[nodiscard]] bool valid_sha256(const std::string_view value)
{
    return value.size() == 64U && std::all_of(value.begin(), value.end(),
                                              [](const char character)
                                              {
                                                  return (character >= '0' && character <= '9') ||
                                                         (character >= 'a' && character <= 'f');
                                              });
}

[[nodiscard]] std::string sidecar_filename(const std::string_view asset_id,
                                           const std::int64_t generation)
{
    return std::string(asset_id) + "." + std::to_string(generation) + ".ravo.json";
}

[[nodiscard]] Result<void> expect_exact_keys(const JsonValue::Object &object,
                                             const std::initializer_list<std::string_view> keys,
                                             const std::string_view owner)
{
    std::set<std::string, std::less<>> expected;
    for (const auto key : keys)
        expected.emplace(key);
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (!expected.contains(key))
            return backup_error(ErrorCode::kValidation, "Backup manifest contains an unknown field",
                                "unknown_backup_manifest_field", {},
                                std::string(owner) + "." + key);
    }
    for (const auto &key : expected)
    {
        if (!object.contains(key))
            return backup_error(
                ErrorCode::kValidation, "Backup manifest is missing a required field",
                "missing_backup_manifest_field", {}, std::string(owner) + "." + key);
    }
    return {};
}

[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue *value,
                                                               const std::string_view field)
{
    if (value == nullptr || value->object_if() == nullptr)
        return backup_error(ErrorCode::kValidation, "Backup manifest field must be an object",
                            "backup_manifest_type_mismatch", {}, std::string(field));
    return value->object_if();
}

[[nodiscard]] Result<const JsonValue::Array *> require_array(const JsonValue *value,
                                                             const std::string_view field)
{
    if (value == nullptr || value->array_if() == nullptr)
        return backup_error(ErrorCode::kValidation, "Backup manifest field must be an array",
                            "backup_manifest_type_mismatch", {}, std::string(field));
    return value->array_if();
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object,
                                                 const std::string_view key,
                                                 const std::size_t maximum_bytes)
{
    const auto found = object.find(key);
    const auto *value = found == object.end() ? nullptr : found->second.string_if();
    if (value == nullptr || value->empty() || value->size() > maximum_bytes)
        return backup_error(ErrorCode::kValidation, "Backup manifest string is invalid",
                            "invalid_backup_manifest_string", {}, std::string(key));
    return *value;
}

template <typename Integer>
[[nodiscard]] Result<Integer> require_integer(const JsonValue::Object &object,
                                              const std::string_view key, const Integer minimum,
                                              const Integer maximum)
{
    const auto found = object.find(key);
    const auto *number = found == object.end() ? nullptr : found->second.number_if();
    if (number == nullptr)
        return backup_error(ErrorCode::kValidation, "Backup manifest integer is missing",
                            "backup_manifest_type_mismatch", {}, std::string(key));
    Integer value{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        value < minimum || value > maximum)
        return backup_error(ErrorCode::kValidation,
                            "Backup manifest integer is outside its supported bounds",
                            "invalid_backup_manifest_integer", {}, std::string(key));
    return value;
}

struct ParsedBackupManifest
{
    std::int64_t format_version = kCatalogBackupFormatVersion;
    std::int64_t created_unix_ms = 0;
    CatalogDatabaseArtifact catalog;
    std::vector<RecoveryArtifact> sidecars;
    std::vector<CatalogBackupTreeFile> derived;
    std::vector<CatalogBackupTreeFile> external_editor;
    std::vector<CatalogBackupTreeFile> dng_conversion;
    std::vector<CatalogBackupTreeFile> smart_previews;
};

[[nodiscard]] Result<std::string> read_backup_manifest(const std::filesystem::path &path,
                                                       const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status))
        return backup_error(ErrorCode::kValidation, "Backup manifest is not a regular file",
                            "backup_manifest_not_regular", path_utf8(path), status_error.message());
    const auto bytes = std::filesystem::file_size(path, status_error);
    if (status_error || bytes == 0U || bytes > kCatalogBackupManifestMaximumBytes ||
        bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
        return backup_error(ErrorCode::kValidation, "Backup manifest size is invalid",
                            "invalid_backup_manifest_size", path_utf8(path),
                            status_error.message());
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return backup_error(ErrorCode::kIo, "Unable to open backup manifest",
                            "backup_manifest_open_failed", path_utf8(path));
    std::string document;
    document.reserve(static_cast<std::size_t>(bytes));
    std::vector<char> buffer(kManifestReadChunkBytes);
    while (input)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
            document.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof() || document.size() != static_cast<std::size_t>(bytes))
        return backup_error(ErrorCode::kIo, "Unable to read complete backup manifest",
                            "backup_manifest_read_failed", path_utf8(path));
    return document;
}

[[nodiscard]] Result<ParsedBackupManifest>
parse_backup_manifest(const std::filesystem::path &backup_root,
                      const CancellationToken &cancellation)
{
    auto document = read_backup_manifest(
        backup_root / path_from_utf8(kCatalogBackupManifestFilename), cancellation);
    if (!document)
        return document.error();
    auto parsed = parse_json(document.value());
    if (!parsed)
    {
        auto error = parsed.error();
        error.context.insert_or_assign("reason", "invalid_backup_manifest_json");
        return error;
    }
    auto root = require_object(&parsed.value(), "root");
    if (!root)
        return root.error();
    auto schema = require_string(*root.value(), "schema", 64U);
    auto version = require_integer<std::int64_t>(*root.value(), "version", 1,
                                                 std::numeric_limits<std::int64_t>::max());
    auto created = require_integer<std::int64_t>(*root.value(), "created_unix_ms", 0,
                                                 std::numeric_limits<std::int64_t>::max());
    if (!schema)
        return schema.error();
    if (!version)
        return version.error();
    if (!created)
        return created.error();
    if (schema.value() != "ravo-catalog-backup")
        return backup_error(ErrorCode::kValidation, "Backup manifest schema identifier is invalid",
                            "invalid_backup_manifest_schema");
    if (version.value() > kCatalogBackupFormatVersion)
        return backup_error(ErrorCode::kUnsupported, "Backup format is newer than this Ravo",
                            "newer_backup_format");
    if (version.value() < kCatalogBackupFormatVersionMin)
        return backup_error(ErrorCode::kValidation, "Backup format version is invalid",
                            "invalid_backup_format_version");
    if (version.value() == 1)
    {
        auto root_keys = expect_exact_keys(
            *root.value(),
            {"catalog", "created_unix_ms", "excludes", "schema", "sidecars", "version"}, "root");
        if (!root_keys)
            return root_keys.error();
    }
    else if (version.value() == 2)
    {
        auto root_keys = expect_exact_keys(*root.value(),
                                           {"catalog", "created_unix_ms", "derived", "excludes",
                                            "external_editor", "schema", "sidecars", "version"},
                                           "root");
        if (!root_keys)
            return root_keys.error();
    }
    else
    {
        auto root_keys = expect_exact_keys(*root.value(),
                                           {"catalog", "created_unix_ms", "derived",
                                            "dng_conversion", "excludes", "external_editor",
                                            "schema", "sidecars", "smart_previews", "version"},
                                           "root");
        if (!root_keys)
            return root_keys.error();
    }

    auto catalog = require_object(parsed.value().find("catalog"), "catalog");
    if (!catalog)
        return catalog.error();
    auto catalog_keys = expect_exact_keys(
        *catalog.value(), {"bytes", "catalog_id", "file", "revision", "schema_version", "sha256"},
        "catalog");
    if (!catalog_keys)
        return catalog_keys.error();
    auto catalog_file = require_string(*catalog.value(), "file", 64U);
    auto catalog_id = require_string(*catalog.value(), "catalog_id", 512U);
    auto catalog_schema = require_integer<std::int64_t>(*catalog.value(), "schema_version", 1,
                                                        std::numeric_limits<std::int64_t>::max());
    auto catalog_revision = require_integer<std::int64_t>(*catalog.value(), "revision", 0,
                                                          std::numeric_limits<std::int64_t>::max());
    auto catalog_sha = require_string(*catalog.value(), "sha256", 64U);
    auto catalog_bytes = require_integer<std::uint64_t>(*catalog.value(), "bytes", 1U,
                                                        std::numeric_limits<std::uint64_t>::max());
    if (!catalog_file)
        return catalog_file.error();
    if (!catalog_id)
        return catalog_id.error();
    if (!catalog_schema)
        return catalog_schema.error();
    if (!catalog_revision)
        return catalog_revision.error();
    if (!catalog_sha)
        return catalog_sha.error();
    if (!catalog_bytes)
        return catalog_bytes.error();
    if (catalog_schema.value() > kCatalogSchemaVersion)
        return backup_error(ErrorCode::kUnsupported,
                            "Backup catalog schema is newer than this Ravo",
                            "newer_backup_catalog_schema");
    if (catalog_file.value() != kCatalogBackupCatalogFilename ||
        !valid_sha256(catalog_sha.value()) ||
        catalog_schema.value() < kCatalogRecoveryMinimumSchemaVersion)
        return backup_error(ErrorCode::kValidation, "Backup catalog descriptor is invalid",
                            "invalid_backup_catalog_descriptor");

    auto excludes = require_array(parsed.value().find("excludes"), "excludes");
    if (!excludes)
        return excludes.error();
    if (excludes.value()->size() != 2U || (*excludes.value())[0].string_if() == nullptr ||
        (*excludes.value())[1].string_if() == nullptr ||
        *(*excludes.value())[0].string_if() != "originals" ||
        *(*excludes.value())[1].string_if() != "previews")
        return backup_error(ErrorCode::kValidation, "Backup exclusions are invalid",
                            "invalid_backup_exclusions");

    auto sidecars = require_array(parsed.value().find("sidecars"), "sidecars");
    if (!sidecars)
        return sidecars.error();
    if (sidecars.value()->size() > kBackupMaximumAssets)
        return backup_error(ErrorCode::kValidation, "Backup contains too many sidecars",
                            "backup_sidecar_count_exceeded");

    ParsedBackupManifest manifest;
    manifest.created_unix_ms = created.value();
    manifest.catalog.path = path_utf8(backup_root / path_from_utf8(kCatalogBackupCatalogFilename));
    manifest.catalog.catalog_id = std::move(catalog_id).value();
    manifest.catalog.schema_version = catalog_schema.value();
    manifest.catalog.revision = catalog_revision.value();
    manifest.catalog.sha256 = std::move(catalog_sha).value();
    manifest.catalog.bytes = catalog_bytes.value();
    manifest.sidecars.reserve(sidecars.value()->size());
    std::string previous_asset_id;
    for (std::size_t index = 0; index < sidecars.value()->size(); ++index)
    {
        auto entry = require_object(&(*sidecars.value())[index], "sidecars[]");
        if (!entry)
            return entry.error();
        auto keys = expect_exact_keys(
            *entry.value(), {"asset_id", "bytes", "file", "generation", "sha256"}, "sidecars[]");
        if (!keys)
            return keys.error();
        auto asset_id = require_string(*entry.value(), "asset_id", 180U);
        auto generation = require_integer<std::int64_t>(*entry.value(), "generation", 1,
                                                        std::numeric_limits<std::int64_t>::max());
        auto file = require_string(*entry.value(), "file", 512U);
        auto checksum = require_string(*entry.value(), "sha256", 64U);
        auto bytes = require_integer<std::uint64_t>(*entry.value(), "bytes", 1U,
                                                    std::numeric_limits<std::uint64_t>::max());
        if (!asset_id)
            return asset_id.error();
        if (!generation)
            return generation.error();
        if (!file)
            return file.error();
        if (!checksum)
            return checksum.error();
        if (!bytes)
            return bytes.error();
        const auto expected_file = std::string(kCatalogBackupSidecarDirectory) + "/" +
                                   sidecar_filename(asset_id.value(), generation.value());
        if (!safe_asset_id(asset_id.value()) || !valid_sha256(checksum.value()) ||
            file.value() != expected_file ||
            (!previous_asset_id.empty() && asset_id.value() <= previous_asset_id))
            return backup_error(ErrorCode::kValidation, "Backup sidecar descriptor is invalid",
                                "invalid_backup_sidecar_descriptor");
        previous_asset_id = asset_id.value();
        manifest.sidecars.push_back({std::move(asset_id).value(), generation.value(),
                                     path_utf8(backup_root / path_from_utf8(file.value())),
                                     std::move(checksum).value(), bytes.value()});
    }
    manifest.format_version = version.value();
    if (version.value() >= 2)
    {
        auto derived = require_array(parsed.value().find("derived"), "derived");
        if (!derived)
            return derived.error();
        auto external = require_array(parsed.value().find("external_editor"), "external_editor");
        if (!external)
            return external.error();
        auto derived_files = catalog_backup_parse_tree_files(
            *derived.value(), kCatalogBackupDerivedDirectory, backup_root);
        if (!derived_files)
            return derived_files.error();
        auto external_files = catalog_backup_parse_tree_files(
            *external.value(), kCatalogBackupExternalEditorDirectory, backup_root);
        if (!external_files)
            return external_files.error();
        manifest.derived = std::move(derived_files).value();
        manifest.external_editor = std::move(external_files).value();
    }
    if (version.value() >= 3)
    {
        auto dng = require_array(parsed.value().find("dng_conversion"), "dng_conversion");
        if (!dng)
            return dng.error();
        auto smart = require_array(parsed.value().find("smart_previews"), "smart_previews");
        if (!smart)
            return smart.error();
        auto dng_files = catalog_backup_parse_tree_files(
            *dng.value(), kCatalogBackupDngConversionDirectory, backup_root);
        if (!dng_files)
            return dng_files.error();
        auto smart_files = catalog_backup_parse_tree_files(
            *smart.value(), kCatalogBackupSmartPreviewsDirectory, backup_root);
        if (!smart_files)
            return smart_files.error();
        manifest.dng_conversion = std::move(dng_files).value();
        manifest.smart_previews = std::move(smart_files).value();
    }
    return manifest;
}

[[nodiscard]] Result<void> verify_backup_layout(const std::filesystem::path &root,
                                                const ParsedBackupManifest &manifest)
{
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(root_status))
        return backup_error(ErrorCode::kValidation, "Backup root is not a directory",
                            "backup_root_not_directory", path_utf8(root), error.message());
    std::set<std::string, std::less<>> expected_root{std::string(kCatalogBackupCatalogFilename),
                                                     std::string(kCatalogBackupManifestFilename),
                                                     std::string(kCatalogBackupSidecarDirectory)};
    if (manifest.format_version >= 2)
    {
        expected_root.insert(std::string(kCatalogBackupDerivedDirectory));
        expected_root.insert(std::string(kCatalogBackupExternalEditorDirectory));
    }
    if (manifest.format_version >= 3)
    {
        expected_root.insert(std::string(kCatalogBackupDngConversionDirectory));
        expected_root.insert(std::string(kCatalogBackupSmartPreviewsDirectory));
    }
    std::set<std::string, std::less<>> actual_root;
    for (std::filesystem::directory_iterator iterator(root, error), end; iterator != end;
         iterator.increment(error))
    {
        if (error)
            return backup_error(ErrorCode::kIo, "Unable to enumerate backup directory",
                                "backup_enumeration_failed", path_utf8(root), error.message());
        actual_root.insert(path_utf8(iterator->path().filename()));
    }
    if (error || actual_root != expected_root)
        return backup_error(ErrorCode::kValidation,
                            "Backup root contains missing or unexpected entries",
                            "invalid_backup_root_layout", path_utf8(root), error.message());

    const auto sidecar_root = root / path_from_utf8(kCatalogBackupSidecarDirectory);
    const auto sidecar_status = std::filesystem::symlink_status(sidecar_root, error);
    if (error || !std::filesystem::is_directory(sidecar_status))
        return backup_error(ErrorCode::kValidation, "Backup sidecar root is not a directory",
                            "backup_sidecar_root_not_directory", path_utf8(sidecar_root),
                            error.message());
    const auto &sidecars = manifest.sidecars;
    std::set<std::string, std::less<>> expected_sidecars;
    for (const auto &sidecar : sidecars)
        expected_sidecars.insert(sidecar_filename(sidecar.asset_id, sidecar.generation));
    std::set<std::string, std::less<>> actual_sidecars;
    for (std::filesystem::directory_iterator iterator(sidecar_root, error), end; iterator != end;
         iterator.increment(error))
    {
        if (error)
            return backup_error(ErrorCode::kIo, "Unable to enumerate backup sidecars",
                                "backup_enumeration_failed", path_utf8(sidecar_root),
                                error.message());
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error || !std::filesystem::is_regular_file(status))
            return backup_error(ErrorCode::kValidation, "Backup sidecar is not a regular file",
                                "backup_sidecar_not_regular", path_utf8(iterator->path()),
                                status_error.message());
        actual_sidecars.insert(path_utf8(iterator->path().filename()));
        if (actual_sidecars.size() > expected_sidecars.size())
            return backup_error(ErrorCode::kValidation,
                                "Backup sidecar directory contains unexpected entries",
                                "invalid_backup_sidecar_layout", path_utf8(sidecar_root));
    }
    if (error || actual_sidecars != expected_sidecars)
        return backup_error(
            ErrorCode::kValidation, "Backup sidecar directory does not match its manifest",
            "invalid_backup_sidecar_layout", path_utf8(sidecar_root), error.message());
    if (manifest.format_version >= 2)
    {
        auto derived_layout = catalog_backup_verify_tree_layout(
            root / path_from_utf8(kCatalogBackupDerivedDirectory), manifest.derived);
        if (!derived_layout)
            return derived_layout.error();
        auto external_layout = catalog_backup_verify_tree_layout(
            root / path_from_utf8(kCatalogBackupExternalEditorDirectory), manifest.external_editor);
        if (!external_layout)
            return external_layout.error();
    }
    if (manifest.format_version >= 3)
    {
        auto dng_layout = catalog_backup_verify_tree_layout(
            root / path_from_utf8(kCatalogBackupDngConversionDirectory), manifest.dng_conversion);
        if (!dng_layout)
            return dng_layout.error();
        auto smart_layout = catalog_backup_verify_tree_layout(
            root / path_from_utf8(kCatalogBackupSmartPreviewsDirectory), manifest.smart_previews);
        if (!smart_layout)
            return smart_layout.error();
    }
    return {};
}

[[nodiscard]] JsonValue backup_manifest_json(
    const CatalogDatabaseArtifact &catalog, const std::vector<RecoveryArtifact> &sidecars,
    const std::vector<CatalogBackupTreeFile> &derived,
    const std::vector<CatalogBackupTreeFile> &external_editor,
    const std::vector<CatalogBackupTreeFile> &dng_conversion,
    const std::vector<CatalogBackupTreeFile> &smart_previews, const std::int64_t created_unix_ms)
{
    JsonValue::Array sidecar_array;
    sidecar_array.reserve(sidecars.size());
    for (const auto &sidecar : sidecars)
    {
        sidecar_array.emplace_back(JsonValue::Object{
            {"asset_id", sidecar.asset_id},
            {"bytes", JsonValue::number(std::to_string(sidecar.bytes))},
            {"file", std::string(kCatalogBackupSidecarDirectory) + "/" +
                         sidecar_filename(sidecar.asset_id, sidecar.generation)},
            {"generation", JsonValue::number(std::to_string(sidecar.generation))},
            {"sha256", sidecar.sha256},
        });
    }
    return JsonValue::Object{
        {"catalog",
         JsonValue::Object{
             {"bytes", JsonValue::number(std::to_string(catalog.bytes))},
             {"catalog_id", catalog.catalog_id},
             {"file", std::string(kCatalogBackupCatalogFilename)},
             {"revision", JsonValue::number(std::to_string(catalog.revision))},
             {"schema_version", JsonValue::number(std::to_string(catalog.schema_version))},
             {"sha256", catalog.sha256},
         }},
        {"created_unix_ms", JsonValue::number(std::to_string(created_unix_ms))},
        {"derived", catalog_backup_tree_files_json(derived, kCatalogBackupDerivedDirectory)},
        {"dng_conversion",
         catalog_backup_tree_files_json(dng_conversion, kCatalogBackupDngConversionDirectory)},
        {"excludes", JsonValue::Array{JsonValue{"originals"}, JsonValue{"previews"}}},
        {"external_editor",
         catalog_backup_tree_files_json(external_editor, kCatalogBackupExternalEditorDirectory)},
        {"schema", "ravo-catalog-backup"},
        {"sidecars", std::move(sidecar_array)},
        {"smart_previews",
         catalog_backup_tree_files_json(smart_previews, kCatalogBackupSmartPreviewsDirectory)},
        {"version", JsonValue::number(std::to_string(kCatalogBackupFormatVersion))},
    };
}

} // namespace

Result<CatalogBackupArtifact> CatalogService::create_backup(const std::string_view destination,
                                                            const CancellationToken &cancellation)
{
    if (repository_ == nullptr || recovery_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (destination.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog backup destination must not be empty");
    auto active = cancellation.check();
    if (!active)
        return active.error();
    const auto output = path_from_utf8(destination);
    if (output.filename().empty())
        return backup_error(ErrorCode::kInvalidArgument,
                            "Catalog backup destination must name a directory",
                            "invalid_backup_destination", std::string(destination));
    std::error_code status_error;
    const auto output_status = std::filesystem::symlink_status(output, status_error);
    if (!status_error && output_status.type() != std::filesystem::file_type::not_found)
        return backup_error(ErrorCode::kConflict, "Catalog backup destination already exists",
                            "backup_destination_conflict", std::string(destination));
    if (status_error != std::errc::no_such_file_or_directory)
        return backup_error(ErrorCode::kIo, "Unable to inspect catalog backup destination",
                            "backup_destination_inspect_failed", std::string(destination),
                            status_error.message());

    const auto checkpoint = [this, &cancellation](const std::string_view name,
                                                  const std::string_view path) -> Result<void>
    {
        if (testing_backup_checkpoint_)
        {
            auto injected = testing_backup_checkpoint_(name, path);
            if (!injected)
            {
                auto error = std::move(injected).error();
                error.context.insert_or_assign("checkpoint", std::string(name));
                if (!path.empty())
                    error.context.insert_or_assign("path", std::string(path));
                return error;
            }
        }
        auto active = cancellation.check();
        if (!active)
        {
            auto error = std::move(active).error();
            error.context.insert_or_assign("checkpoint", std::string(name));
            if (!path.empty())
                error.context.insert_or_assign("path", std::string(path));
            return error;
        }
        return {};
    };
    auto ready = checkpoint("before_snapshot", destination);
    if (!ready)
        return ready.error();

    auto synchronized = sync_recovery(std::nullopt, cancellation);
    if (!synchronized)
        return synchronized.error();
    auto before = repository_->snapshot();
    if (!before)
        return before.error();
    auto states_before = repository_->list_recovery_states();
    if (!states_before)
        return states_before.error();
    if (std::any_of(states_before.value().begin(), states_before.value().end(),
                    [](const AssetRecoveryState &state) { return state.pending(); }))
        return backup_error(ErrorCode::kConflict, "Catalog has pending recovery generations",
                            "backup_recovery_pending");

    atomic_publication_internal::OwnedTemporaryDirectory stage;
    std::error_code create_error;
    for (int attempt = 0; attempt < 16 && stage.path().empty(); ++attempt)
    {
        const auto candidate =
            atomic_publication_internal::temporary_candidate(output, "catalog-backup");
        create_error.clear();
        if (std::filesystem::create_directory(candidate, create_error))
            stage.reset(candidate);
        else if (create_error && create_error != std::errc::file_exists)
            break;
    }
    if (stage.path().empty())
        return backup_error(ErrorCode::kIo, "Unable to create catalog backup staging directory",
                            "backup_staging_create_failed", std::string(destination),
                            create_error.message());
    const auto fail = [&stage](TaskError error) -> Result<CatalogBackupArtifact>
    {
        const auto cleanup_error = stage.remove();
        if (cleanup_error)
        {
            error.context.insert_or_assign("cleanup_failed", "true");
            error.context.insert_or_assign("cleanup_error", cleanup_error.message());
        }
        return error;
    };
    ready = checkpoint("stage_created", path_utf8(stage.path()));
    if (!ready)
        return fail(ready.error());

    const auto stage_sidecars = stage.path() / path_from_utf8(kCatalogBackupSidecarDirectory);
    if (!std::filesystem::create_directory(stage_sidecars, create_error))
        return fail(backup_error(ErrorCode::kIo,
                                 "Unable to create backup sidecar staging directory",
                                 "backup_sidecar_staging_create_failed", path_utf8(stage_sidecars),
                                 create_error.message()));
    ready = checkpoint("sidecar_directory_created", path_utf8(stage_sidecars));
    if (!ready)
        return fail(ready.error());

    const auto stage_catalog = stage.path() / path_from_utf8(kCatalogBackupCatalogFilename);
    auto catalog = repository_->create_backup_database(path_utf8(stage_catalog), cancellation);
    if (!catalog)
        return fail(catalog.error());
    if (catalog.value().catalog_id != before.value().catalog_id ||
        catalog.value().revision != before.value().revision ||
        catalog.value().schema_version != before.value().schema_version ||
        catalog.value().recovery_states != states_before.value())
        return fail(backup_error(ErrorCode::kConflict,
                                 "Catalog changed while its backup snapshot was created",
                                 "backup_snapshot_changed"));
    ready = checkpoint("database_snapshot_created", path_utf8(stage_catalog));
    if (!ready)
        return fail(ready.error());

    std::vector<RecoveryArtifact> copied_sidecars;
    copied_sidecars.reserve(catalog.value().recovery_states.size());
    for (const auto &state : catalog.value().recovery_states)
    {
        active = cancellation.check();
        if (!active)
            return fail(active.error());
        auto source = recovery_->verify(state.asset_id, state.generation, cancellation);
        if (!source)
        {
            auto error = source.error();
            error.context.insert_or_assign("asset_id", state.asset_id);
            error.context.insert_or_assign("reason", "backup_source_sidecar_invalid");
            return fail(std::move(error));
        }
        const auto output_sidecar =
            stage_sidecars / path_from_utf8(sidecar_filename(state.asset_id, state.generation));
        auto copied =
            copy_file_atomically(source.value().path, path_utf8(output_sidecar), cancellation);
        if (!copied)
        {
            auto error = copied.error();
            error.context.insert_or_assign("asset_id", state.asset_id);
            error.context.insert_or_assign("reason", "backup_sidecar_copy_failed");
            return fail(std::move(error));
        }
        auto verified = recovery_->verify_artifact(path_utf8(output_sidecar), state.asset_id,
                                                   state.generation, cancellation);
        if (!verified)
            return fail(verified.error());
        if (verified.value().sha256 != source.value().sha256 ||
            verified.value().bytes != source.value().bytes ||
            copied.value() != source.value().bytes)
            return fail(backup_error(ErrorCode::kValidation,
                                     "Copied backup sidecar differs from its source",
                                     "backup_sidecar_copy_mismatch", path_utf8(output_sidecar)));
        copied_sidecars.push_back(std::move(verified).value());
        ready = checkpoint("sidecar_copied", path_utf8(output_sidecar));
        if (!ready)
            return fail(ready.error());
    }

    const auto live_support = path_from_utf8(before.value().database_path + ".ravo");
    const auto stage_derived = stage.path() / path_from_utf8(kCatalogBackupDerivedDirectory);
    auto copied_derived = catalog_backup_copy_tree(
        live_support / path_from_utf8(kCatalogBackupDerivedDirectory), stage_derived, cancellation);
    if (!copied_derived)
        return fail(copied_derived.error());
    ready = checkpoint("derived_tree_copied", path_utf8(stage_derived));
    if (!ready)
        return fail(ready.error());
    const auto stage_external =
        stage.path() / path_from_utf8(kCatalogBackupExternalEditorDirectory);
    auto copied_external = catalog_backup_copy_tree(
        live_support / path_from_utf8(kCatalogBackupExternalEditorDirectory), stage_external,
        cancellation);
    if (!copied_external)
        return fail(copied_external.error());
    ready = checkpoint("external_editor_tree_copied", path_utf8(stage_external));
    if (!ready)
        return fail(ready.error());
    const auto stage_dng = stage.path() / path_from_utf8(kCatalogBackupDngConversionDirectory);
    auto copied_dng = catalog_backup_copy_tree(
        live_support / path_from_utf8(kCatalogBackupDngConversionDirectory), stage_dng,
        cancellation);
    if (!copied_dng)
        return fail(copied_dng.error());
    ready = checkpoint("dng_conversion_tree_copied", path_utf8(stage_dng));
    if (!ready)
        return fail(ready.error());
    const auto stage_smart = stage.path() / path_from_utf8(kCatalogBackupSmartPreviewsDirectory);
    auto copied_smart = catalog_backup_copy_tree(
        live_support / path_from_utf8(kCatalogBackupSmartPreviewsDirectory), stage_smart,
        cancellation);
    if (!copied_smart)
        return fail(copied_smart.error());
    ready = checkpoint("smart_previews_tree_copied", path_utf8(stage_smart));
    if (!ready)
        return fail(ready.error());

    auto after = repository_->snapshot();
    if (!after)
        return fail(after.error());
    auto states_after = repository_->list_recovery_states();
    if (!states_after)
        return fail(states_after.error());
    if (after.value().catalog_id != before.value().catalog_id ||
        after.value().revision != before.value().revision ||
        states_after.value() != states_before.value())
        return fail(backup_error(ErrorCode::kConflict,
                                 "Catalog changed while its backup was materialized",
                                 "backup_source_changed"));
    ready = checkpoint("source_rechecked", path_utf8(stage.path()));
    if (!ready)
        return fail(ready.error());

    const auto created_unix_ms = now_unix_ms();
    const auto manifest_path = stage.path() / path_from_utf8(kCatalogBackupManifestFilename);
    const auto manifest = serialize_json(backup_manifest_json(
        catalog.value(), copied_sidecars, copied_derived.value(), copied_external.value(),
        copied_dng.value(), copied_smart.value(), created_unix_ms));
    if (manifest.size() > kCatalogBackupManifestMaximumBytes)
        return fail(backup_error(ErrorCode::kValidation, "Backup manifest exceeds its byte limit",
                                 "backup_manifest_too_large", path_utf8(manifest_path)));
    auto published_manifest =
        publish_text_artifact_no_replace(path_utf8(manifest_path), manifest, cancellation);
    if (!published_manifest)
        return fail(published_manifest.error());
    ready = checkpoint("manifest_published", path_utf8(manifest_path));
    if (!ready)
        return fail(ready.error());

    auto verified = verify_backup(path_utf8(stage.path()), cancellation);
    if (!verified)
        return fail(verified.error());
    ready = checkpoint("staging_verified", path_utf8(stage.path()));
    if (!ready)
        return fail(ready.error());
    ready = checkpoint("before_publish", path_utf8(stage.path()));
    if (!ready)
        return fail(ready.error());
    const auto publish_error =
        atomic_publication_internal::publish_no_replace(stage.path(), output);
    if (publish_error)
    {
        const auto code =
            publish_error == std::errc::file_exists ? ErrorCode::kConflict : ErrorCode::kIo;
        return fail(backup_error(code, "Unable to publish catalog backup directory",
                                 code == ErrorCode::kConflict ? "backup_destination_conflict" :
                                                                "backup_publish_failed",
                                 std::string(destination), publish_error.message()));
    }
    stage.release();

    auto artifact = std::move(verified).value().artifact;
    artifact.path = std::string(destination);
    artifact.manifest_path = path_utf8(output / path_from_utf8(kCatalogBackupManifestFilename));
    artifact.catalog.path = path_utf8(output / path_from_utf8(kCatalogBackupCatalogFilename));
    return artifact;
}

Result<CatalogBackupVerification> verify_catalog_backup(
    const CatalogBackupDatabaseVerifier &database_verifier, const RecoveryStore &recovery_verifier,
    const std::string_view backup_directory, const CancellationToken &cancellation)
{
    if (backup_directory.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog backup directory must not be empty");
    auto active = cancellation.check();
    if (!active)
        return active.error();
    const auto root = path_from_utf8(backup_directory);
    auto manifest = parse_backup_manifest(root, cancellation);
    if (!manifest)
        return manifest.error();
    auto layout = verify_backup_layout(root, manifest.value());
    if (!layout)
        return layout.error();
    auto catalog = database_verifier.verify_backup_database(
        manifest.value().catalog.path, manifest.value().catalog.sha256, cancellation);
    if (!catalog)
        return catalog.error();
    if (catalog.value().catalog_id != manifest.value().catalog.catalog_id ||
        catalog.value().schema_version != manifest.value().catalog.schema_version ||
        catalog.value().revision != manifest.value().catalog.revision ||
        catalog.value().bytes != manifest.value().catalog.bytes ||
        catalog.value().recovery_states.size() != manifest.value().sidecars.size())
        return backup_error(ErrorCode::kValidation, "Backup catalog does not match its manifest",
                            "backup_catalog_manifest_mismatch", manifest.value().catalog.path);

    std::uint64_t sidecar_bytes = 0U;
    for (std::size_t index = 0; index < manifest.value().sidecars.size(); ++index)
    {
        const auto &expected = manifest.value().sidecars[index];
        const auto &database_state = catalog.value().recovery_states[index];
        if (database_state.asset_id != expected.asset_id ||
            database_state.generation != expected.generation || database_state.pending())
            return backup_error(ErrorCode::kValidation,
                                "Backup sidecars do not match catalog recovery state",
                                "backup_sidecar_catalog_mismatch", expected.path);
        auto verified = recovery_verifier.verify_artifact(expected.path, expected.asset_id,
                                                          expected.generation, cancellation);
        if (!verified)
            return verified.error();
        if (verified.value().sha256 != expected.sha256 || verified.value().bytes != expected.bytes)
            return backup_error(ErrorCode::kValidation,
                                "Backup sidecar does not match its manifest",
                                "backup_sidecar_manifest_mismatch", expected.path);
        if (sidecar_bytes > std::numeric_limits<std::uint64_t>::max() - expected.bytes)
            return backup_error(ErrorCode::kValidation, "Backup sidecar byte count overflows",
                                "backup_sidecar_size_overflow");
        sidecar_bytes += expected.bytes;
    }
    auto derived_bytes =
        catalog_backup_verify_tree_checksums(manifest.value().derived, cancellation);
    if (!derived_bytes)
        return derived_bytes.error();
    auto external_bytes =
        catalog_backup_verify_tree_checksums(manifest.value().external_editor, cancellation);
    if (!external_bytes)
        return external_bytes.error();
    auto dng_bytes =
        catalog_backup_verify_tree_checksums(manifest.value().dng_conversion, cancellation);
    if (!dng_bytes)
        return dng_bytes.error();
    auto smart_bytes =
        catalog_backup_verify_tree_checksums(manifest.value().smart_previews, cancellation);
    if (!smart_bytes)
        return smart_bytes.error();
    active = cancellation.check();
    if (!active)
        return active.error();

    CatalogBackupVerification verification;
    verification.artifact.path = std::string(backup_directory);
    verification.artifact.manifest_path =
        path_utf8(root / path_from_utf8(kCatalogBackupManifestFilename));
    verification.artifact.catalog = std::move(catalog).value();
    verification.artifact.format_version = manifest.value().format_version;
    verification.artifact.created_unix_ms = manifest.value().created_unix_ms;
    verification.artifact.sidecar_count = manifest.value().sidecars.size();
    verification.artifact.sidecar_bytes = sidecar_bytes;
    verification.artifact.derived_count = manifest.value().derived.size();
    verification.artifact.derived_bytes = derived_bytes.value();
    verification.artifact.external_editor_count = manifest.value().external_editor.size();
    verification.artifact.external_editor_bytes = external_bytes.value();
    verification.artifact.dng_conversion_count = manifest.value().dng_conversion.size();
    verification.artifact.dng_conversion_bytes = dng_bytes.value();
    verification.artifact.smart_previews_count = manifest.value().smart_previews.size();
    verification.artifact.smart_previews_bytes = smart_bytes.value();
    verification.originals_included = false;
    verification.previews_included = false;
    return verification;
}

Result<CatalogBackupVerification>
CatalogService::verify_backup(const std::string_view backup_directory,
                              const CancellationToken &cancellation) const
{
    if (repository_ == nullptr || recovery_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return verify_catalog_backup(*repository_, *recovery_, backup_directory, cancellation);
}

} // namespace ravo
