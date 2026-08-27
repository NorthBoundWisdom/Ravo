#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"

namespace ravo
{

inline constexpr std::int64_t kCatalogSchemaVersion = 4;
inline constexpr std::string_view kRecipeHistoryKindHistory = "history";
inline constexpr std::string_view kRecipeHistoryKindSnapshot = "snapshot";
inline constexpr std::size_t kTagMaxLength = 128;
inline constexpr std::size_t kMetadataFieldMaxLength = 4096;
inline constexpr std::int64_t kPreviewContractVersion = 7;
inline constexpr std::uint32_t kDefaultPreviewMaxEdge = 1600;
inline constexpr std::uint32_t kInteractivePreviewMaxEdge = 640;
inline constexpr std::uint32_t kThumbnailMaxEdge = 320;
inline constexpr std::string_view kEmbeddedBrowsePreviewDigest = "embedded-jpeg-orient";
inline constexpr int kDefaultJpegQuality = 95;
inline constexpr int kJpegQualityMin = 5;
inline constexpr int kJpegQualityMax = 100;
inline constexpr int kDefaultPngCompression = 5;
inline constexpr int kPngCompressionMin = 0;
inline constexpr int kPngCompressionMax = 9;
inline constexpr int kDefaultTiffCompressionLevel = 6;
inline constexpr int kTiffCompressionLevelMin = 1;
inline constexpr int kTiffCompressionLevelMax = 9;
inline constexpr int kDefaultTiffResolutionDpi = 300;
inline constexpr int kTiffResolutionDpiMin = 72;
inline constexpr int kTiffResolutionDpiMax = 9600;
inline constexpr std::size_t kExportDocumentNameMaxBytes = 16U * 1024U;

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

enum class ExportFormat
{
    kPng,
    kJpeg,
    kTiff,
    kOriginalCopy,
};

enum class JpegSubsampling : std::uint8_t
{
    kAuto = 0,
    k444 = 1,
    k440 = 2,
    k422 = 3,
    k420 = 4,
};

struct JpegExportOptions
{
    int quality = kDefaultJpegQuality;
    JpegSubsampling subsampling = JpegSubsampling::kAuto;

    [[nodiscard]] bool operator==(const JpegExportOptions &) const = default;
};

enum class PngBitDepth : std::uint8_t
{
    k8 = 8,
    k16 = 16,
};

struct PngExportOptions
{
    PngBitDepth bit_depth = PngBitDepth::k8;
    int compression = kDefaultPngCompression;

    [[nodiscard]] bool operator==(const PngExportOptions &) const = default;
};

enum class TiffSampleType : std::uint8_t
{
    kUint8 = 0,
    kUint16 = 1,
    kFloat16 = 2,
    kFloat32 = 3,
};

enum class TiffCompression : std::uint8_t
{
    kNone = 0,
    kDeflate = 1,
    kDeflatePredictor = 2,
};

struct TiffExportOptions
{
    TiffSampleType sample_type = TiffSampleType::kUint8;
    TiffCompression compression = TiffCompression::kDeflatePredictor;
    int compression_level = kDefaultTiffCompressionLevel;
    bool grayscale_if_neutral = false;
    int resolution_dpi = kDefaultTiffResolutionDpi;

    [[nodiscard]] bool operator==(const TiffExportOptions &) const = default;
};

enum class RasterPixelFormat : std::uint8_t
{
    kRgb8 = 0,
};

enum class RasterAlphaMode : std::uint8_t
{
    kOpaque = 0,
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
    std::string tag;
};

struct CaptureMetadata
{
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
    std::optional<double> iso;
    std::optional<double> aperture;
    std::optional<double> focal_length_mm;
    std::optional<double> shutter_s;
    std::optional<std::int64_t> captured_unix_s;

    [[nodiscard]] bool operator==(const CaptureMetadata &) const noexcept = default;
};

struct WritableMetadata
{
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> creator;
    std::optional<std::string> copyright;

    [[nodiscard]] bool operator==(const WritableMetadata &) const noexcept = default;
};

struct ExportMetadataSnapshot
{
    std::string destination_document_name;
    WritableMetadata writable;

    [[nodiscard]] bool operator==(const ExportMetadataSnapshot &) const noexcept = default;
};

// Tagged product-export pixels. Exactly one alternative is valid. The raster
// port borrows this value only for a synchronous encode call.
struct ExportPixelBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ColorProfileState color_profile;
    std::variant<std::vector<std::uint8_t>, std::vector<std::uint16_t>, std::vector<float>> samples;
};

