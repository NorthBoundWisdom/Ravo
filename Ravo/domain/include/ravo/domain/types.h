#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

inline constexpr std::int64_t kCatalogSchemaVersion = 1;
inline constexpr std::int64_t kPreviewContractVersion = 1;
inline constexpr std::uint32_t kDefaultPreviewMaxEdge = 1600;

inline constexpr std::string_view kMediaTypePng = "image/png";
inline constexpr std::string_view kMediaTypeJpeg = "image/jpeg";
inline constexpr std::string_view kMediaTypeTiff = "image/tiff";
inline constexpr std::string_view kMediaTypeBmp = "image/bmp";
inline constexpr std::string_view kMediaTypeGif = "image/gif";
inline constexpr std::string_view kMediaTypeWebp = "image/webp";
inline constexpr std::string_view kMediaTypeRaw = "image/x-raw";

[[nodiscard]] inline bool is_raw_media_type(const std::string_view media_type) noexcept
{
    return media_type == kMediaTypeRaw;
}

[[nodiscard]] inline bool is_raster_media_type(const std::string_view media_type) noexcept
{
    return !media_type.empty() && media_type.starts_with("image/") &&
           !is_raw_media_type(media_type);
}
inline constexpr std::string_view kImportStateImported = "imported";
inline constexpr std::string_view kImportStateFailed = "failed";
inline constexpr std::string_view kImportStateMissing = "missing";
inline constexpr std::string_view kPreviewStateNone = "none";
inline constexpr std::string_view kPreviewStatePending = "pending";
inline constexpr std::string_view kPreviewStateReady = "ready";
inline constexpr std::string_view kPreviewStateFailed = "failed";

enum class ImportItemStatus
{
    kImported,
    kDuplicate,
    kUnsupported,
    kFailed,
};

struct CatalogSnapshot
{
    std::string catalog_id;
    std::string database_path;
    std::string cache_root;
    std::int64_t schema_version = kCatalogSchemaVersion;
    std::int64_t revision = 0;
};

struct AssetRecord
{
    std::string id;
    std::string normalized_uri;
    std::string media_type;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
    std::optional<std::string> content_fingerprint;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::string import_state{kImportStateImported};
    std::optional<std::string> error_code;
    std::optional<std::string> error_message;
    std::int64_t created_unix_ms = 0;
};

struct PreviewRecord
{
    std::string asset_id;
    std::int64_t contract_version = kPreviewContractVersion;
    std::string cache_key;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::string state{kPreviewStateNone};
    std::optional<std::string> cache_relpath;
    std::optional<std::int64_t> last_success_unix_ms;
};

struct ImportItemResult
{
    ImportItemStatus status = ImportItemStatus::kFailed;
    std::string input_path;
    std::optional<AssetRecord> asset;
    std::optional<TaskError> error;
};

struct PreviewRequest
{
    std::string asset_id;
    std::uint32_t max_edge = kDefaultPreviewMaxEdge;
    std::uint64_t request_revision = 0;
    CancellationToken cancellation{};
    std::string correlation_id;
};

struct PreviewResult
{
    std::string asset_id;
    std::uint64_t request_revision = 0;
    std::string cache_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string cache_key;
};

struct RasterInfo
{
    std::string media_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct EncodedPng
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
};

struct FileIdentity
{
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

[[nodiscard]] std::string generate_catalog_id();
[[nodiscard]] std::string generate_asset_id();
[[nodiscard]] std::string make_content_fingerprint(const FileIdentity &identity);
[[nodiscard]] std::string make_preview_cache_key(std::string_view asset_id, std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::string_view fingerprint);
void fit_within_max_edge(std::uint32_t source_width, std::uint32_t source_height,
                         std::uint32_t max_edge, std::uint32_t &output_width,
                         std::uint32_t &output_height) noexcept;

} // namespace ravo
