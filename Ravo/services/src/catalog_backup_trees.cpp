#include "catalog_backup_trees.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/adapters/text_file.h"

namespace ravo
{
namespace
{

constexpr std::size_t kBackupMaximumTreeFiles = 1'000'000U;

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] TaskError tree_error(const ErrorCode code, std::string message, std::string reason,
                                   std::string path = {}, std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
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

[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue *value,
                                                               const std::string_view field)
{
    if (value == nullptr || value->object_if() == nullptr)
        return tree_error(ErrorCode::kValidation, "Backup tree entry must be an object",
                          "backup_manifest_type_mismatch", {}, std::string(field));
    return value->object_if();
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object,
                                                 const std::string_view key,
                                                 const std::size_t maximum_bytes)
{
    const auto found = object.find(key);
    const auto *value = found == object.end() ? nullptr : found->second.string_if();
    if (value == nullptr || value->empty() || value->size() > maximum_bytes)
        return tree_error(ErrorCode::kValidation, "Backup tree string is invalid",
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
        return tree_error(ErrorCode::kValidation, "Backup tree integer is missing",
                          "backup_manifest_type_mismatch", {}, std::string(key));
    Integer value{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        value < minimum || value > maximum)
        return tree_error(ErrorCode::kValidation,
                          "Backup tree integer is outside its supported bounds",
                          "invalid_backup_manifest_integer", {}, std::string(key));
    return value;
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
            return tree_error(ErrorCode::kValidation, "Backup tree entry contains an unknown field",
                              "unknown_backup_manifest_field", {}, std::string(owner) + "." + key);
    }
    for (const auto &key : expected)
    {
        if (!object.contains(key))
            return tree_error(ErrorCode::kValidation,
                              "Backup tree entry is missing a required field",
                              "missing_backup_manifest_field", {}, std::string(owner) + "." + key);
    }
    return {};
}

} // namespace

bool catalog_backup_safe_relative_path(const std::string_view relative) noexcept
{
    if (relative.empty() || relative.size() > 1024U || relative.front() == '/' ||
        relative.find('\\') != std::string_view::npos)
        return false;
    std::size_t begin = 0U;
    while (begin <= relative.size())
    {
        const auto end = relative.find('/', begin);
        const auto part = relative.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (const char raw : part)
        {
            const auto ch = static_cast<unsigned char>(raw);
            if (std::isalnum(ch) != 0 || raw == '_' || raw == '-' || raw == '.' || raw == ' ')
                continue;
            return false;
        }
        if (end == std::string_view::npos)
            break;
        begin = end + 1U;
    }
    return true;
}

Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_enumerate_tree(const std::filesystem::path &tree_root,
                              const CancellationToken &cancellation)
{
    std::vector<CatalogBackupTreeFile> files;
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(tree_root, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        (!status_error && status.type() == std::filesystem::file_type::not_found))
        return files;
    if (status_error || !std::filesystem::is_directory(status))
        return tree_error(ErrorCode::kValidation, "Backup source tree root is invalid",
                          "backup_tree_root_invalid", path_utf8(tree_root), status_error.message());

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator
             iterator(tree_root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error))
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        if (error)
            return tree_error(ErrorCode::kIo, "Unable to enumerate backup source tree",
                              "backup_tree_enumeration_failed", path_utf8(tree_root),
                              error.message());
        std::error_code entry_error;
        const auto entry_status = iterator->symlink_status(entry_error);
        if (entry_error)
            return tree_error(ErrorCode::kIo, "Unable to inspect backup source tree entry",
                              "backup_tree_entry_inspect_failed", path_utf8(iterator->path()),
                              entry_error.message());
        if (std::filesystem::is_symlink(entry_status))
            return tree_error(ErrorCode::kValidation,
                              "Backup source tree must not contain symlinks",
                              "backup_tree_symlink_rejected", path_utf8(iterator->path()));
        if (!std::filesystem::is_regular_file(entry_status))
            continue;
        const auto relative = path_utf8(iterator->path().lexically_relative(tree_root));
        if (!catalog_backup_safe_relative_path(relative))
            return tree_error(ErrorCode::kValidation, "Backup source tree path is unsafe",
                              "invalid_backup_tree_relative_path", relative);
        auto digest = sha256_file_hex(path_utf8(iterator->path()));
        if (!digest)
            return digest.error();
        std::error_code size_error;
        const auto bytes = std::filesystem::file_size(iterator->path(), size_error);
        if (size_error || bytes == 0U ||
            bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max()))
            return tree_error(ErrorCode::kValidation, "Backup source tree file size is invalid",
                              "invalid_backup_tree_file_size", path_utf8(iterator->path()),
                              size_error.message());
        CatalogBackupTreeFile entry;
        entry.relative_path = relative;
        entry.path = path_utf8(iterator->path());
        entry.sha256 = std::move(digest).value();
        entry.bytes = static_cast<std::uint64_t>(bytes);
        files.push_back(std::move(entry));
        if (files.size() > kBackupMaximumTreeFiles)
            return tree_error(ErrorCode::kValidation, "Backup source tree contains too many files",
                              "backup_tree_count_exceeded", path_utf8(tree_root));
    }
    if (error)
        return tree_error(ErrorCode::kIo, "Unable to finish enumerating backup source tree",
                          "backup_tree_enumeration_failed", path_utf8(tree_root), error.message());
    std::sort(files.begin(), files.end(),
              [](const CatalogBackupTreeFile &left, const CatalogBackupTreeFile &right)
              { return left.relative_path < right.relative_path; });
    return files;
}

Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_copy_tree(const std::filesystem::path &source_root,
                         const std::filesystem::path &destination_root,
                         const CancellationToken &cancellation)
{
    std::error_code create_error;
    if (!std::filesystem::create_directories(destination_root, create_error) && create_error)
        return tree_error(ErrorCode::kIo, "Unable to create backup tree staging directory",
                          "backup_tree_staging_create_failed", path_utf8(destination_root),
                          create_error.message());
    auto enumerated = catalog_backup_enumerate_tree(source_root, cancellation);
    if (!enumerated)
        return enumerated.error();
    std::vector<CatalogBackupTreeFile> copied;
    copied.reserve(enumerated.value().size());
    for (const auto &source : enumerated.value())
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const auto destination = destination_root / path_from_utf8(source.relative_path);
        if (!std::filesystem::create_directories(destination.parent_path(), create_error) &&
            create_error)
            return tree_error(ErrorCode::kIo, "Unable to create backup tree parent directory",
                              "backup_tree_parent_create_failed",
                              path_utf8(destination.parent_path()), create_error.message());
        auto published = copy_file_atomically(source.path, path_utf8(destination), cancellation);
        if (!published)
            return published.error();
        if (published.value() != source.bytes)
            return tree_error(ErrorCode::kValidation,
                              "Copied backup tree file differs in size from its source",
                              "backup_tree_copy_size_mismatch", path_utf8(destination));
        auto digest = sha256_file_hex(path_utf8(destination));
        if (!digest)
            return digest.error();
        if (digest.value() != source.sha256)
            return tree_error(ErrorCode::kValidation,
                              "Copied backup tree file differs from its source checksum",
                              "backup_tree_copy_checksum_mismatch", path_utf8(destination));
        CatalogBackupTreeFile entry = source;
        entry.path = path_utf8(destination);
        copied.push_back(std::move(entry));
    }
    return copied;
}