struct RecipeHistoryEntry
{
    std::int64_t id = 0;
    std::string asset_id;
    std::int64_t seq = 0;
    std::string kind{kRecipeHistoryKindHistory};
    std::optional<std::string> label;
    std::string recipe_json;
    std::int64_t created_unix_ms = 0;
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
    std::vector<std::string> tags;
    CaptureMetadata capture;
    WritableMetadata metadata;
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

    [[nodiscard]] bool operator==(const PreviewRecord &) const noexcept = default;
};

struct ImportItemResult
{
    ImportItemStatus status = ImportItemStatus::kFailed;
    std::string input_path;
    std::optional<AssetRecord> asset;
    std::optional<std::string> preview_cache_path;
    std::optional<TaskError> error;
};

struct PreviewRequest
{
    std::string asset_id;
    std::uint32_t max_edge = kDefaultPreviewMaxEdge;
    std::uint64_t request_revision = 0;
    bool ignore_edits = false;
    bool ignore_crop = false;
    bool ignore_straighten = false;
    bool persist_preview_record = true;
    // Gallery/import thumbnails may use a RAW embedded JPEG. Develop, loupe, scopes,
    // export and interactive preview must leave this false so processed pixels stay
    // on the CPU RAW + Sigmoid contract.
    bool prefer_embedded_preview = false;
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
    std::vector<std::uint8_t> rgb;
    ColorProfileState color_profile;
};

struct ExportRequest
{
    std::string asset_id;
    std::string output_path;
    ExportFormat format = ExportFormat::kPng;
    JpegExportOptions jpeg_options;
    std::uint32_t max_edge = 0;
    CancellationToken cancellation{};
    std::string correlation_id;
    PngExportOptions png_options;
    TiffExportOptions tiff_options;
};

struct ExportResult
{
    std::string asset_id;
    std::string output_path;
    ExportFormat format = ExportFormat::kPng;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t bytes_written = 0;
};

struct RasterInfo
{
    std::string media_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DecodedRaster
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
    ColorProfileState color_profile;
    RasterPixelFormat pixel_format = RasterPixelFormat::kRgb8;
    RasterAlphaMode alpha_mode = RasterAlphaMode::kOpaque;
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
[[nodiscard]] std::string_view export_format_name(ExportFormat format) noexcept;
[[nodiscard]] std::string_view export_format_extension(ExportFormat format) noexcept;
[[nodiscard]] Result<ExportFormat> parse_export_format(std::string_view name);
[[nodiscard]] std::string_view jpeg_subsampling_name(JpegSubsampling subsampling) noexcept;
[[nodiscard]] Result<JpegSubsampling> parse_jpeg_subsampling(std::string_view name);
[[nodiscard]] Result<void> validate_jpeg_export_options(const JpegExportOptions &options);
[[nodiscard]] std::string_view png_bit_depth_name(PngBitDepth bit_depth) noexcept;
[[nodiscard]] Result<PngBitDepth> parse_png_bit_depth(std::string_view name);
[[nodiscard]] Result<void> validate_png_export_options(const PngExportOptions &options);
[[nodiscard]] std::string_view tiff_sample_type_name(TiffSampleType sample_type) noexcept;
[[nodiscard]] Result<TiffSampleType> parse_tiff_sample_type(std::string_view name);
[[nodiscard]] std::string_view tiff_compression_name(TiffCompression compression) noexcept;
[[nodiscard]] Result<TiffCompression> parse_tiff_compression(std::string_view name);
[[nodiscard]] Result<void> validate_tiff_export_options(const TiffExportOptions &options);
[[nodiscard]] Result<void> validate_tiff_export_metadata(const ExportMetadataSnapshot &metadata);
[[nodiscard]] Result<std::string> normalize_tag_name(std::string_view name);
[[nodiscard]] Result<std::vector<std::string>> parse_tag_list(std::string_view text);
[[nodiscard]] Result<void> validate_metadata_field(std::string_view name, std::string_view value);
[[nodiscard]] std::string asset_display_name(const AssetRecord &asset);
[[nodiscard]] bool asset_matches_query(const AssetRecord &asset, const LibraryQuery &query);
[[nodiscard]] std::vector<AssetRecord> filter_and_sort_assets(std::vector<AssetRecord> assets,
                                                              const LibraryQuery &query);
[[nodiscard]] bool asset_in_folder(const AssetRecord &asset, std::string_view folder_uri) noexcept;
[[nodiscard]] std::vector<FolderRecord> library_folders(const std::vector<AssetRecord> &assets);

} // namespace ravo
