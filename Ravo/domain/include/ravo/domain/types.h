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

inline constexpr std::int64_t kCatalogSchemaVersion = 3;
inline constexpr std::int64_t kPreviewContractVersion = 3;
inline constexpr std::uint32_t kDefaultPreviewMaxEdge = 1600;
inline constexpr std::uint32_t kThumbnailMaxEdge = 320;

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

enum class ColorLabel
{
    kNone,
    kRed,
    kYellow,
    kGreen,
    kBlue,
    kPurple,
};

enum class RatingFilterMode
{
    kAny,
    kMinimum,
    kExact,
};

enum class RejectFilter
{
    kInclude,
    kExclude,
    kOnly,
};

enum class AssetSortField
{
    kImportTime,
    kDisplayName,
    kRating,
};

enum class SortDirection
{
    kAscending,
    kDescending,
};

struct ReviewState
{
    int rating = 0;
    ColorLabel color_label = ColorLabel::kNone;
    bool rejected = false;
};

struct LibraryQuery
{
    RatingFilterMode rating_mode = RatingFilterMode::kAny;
    int rating_value = 0;
    std::vector<ColorLabel> color_labels;
    RejectFilter reject_filter = RejectFilter::kInclude;
    AssetSortField sort_field = AssetSortField::kImportTime;
    SortDirection sort_direction = SortDirection::kDescending;
    std::string folder_uri;
};

struct FolderRecord
{
    std::string uri;
    std::string display_name;
    int depth = 0;
    int asset_count = 0;
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
    ReviewState review;
    bool has_edits = false;
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
    bool ignore_edits = false;
    bool ignore_crop = false;
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
    bool original_missing = false;
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
                                                 std::uint32_t height, std::string_view fingerprint,
                                                 std::string_view edit_digest = "identity");
void fit_within_max_edge(std::uint32_t source_width, std::uint32_t source_height,
                         std::uint32_t max_edge, std::uint32_t &output_width,
                         std::uint32_t &output_height) noexcept;
[[nodiscard]] Result<void> validate_rating(int rating);
[[nodiscard]] std::string_view color_label_name(ColorLabel label) noexcept;
[[nodiscard]] Result<ColorLabel> parse_color_label(std::string_view name);
[[nodiscard]] std::string asset_display_name(const AssetRecord &asset);
[[nodiscard]] bool asset_matches_query(const AssetRecord &asset, const LibraryQuery &query);
[[nodiscard]] std::vector<AssetRecord> filter_and_sort_assets(std::vector<AssetRecord> assets,
                                                              const LibraryQuery &query);
[[nodiscard]] bool asset_in_folder(const AssetRecord &asset, std::string_view folder_uri) noexcept;
[[nodiscard]] std::vector<FolderRecord> library_folders(const std::vector<AssetRecord> &assets);

} // namespace ravo