Result<void> catalog_backup_verify_tree_layout(const std::filesystem::path &tree_root,
                                               const std::vector<CatalogBackupTreeFile> &expected)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(tree_root, error);
    if (error || !std::filesystem::is_directory(status))
        return tree_error(ErrorCode::kValidation, "Backup tree root is not a directory",
                          "backup_tree_root_not_directory", path_utf8(tree_root), error.message());
    std::set<std::string, std::less<>> expected_names;
    for (const auto &entry : expected)
        expected_names.insert(entry.relative_path);
    std::set<std::string, std::less<>> actual_names;
    for (std::filesystem::recursive_directory_iterator
             iterator(tree_root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error))
    {
        if (error)
            return tree_error(ErrorCode::kIo, "Unable to enumerate backup tree",
                              "backup_tree_enumeration_failed", path_utf8(tree_root),
                              error.message());
        std::error_code entry_error;
        const auto entry_status = iterator->symlink_status(entry_error);
        if (entry_error)
            return tree_error(ErrorCode::kIo, "Unable to inspect backup tree entry",
                              "backup_tree_entry_inspect_failed", path_utf8(iterator->path()),
                              entry_error.message());
        if (std::filesystem::is_symlink(entry_status))
            return tree_error(ErrorCode::kValidation, "Backup tree must not contain symlinks",
                              "backup_tree_symlink_rejected", path_utf8(iterator->path()));
        if (!std::filesystem::is_regular_file(entry_status))
            continue;
        const auto relative = path_utf8(iterator->path().lexically_relative(tree_root));
        if (!catalog_backup_safe_relative_path(relative))
            return tree_error(ErrorCode::kValidation, "Backup tree path is unsafe",
                              "invalid_backup_tree_relative_path", relative);
        actual_names.insert(relative);
        if (actual_names.size() > expected_names.size())
            return tree_error(ErrorCode::kValidation,
                              "Backup tree directory contains unexpected entries",
                              "invalid_backup_tree_layout", path_utf8(tree_root));
    }
    if (error || actual_names != expected_names)
        return tree_error(ErrorCode::kValidation,
                          "Backup tree directory does not match its manifest",
                          "invalid_backup_tree_layout", path_utf8(tree_root), error.message());
    return {};
}

