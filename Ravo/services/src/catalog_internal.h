#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] std::int64_t now_unix_ms();
[[nodiscard]] ImportItemResult failed_item(std::string path, TaskError error);
[[nodiscard]] ImportItemResult unsupported_item(std::string path, TaskError error);
[[nodiscard]] std::string lower_ascii(std::string value);
[[nodiscard]] std::string extension_lower(const std::filesystem::path &path);
[[nodiscard]] bool is_raw_extension(const std::filesystem::path &path);
[[nodiscard]] bool is_import_candidate(const std::filesystem::path &path);
[[nodiscard]] Result<std::vector<std::string>>
collect_import_paths(const std::vector<std::string> &inputs, const CancellationToken &cancellation);
[[nodiscard]] std::string fnv1a64_hex(std::string_view text);
[[nodiscard]] Recipe identity_recipe_for(const AssetRecord &asset, const std::string &path);
[[nodiscard]] DevelopParams baseline_develop_for(const AssetRecord &asset);
[[nodiscard]] Result<Recipe> baseline_recipe_for(const AssetRecord &asset, const std::string &path);
[[nodiscard]] bool matches_develop_baseline(const AssetRecord &asset, DevelopParams params);
[[nodiscard]] std::string parameter_key_part(const ParameterValue &value);
[[nodiscard]] std::string raw_preprocess_key(const Recipe &recipe);
void disable_raw_preprocess(Recipe &recipe);
[[nodiscard]] std::filesystem::path utf8_path(std::string_view text);
[[nodiscard]] bool is_disk_full(const std::error_code &error) noexcept;
[[nodiscard]] TaskError export_io_error(std::string message, std::string_view path,
                                        const std::error_code &error);
[[nodiscard]] Result<void> write_bytes_atomically(std::string_view dest_utf8,
                                                  const std::vector<std::uint8_t> &bytes,
                                                  const CancellationToken &cancellation);
[[nodiscard]] Result<std::uint64_t> copy_file_atomically(std::string_view source_utf8,
                                                         std::string_view dest_utf8,
                                                         const CancellationToken &cancellation);

} // namespace ravo
