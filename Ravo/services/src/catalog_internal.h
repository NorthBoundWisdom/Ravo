#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

enum class OriginalCopyCheckpoint : std::uint8_t
{
    kSourceOpened,
    kBeforeSourceRead,
    kSourceChunkRead,
    kBeforeTemporaryOpen,
    kTemporaryCreated,
    kBeforeTemporaryWrite,
    kTemporaryChunkWritten,
    kBeforeTemporaryFinish,
    kBeforePublish,
};

struct OriginalCopyCheckpointHook
{
    using Callback = std::error_code (*)(void *context, OriginalCopyCheckpoint checkpoint,
                                         std::string_view path,
                                         std::uint64_t bytes_processed) noexcept;

    Callback callback = nullptr;
    // Internal synchronous test seam. The caller keeps this non-owning context
    // alive for the complete copy call. The checkpoint path is borrowed only
    // for the callback: source checkpoints identify the source; temporary and
    // pre-publish checkpoints identify the candidate or owned temporary file.
    // kBeforePublish runs only after the temporary file has been finished. A
    // nonzero return injects that operation error at the observed stage.
    void *context = nullptr;
};

enum class EncodedPublicationCheckpoint : std::uint8_t
{
    kBeforeTemporaryOpen,
    kTemporaryCreated,
    kBeforeTemporaryWrite,
    kTemporaryChunkWritten,
    kBeforeTemporarySync,
    kBeforeTemporaryClose,
    kBeforePublish,
};

struct EncodedPublicationCheckpointHook
{
    using Callback = std::error_code (*)(void *context, EncodedPublicationCheckpoint checkpoint,
                                         std::string_view path,
                                         std::uint64_t bytes_processed) noexcept;

    Callback callback = nullptr;
    // Internal synchronous test seam. The caller keeps this non-owning context
    // alive for the complete publication call. Paths are borrowed only for the
    // callback. A nonzero return injects an operation error at that stage.
    void *context = nullptr;
};

[[nodiscard]] std::int64_t now_unix_ms();
[[nodiscard]] ImportItemResult failed_item(std::string path, TaskError error);
[[nodiscard]] ImportItemResult unsupported_item(std::string path, TaskError error);
[[nodiscard]] std::string lower_ascii(std::string value);
[[nodiscard]] std::string extension_lower(const std::filesystem::path &path);
[[nodiscard]] bool is_raw_extension(const std::filesystem::path &path);
[[nodiscard]] bool is_jpeg_extension(const std::filesystem::path &path);
[[nodiscard]] Result<std::optional<std::string>> adjacent_jpeg(std::string_view source);
[[nodiscard]] bool is_import_candidate(const std::filesystem::path &path);
[[nodiscard]] Result<std::vector<std::string>>
collect_import_paths(const std::vector<std::string> &inputs, const CancellationToken &cancellation,
                     bool recursive = true);
[[nodiscard]] std::string fnv1a64_hex(std::string_view text);
[[nodiscard]] Recipe identity_recipe_for(const AssetRecord &asset, const std::string &path);
[[nodiscard]] DevelopParams baseline_develop_for(const AssetRecord &asset);
[[nodiscard]] Result<Recipe> baseline_recipe_for(const AssetRecord &asset, const std::string &path);
[[nodiscard]] bool matches_develop_baseline(const AssetRecord &asset, DevelopParams params);
[[nodiscard]] std::string parameter_key_part(const ParameterValue &value);
[[nodiscard]] std::string input_color_preprocess_key(const Recipe &recipe);
[[nodiscard]] std::string raw_preprocess_key(const Recipe &recipe);
void disable_raw_preprocess(Recipe &recipe);
[[nodiscard]] std::filesystem::path utf8_path(std::string_view text);
[[nodiscard]] bool is_disk_full(const std::error_code &error) noexcept;
[[nodiscard]] Result<void> write_bytes_atomically(std::string_view dest_utf8,
                                                  const std::vector<std::uint8_t> &bytes,
                                                  const CancellationToken &cancellation);
[[nodiscard]] Result<void> write_bytes_atomically(std::string_view dest_utf8,
                                                  const std::vector<std::uint8_t> &bytes,
                                                  const CancellationToken &cancellation,
                                                  EncodedPublicationCheckpointHook checkpoint_hook);
[[nodiscard]] Result<std::uint64_t> copy_file_atomically(std::string_view source_utf8,
                                                         std::string_view dest_utf8,
                                                         const CancellationToken &cancellation);
[[nodiscard]] Result<std::uint64_t>
copy_file_atomically(std::string_view source_utf8, std::string_view dest_utf8,
                     const CancellationToken &cancellation,
                     OriginalCopyCheckpointHook checkpoint_hook);
[[nodiscard]] Result<std::uint64_t> verify_files_identical(std::string_view source_utf8,
                                                           std::string_view copy_utf8,
                                                           const CancellationToken &cancellation);

} // namespace ravo