Result<std::uint64_t>
catalog_backup_verify_tree_checksums(const std::vector<CatalogBackupTreeFile> &expected,
                                     const CancellationToken &cancellation)
{
    std::uint64_t total_bytes = 0U;
    for (const auto &entry : expected)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        std::error_code status_error;
        const auto status =
            std::filesystem::symlink_status(path_from_utf8(entry.path), status_error);
        if (status_error || !std::filesystem::is_regular_file(status))
            return tree_error(ErrorCode::kValidation, "Backup tree file is missing",
                              "backup_tree_file_missing", entry.path, status_error.message());
        std::error_code size_error;
        const auto bytes = std::filesystem::file_size(path_from_utf8(entry.path), size_error);
        if (size_error || static_cast<std::uint64_t>(bytes) != entry.bytes)
            return tree_error(ErrorCode::kValidation,
                              "Backup tree file size does not match manifest",
                              "backup_tree_size_mismatch", entry.path, size_error.message());
        auto digest = sha256_file_hex(entry.path);
        if (!digest)
            return digest.error();
        if (digest.value() != entry.sha256)
            return tree_error(ErrorCode::kValidation,
                              "Backup tree file checksum does not match manifest",
                              "backup_tree_checksum_mismatch", entry.path);
        if (total_bytes > std::numeric_limits<std::uint64_t>::max() - entry.bytes)
            return tree_error(ErrorCode::kValidation, "Backup tree byte count overflows",
                              "backup_tree_size_overflow");
        total_bytes += entry.bytes;
    }
    return total_bytes;
}

JsonValue::Array catalog_backup_tree_files_json(const std::vector<CatalogBackupTreeFile> &files,
                                                const std::string_view directory_name)
{
    JsonValue::Array array;
    array.reserve(files.size());
    for (const auto &file : files)
    {
        array.emplace_back(JsonValue::Object{
            {"bytes", JsonValue::number(std::to_string(file.bytes))},
            {"file", std::string(directory_name) + "/" + file.relative_path},
            {"sha256", file.sha256},
        });
    }
    return array;
}

Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_parse_tree_files(const JsonValue::Array &entries,
                                const std::string_view directory_name,
                                const std::filesystem::path &backup_root)
{
    if (entries.size() > kBackupMaximumTreeFiles)
        return tree_error(ErrorCode::kValidation, "Backup contains too many tree files",
                          "backup_tree_count_exceeded");
    std::vector<CatalogBackupTreeFile> files;
    files.reserve(entries.size());
    std::string previous;
    const auto prefix = std::string(directory_name) + "/";
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        auto entry = require_object(&entries[index], std::string(directory_name) + "[]");
        if (!entry)
            return entry.error();
        auto keys = expect_exact_keys(*entry.value(), {"bytes", "file", "sha256"},
                                      std::string(directory_name) + "[]");
        if (!keys)
            return keys.error();
        auto file = require_string(*entry.value(), "file", 1200U);
        auto checksum = require_string(*entry.value(), "sha256", 64U);
        auto bytes = require_integer<std::uint64_t>(*entry.value(), "bytes", 1U,
                                                    std::numeric_limits<std::uint64_t>::max());
        if (!file)
            return file.error();
        if (!checksum)
            return checksum.error();
        if (!bytes)
            return bytes.error();
        if (file.value().size() <= prefix.size() ||
            file.value().compare(0, prefix.size(), prefix) != 0)
            return tree_error(ErrorCode::kValidation, "Backup tree file path is outside its tree",
                              "invalid_backup_tree_descriptor");
        const auto relative = file.value().substr(prefix.size());
        if (!catalog_backup_safe_relative_path(relative) || !valid_sha256(checksum.value()) ||
            (!previous.empty() && relative <= previous))
            return tree_error(ErrorCode::kValidation, "Backup tree descriptor is invalid",
                              "invalid_backup_tree_descriptor");
        previous = relative;
        CatalogBackupTreeFile parsed;
        parsed.relative_path = relative;
        parsed.path = path_utf8(backup_root / path_from_utf8(file.value()));
        parsed.sha256 = std::move(checksum).value();
        parsed.bytes = bytes.value();
        files.push_back(std::move(parsed));
    }
    return files;
}

} // namespace ravo
