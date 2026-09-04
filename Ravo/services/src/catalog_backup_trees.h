#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

[[nodiscard]] bool catalog_backup_safe_relative_path(std::string_view relative) noexcept;

[[nodiscard]] Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_enumerate_tree(const std::filesystem::path &tree_root,
                              const CancellationToken &cancellation);

[[nodiscard]] Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_copy_tree(const std::filesystem::path &source_root,
                         const std::filesystem::path &destination_root,
                         const CancellationToken &cancellation);

[[nodiscard]] Result<void>
catalog_backup_verify_tree_layout(const std::filesystem::path &tree_root,
                                  const std::vector<CatalogBackupTreeFile> &expected);

[[nodiscard]] Result<std::uint64_t>
catalog_backup_verify_tree_checksums(const std::vector<CatalogBackupTreeFile> &expected,
                                     const CancellationToken &cancellation);

[[nodiscard]] JsonValue::Array
catalog_backup_tree_files_json(const std::vector<CatalogBackupTreeFile> &files,
                               std::string_view directory_name);

[[nodiscard]] Result<std::vector<CatalogBackupTreeFile>>
catalog_backup_parse_tree_files(const JsonValue::Array &entries, std::string_view directory_name,
                                const std::filesystem::path &backup_root);

} // namespace ravo
