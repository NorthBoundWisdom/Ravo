#pragma once

#include <array>
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

inline constexpr std::int64_t kCatalogSchemaVersion = 14;
inline constexpr std::int64_t kCatalogRecoveryMinimumSchemaVersion = 6;
inline constexpr std::int64_t kRecoverySidecarSchemaVersion = 1;
inline constexpr std::int64_t kCatalogBackupFormatVersion = 2;
inline constexpr std::uintmax_t kRecoverySidecarMaximumBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kRecoveryHistoryMaximumEntries = 10'000U;
inline constexpr std::size_t kRecoveryTagMaximumEntries = 10'000U;
inline constexpr std::uintmax_t kCatalogBackupManifestMaximumBytes = 64U * 1024U * 1024U;
inline constexpr std::string_view kCatalogBackupCatalogFilename = "catalog.sqlite";
inline constexpr std::string_view kCatalogBackupManifestFilename = "manifest.json";
inline constexpr std::string_view kCatalogBackupSidecarDirectory = "sidecars";
inline constexpr std::string_view kCatalogBackupDerivedDirectory = "derived";
inline constexpr std::string_view kCatalogBackupExternalEditorDirectory = "external-editor";
inline constexpr std::int64_t kCatalogBackupFormatVersionMin = 1;
inline constexpr std::string_view kRecipeHistoryKindHistory = "history";
inline constexpr std::string_view kRecipeHistoryKindSnapshot = "snapshot";

// kAppendIfNew records an automatic history row unless it duplicates the latest
// history-kind entry. kUnchanged publishes the current recipe and leaves the
// stack intact so a later edit can still discard newer steps.
enum class RecipeHistoryWrite
{
    kAppendIfNew,
    kUnchanged,
};

struct RecipeCommitResult
{
    std::int64_t revision = 0;
    std::optional<std::int64_t> history_id;
};

inline constexpr std::size_t kTagMaxLength = 128;
inline constexpr std::size_t kKeywordMaximumDepth = 16;
inline constexpr std::size_t kKeywordMaximumCount = 50'000U;
inline constexpr std::size_t kKeywordPathMaxLength = 512;
inline constexpr char kKeywordPathSeparator = '|';

inline constexpr std::size_t kMetadataFieldMaxLength = 4096;
inline constexpr std::int64_t kPreviewContractVersion = 10;
inline constexpr std::uint32_t kDefaultPreviewMaxEdge = 1600;
inline constexpr std::uint32_t kInteractivePreviewMaxEdge = 960;
inline constexpr std::uint32_t kThumbnailMaxEdge = 320;
inline constexpr std::string_view kEmbeddedBrowsePreviewDigest = "embedded-jpeg-orient";
inline constexpr std::string_view kCompanionJpegBrowsePreviewDigest = "companion-jpeg";
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
inline constexpr std::uint32_t kExportMaxEdgeMin = 0;
inline constexpr std::uint32_t kExportMaxEdgeMax = 65535;
inline constexpr std::uint32_t kExportBoxLimitMin = 0;
inline constexpr std::uint32_t kExportBoxLimitMax = 65535;
inline constexpr double kExportOutputSharpenAmountMin = 0.0;
inline constexpr double kExportOutputSharpenAmountMax = 2.0;
inline constexpr double kExportOutputSharpenRadiusMin = 0.0;
inline constexpr double kExportOutputSharpenRadiusMax = 5.0;
inline constexpr double kExportOutputSharpenThresholdMin = 0.0;
inline constexpr double kExportOutputSharpenThresholdMax = 100.0;
inline constexpr double kExportWatermarkOpacityMin = 0.0;
inline constexpr double kExportWatermarkOpacityMax = 1.0;
inline constexpr double kExportWatermarkScaleMin = 0.5;
inline constexpr double kExportWatermarkScaleMax = 50.0;
inline constexpr double kExportWatermarkOffsetMin = -1.0;
inline constexpr double kExportWatermarkOffsetMax = 1.0;
inline constexpr double kExportWatermarkRotationMin = -180.0;
inline constexpr double kExportWatermarkRotationMax = 180.0;
inline constexpr std::size_t kExportWatermarkTextMaxBytes = 256U;
inline constexpr std::int64_t kExportPresetSchemaVersion = 1;
inline constexpr std::string_view kExportPresetSchema = "ravo.export_preset";
inline constexpr std::size_t kExportPresetFileMaxBytes = 256U * 1024U;
inline constexpr std::int64_t kExportJobSchemaVersion = 1;
inline constexpr std::string_view kExportJobSchema = "ravo.export_job";
inline constexpr std::size_t kExportJobFileMaxBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kExportBatchMaxAssets = 10'000U;
inline constexpr std::size_t kExportFilenameTemplateMaxBytes = 512U;
inline constexpr std::size_t kExportFilenameMaxBytes = 240U;
inline constexpr std::size_t kLibraryPageDefaultSize = 200U;
inline constexpr std::size_t kLibraryPageMaximumSize = 512U;
inline constexpr std::int64_t kLibraryQueryDocumentSchemaVersion = 4;
inline constexpr std::int64_t kLibraryQueryDocumentSchemaVersionMin = 1;
inline constexpr std::size_t kLibraryFacetMaximumValues = 2048U;
inline constexpr std::size_t kLibrarySetNameMaxLength = 128;
inline constexpr std::size_t kLibrarySetMaximumCount = 1'000U;
inline constexpr std::string_view kLibrarySetKindManual = "manual";
inline constexpr std::string_view kLibrarySetKindSmart = "smart";
inline constexpr int kAssetVersionOrdinalPrimary = 0;
inline constexpr int kAssetVersionMaximum = 64;
inline constexpr std::size_t kLibraryStackMaximumCount = 10'000U;
inline constexpr std::size_t kLibraryStackMaximumMembers = 64U;
inline constexpr int kSurveySlotMinimum = 2;
inline constexpr int kSurveySlotMaximum = 4;
inline constexpr std::size_t kImportBatchMaximumAssets = 100'000U;
inline constexpr std::size_t kImportFilenameTemplateMaxBytes = 512U;
inline constexpr std::size_t kImportFilenameMaxBytes = 240U;
inline constexpr std::int64_t kBackupScheduleIntervalMinutesMin = 15;
inline constexpr std::int64_t kBackupScheduleIntervalMinutesMax = 365 * 24 * 60;
inline constexpr int kBackupRetentionCountMin = 1;
inline constexpr int kBackupRetentionCountMax = 100;
inline constexpr std::size_t kExportDocumentNameMaxBytes = 16U * 1024U;
inline constexpr std::size_t kExportCaptureFieldMaxLength = kMetadataFieldMaxLength;
inline constexpr std::size_t kJpegAppMarkerMaxPayloadBytes = 65533U;
inline constexpr std::size_t kJpegExifApp1IdentifierBytes = 6U;
inline constexpr std::size_t kJpegXmpApp1IdentifierBytes = 29U;
inline constexpr std::size_t kJpegPhotoshopApp13IdentifierBytes = 14U;
inline constexpr std::size_t kJpegPhotoshopIrbHeaderBytes = 12U;
inline constexpr std::size_t kExportExifTiffProfileMaxBytes =
    kJpegAppMarkerMaxPayloadBytes - kJpegExifApp1IdentifierBytes;
inline constexpr std::size_t kExportXmpPacketMaxBytes =
    kJpegAppMarkerMaxPayloadBytes - kJpegXmpApp1IdentifierBytes;
inline constexpr std::size_t kExportIptcIimMaxBytes = kJpegAppMarkerMaxPayloadBytes -
                                                      kJpegPhotoshopApp13IdentifierBytes -
                                                      kJpegPhotoshopIrbHeaderBytes - 1U;
inline constexpr std::size_t kExportIptcTitleMaxBytes = 64U;
inline constexpr std::size_t kExportIptcDescriptionMaxBytes = 2000U;
inline constexpr std::size_t kExportIptcCreatorMaxBytes = 32U;
inline constexpr std::size_t kExportIptcCopyrightMaxBytes = 128U;
inline constexpr std::size_t kExportIptcKeywordMaxBytes = 64U;
inline constexpr std::size_t kExportIptcCityMaxBytes = 32U;
inline constexpr std::size_t kExportIptcSublocationMaxBytes = 32U;
inline constexpr std::size_t kExportIptcProvinceStateMaxBytes = 32U;
inline constexpr std::size_t kExportIptcCountryMaxBytes = 64U;
// Even a one-byte tag needs 24 bytes in the canonical dc:subject item. Reject
// impossible counts before copying or sorting the snapshot.
inline constexpr std::size_t kExportTagMaxCount = kExportXmpPacketMaxBytes / 24U;
inline constexpr std::string_view kExportXmpCreatorTool = "Ravo";

inline constexpr std::int32_t kCaptureLatitudeE6Min = -90'000'000;
inline constexpr std::int32_t kCaptureLatitudeE6Max = 90'000'000;
inline constexpr std::int32_t kCaptureLongitudeE6Min = -180'000'000;
inline constexpr std::int32_t kCaptureLongitudeE6Max = 180'000'000;
inline constexpr std::uint32_t kCaptureAltitudeAboveSeaLevelMmMax = 100'000'000U;
inline constexpr std::uint32_t kCaptureAltitudeBelowSeaLevelMmMax = 12'000'000U;
inline constexpr std::int32_t kCaptureUtcOffsetMinutesMin = -14 * 60;
inline constexpr std::int32_t kCaptureUtcOffsetMinutesMax = 14 * 60;
inline constexpr std::size_t kCaptureLocalExifLength = 19U;
inline constexpr std::size_t kCaptureSubsecondDigitsMin = 1U;
inline constexpr std::size_t kCaptureSubsecondDigitsMax = 9U;

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

enum class ImportTransferMode
{
    kAdd,
    kCopy,
    kMove,
};

enum class ImportOrganization
{
    kSingleFolder,
    kPreserveHierarchy,
    kCaptureDate,
    kCaptureMonth,
};

enum class ImportPreviewPolicy
{
    kMinimal,
    kStandard,
    kOneToOne,
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
    kCaptureTime,
    kDisplayName,
    kRating,
    kFileSize,
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

enum class ExportMetadataMode : std::uint8_t
{
    kFull = 0,
    kNoLocation = 1,
    kNone = 2,
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

enum class EditFilter
{
    kAny,
    kEdited,
    kUnedited,
};

struct LibraryNumericRange
{
    std::optional<double> minimum;
    std::optional<double> maximum;

    [[nodiscard]] bool operator==(const LibraryNumericRange &) const noexcept = default;
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
    std::string text;
    std::vector<std::string> media_types;
    EditFilter edit_filter = EditFilter::kAny;
    std::string camera;
    LibraryNumericRange iso;
    LibraryNumericRange aperture;
    LibraryNumericRange focal_length_mm;
    LibraryNumericRange shutter_s;
    LibraryNumericRange aspect_ratio;
    std::optional<std::int64_t> imported_after_unix_ms;
    std::optional<std::int64_t> imported_before_unix_ms;
    std::optional<std::int64_t> captured_after_unix_s;
    std::optional<std::int64_t> captured_before_unix_s;
    // ADR-0128 exact facet selectors (empty string = absent capture field).
    std::optional<std::string> camera_make_equals;
    std::optional<std::string> camera_model_equals;
    std::optional<double> focal_length_mm_equals;
    // ADR-0135 Exif lens-name facet pair (empty string = absent capture field).
    std::optional<std::string> lens_make_equals;
    std::optional<std::string> lens_model_equals;
    std::optional<std::string> captured_local_date; // YYYY:MM:DD from captured_local_exif
    // ADR-0130 exact location selectors (empty string = absent writable field).
    std::optional<std::string> country_equals;
    std::optional<std::string> province_state_equals;
    std::optional<std::string> city_equals;
    std::optional<std::string> sublocation_equals;
    std::string collection_id;

    [[nodiscard]] bool operator==(const LibraryQuery &) const noexcept = default;
};

struct LibraryFacetEntry
{
    std::string key;
    std::string label;
    std::size_t count = 0U;
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
    std::optional<double> focal_length_mm;
    std::optional<std::string> lens_make;
    std::optional<std::string> lens_model;
    std::optional<std::string> captured_local_date;

    [[nodiscard]] bool operator==(const LibraryFacetEntry &) const noexcept = default;
};

struct LibraryCaptureFacets
{
    std::vector<LibraryFacetEntry> cameras;
    std::vector<LibraryFacetEntry> lenses;     // ADR-0128 focal-length proxy
    std::vector<LibraryFacetEntry> lens_names; // ADR-0135 Exif LensMake/LensModel
    std::vector<LibraryFacetEntry> capture_dates;
    bool truncated = false;
    // True when counts were computed inside an explicit LibraryQuery scope
    // instead of the whole catalog.
    bool scoped = false;

    [[nodiscard]] bool operator==(const LibraryCaptureFacets &) const noexcept = default;
};

struct LibraryLocationFacets
{
    std::vector<LibraryFacetEntry> countries;
    std::vector<LibraryFacetEntry> province_states;
    std::vector<LibraryFacetEntry> cities;
    std::vector<LibraryFacetEntry> sublocations;
    bool truncated = false;
    bool scoped = false;

    [[nodiscard]] bool operator==(const LibraryLocationFacets &) const noexcept = default;
};

struct LibraryPageRequest
{
    LibraryQuery query;
    std::optional<LibraryQuery> additional_query;
    bool collapse_stacks = true;
    std::size_t offset = 0U;
    std::size_t limit = kLibraryPageDefaultSize;
    std::optional<std::string> after_asset_id;
    std::optional<std::size_t> known_total;
};

enum class LibrarySetKind : std::uint8_t
{
    kManual = 0,
    kSmart = 1,
};

struct LibrarySetRecord
{
    std::string id;
    LibrarySetKind kind = LibrarySetKind::kManual;
    std::string name;
    std::optional<LibraryQuery> query;
    std::int64_t created_unix_ms = 0;
    std::int64_t updated_unix_ms = 0;
    std::size_t asset_count = 0U;
};

struct LibrarySetMutation
{
    LibrarySetRecord set;
    std::int64_t revision = 0;
};

struct CaptureDateTime
{
    std::string local_exif;
    std::optional<std::string> subsecond_digits;
    std::optional<std::int32_t> utc_offset_minutes;

    [[nodiscard]] bool operator==(const CaptureDateTime &) const noexcept = default;
};

enum class CaptureAltitudeReference : std::uint8_t
{
    kAboveSeaLevel = 0,
    kBelowSeaLevel = 1,
};

struct CaptureAltitude
{
    std::uint32_t magnitude_mm = 0;
    CaptureAltitudeReference reference = CaptureAltitudeReference::kAboveSeaLevel;

    [[nodiscard]] bool operator==(const CaptureAltitude &) const noexcept = default;
};

struct CaptureLocation
{
    std::int32_t latitude_e6 = 0;
    std::int32_t longitude_e6 = 0;
    std::optional<CaptureAltitude> altitude;

    [[nodiscard]] bool operator==(const CaptureLocation &) const noexcept = default;
};

struct CaptureMetadata
{
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
    std::optional<std::string> lens_make;
    std::optional<std::string> lens_model;
    std::optional<double> iso;
    std::optional<double> aperture;
    std::optional<double> focal_length_mm;
    std::optional<double> shutter_s;
    std::optional<std::int64_t> captured_unix_s;
    std::optional<CaptureDateTime> captured_datetime;
    std::optional<CaptureLocation> location;

    [[nodiscard]] bool operator==(const CaptureMetadata &) const noexcept = default;
};

struct WritableMetadata
{
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> creator;
    std::optional<std::string> copyright;
    // ADR-0126 catalog-owned IPTC location Core quartet (not capture GPS).
    std::optional<std::string> country;
    std::optional<std::string> province_state;
    std::optional<std::string> city;
    std::optional<std::string> sublocation;

    [[nodiscard]] bool operator==(const WritableMetadata &) const noexcept = default;
};

// Field patch for multi-select IPTC Core / location edits (ADR-0124/0126).
// Flags mark which fields to write; optional values use absent = clear the field.
struct WritableMetadataPatch
{
    bool update_title = false;
    std::optional<std::string> title;
    bool update_description = false;
    std::optional<std::string> description;
    bool update_creator = false;
    std::optional<std::string> creator;
    bool update_copyright = false;
    std::optional<std::string> copyright;
    bool update_country = false;
    std::optional<std::string> country;
    bool update_province_state = false;
    std::optional<std::string> province_state;
    bool update_city = false;
    std::optional<std::string> city;
    bool update_sublocation = false;
    std::optional<std::string> sublocation;

    [[nodiscard]] bool empty() const noexcept
    {
        return !update_title && !update_description && !update_creator && !update_copyright &&
               !update_country && !update_province_state && !update_city && !update_sublocation;
    }
};

struct ExportUnsignedRational
{
    std::uint32_t numerator = 0U;
    std::uint32_t denominator = 1U;

    [[nodiscard]] bool operator==(const ExportUnsignedRational &) const noexcept = default;
};

struct ExportMetadataSnapshot
{
    std::string destination_document_name;
    WritableMetadata writable;
    CaptureMetadata capture;
    std::vector<std::string> tags;
    bool embed_metadata = true;

    [[nodiscard]] bool operator==(const ExportMetadataSnapshot &) const noexcept = default;
};

struct ExportMetadataPacketSizes
{
    std::size_t exif_tiff_profile_bytes = 0U;
    std::size_t xmp_packet_bytes = 0U;
    std::size_t iptc_iim_bytes = 0U;
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
    std::string id;
    std::string uri;
    std::string display_name;
    int depth = 0;
    int asset_count = 0;
    bool missing = false;
};

struct FolderAssetCount
{
    std::string id;
    std::string uri;
    int direct_asset_count = 0;
    bool missing = false;
};

struct FolderRelinkAsset
{
    std::string asset_id;
    std::string expected_old_uri;
    std::string replacement_uri;
};

struct FolderRelinkCommit
{
    std::string folder_id;
    std::string expected_old_uri;
    std::string replacement_uri;
    std::vector<FolderRelinkAsset> assets;
};

struct FolderRelinkResult
{
    std::string folder_id;
    std::string previous_uri;
    std::string replacement_uri;
    std::size_t asset_count = 0U;
    std::size_t recovery_pending = 0U;
};

struct FolderRemoveResult
{
    std::string folder_uri;
    std::size_t asset_count = 0U;
};

struct CatalogSnapshot
{
    std::string catalog_id;
    std::string database_path;
    std::string cache_root;
    std::int64_t schema_version = kCatalogSchemaVersion;
    // Live SQLite value; snapshot() re-reads it so other connections are visible.
    std::int64_t revision = 0;
};

struct KeywordRecord
{
    std::string id;
    std::optional<std::string> parent_id;
    std::string name;
    std::string path;
    int depth = 0;
    std::int64_t created_unix_ms = 0;
    std::int64_t updated_unix_ms = 0;

    [[nodiscard]] bool operator==(const KeywordRecord &) const noexcept = default;
};

struct KeywordMutation
{
    KeywordRecord keyword;
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
    int version_ordinal = kAssetVersionOrdinalPrimary;
    std::optional<std::string> source_asset_id;
    std::optional<std::string> stack_id;
    int stack_position = 0;
    int stack_count = 0;
    bool stack_pick = false;
};

struct AssetVersionMutation
{
    AssetRecord version;
    std::int64_t revision = 0;
};

struct KeywordMembershipMutation
{
    std::vector<AssetRecord> assets;
    std::int64_t revision = 0;
};

struct WritableMetadataMutation
{
    std::vector<AssetRecord> assets;
    std::int64_t revision = 0;
};

struct LibraryStackRecord
{
    std::string id;
    std::string pick_asset_id;
    std::vector<std::string> member_ids;
    std::int64_t created_unix_ms = 0;
};

struct LibraryStackMutation
{
    LibraryStackRecord stack;
    std::int64_t revision = 0;
};

struct LibraryPage
{
    std::vector<AssetRecord> assets;
    std::size_t offset = 0U;
    std::size_t total = 0U;
    bool has_more = false;
    std::optional<std::string> next_cursor;
    std::int64_t query_elapsed_us = 0;
    std::size_t materialized_rows = 0U;
};

// The catalog transaction owns generation. Filesystem publication acknowledges
// only the exact generation it serialized, so a concurrent newer commit remains
// pending instead of being hidden by a stale sidecar write.
struct AssetRecoveryState
{
    std::string asset_id;
    std::int64_t generation = 0;
    std::int64_t synchronized_generation = 0;

    [[nodiscard]] bool pending() const noexcept
    {
        return generation > synchronized_generation;
    }

    [[nodiscard]] bool operator==(const AssetRecoveryState &) const noexcept = default;
};

struct AssetRecoverySnapshot
{
    std::string catalog_id;
    std::int64_t catalog_revision = 0;
    AssetRecoveryState state;
    AssetRecord asset;
    std::optional<std::string> recipe_json;
    std::vector<RecipeHistoryEntry> history;
};

struct RecoveryArtifact
{
    std::string asset_id;
    std::int64_t generation = 0;
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
};

struct RecoverySyncResult
{
    std::string root;
    std::vector<RecoveryArtifact> artifacts;
    std::size_t pending_before = 0;
    std::size_t pending_after = 0;
};

struct CatalogDatabaseArtifact
{
    std::string path;
    std::string catalog_id;
    std::int64_t schema_version = 0;
    std::int64_t revision = 0;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::vector<AssetRecoveryState> recovery_states;
};

struct CatalogBackupTreeFile
{
    std::string relative_path;
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
};

struct CatalogBackupArtifact
{
    std::string path;
    std::string manifest_path;
    CatalogDatabaseArtifact catalog;
    std::int64_t format_version = kCatalogBackupFormatVersion;
    std::int64_t created_unix_ms = 0;
    std::size_t sidecar_count = 0;
    std::uint64_t sidecar_bytes = 0;
    std::size_t derived_count = 0;
    std::uint64_t derived_bytes = 0;
    std::size_t external_editor_count = 0;
    std::uint64_t external_editor_bytes = 0;
};

struct CatalogBackupVerification
{
    CatalogBackupArtifact artifact;
    bool originals_included = false;
    bool previews_included = false;
};

struct CatalogBackupPolicy
{
    bool enabled = false;
    std::string destination_directory;
    std::int64_t interval_minutes = 24 * 60;
    int retention_count = 7;
    std::optional<std::int64_t> last_success_unix_ms;
    std::optional<std::int64_t> next_run_unix_ms;
    std::uint64_t last_backup_bytes = 0U;
    std::optional<std::string> last_error;
};

struct CatalogBackupScheduleResult
{
    bool ran = false;
    CatalogBackupPolicy policy;
    std::optional<CatalogBackupArtifact> backup;
    std::vector<std::string> removed_backups;
    std::vector<std::string> retained_unverified_paths;
};

enum class CatalogRestoreStage : std::uint8_t
{
    kVerifySource,
    kStageDatabase,
    kStageSidecars,
    kVerifyStaging,
    kPublishSupport,
    kPublishCatalog,
    kOpenCatalog,
    kComplete,
};

struct CatalogRestoreRequest
{
    std::string backup_directory;
    std::string destination_catalog;
    CancellationToken cancellation{};
    std::string correlation_id;
};

struct CatalogRestoreProgress
{
    CatalogRestoreStage stage = CatalogRestoreStage::kVerifySource;
    std::size_t completed = 0U;
    std::size_t total = 0U;
    std::uint64_t bytes_completed = 0U;
};

struct CatalogRestoreResult
{
    CatalogBackupArtifact source_backup;
    CatalogSnapshot catalog;
    std::string support_root;
    bool previews_rebuild_required = true;
    bool published = false;
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
    std::optional<std::string> destination_path;
    std::optional<std::string> sidecar_destination_path;
    std::optional<std::string> jpeg_companion_destination_path;
    std::optional<std::string> second_copy_destination_path;
    std::optional<std::string> second_copy_sidecar_destination_path;
    std::optional<std::string> second_copy_jpeg_companion_destination_path;
    std::optional<TaskError> source_cleanup_error;
    bool copies_verified = false;
    bool preview_pending = false;
};

struct ImportRequest
{
    std::vector<std::string> inputs;
    ImportTransferMode mode = ImportTransferMode::kAdd;
    ImportOrganization organization = ImportOrganization::kSingleFolder;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kStandard;
    std::string destination_directory;
    std::string filename_template;
    std::string second_copy_directory;
    std::optional<std::string> source_root;
    bool recursive = true;
    bool include_xmp_sidecars = true;
    bool defer_previews = false;
    CancellationToken cancellation{};
};

struct ImportBatchResult
{
    ImportTransferMode mode = ImportTransferMode::kAdd;
    ImportPreviewPolicy preview = ImportPreviewPolicy::kStandard;
    std::vector<ImportItemResult> items;
    std::size_t imported = 0U;
    std::size_t duplicates = 0U;
    std::size_t unsupported = 0U;
    std::size_t failed = 0U;
    std::size_t source_cleanup_failed = 0U;
    std::size_t verified_second_copies = 0U;
};

struct ImportCandidate
{
    std::string source_path;
    std::string relative_path;
    std::string display_name;
    std::string media_type;
    std::uint64_t size_bytes = 0U;
    std::int64_t mtime_unix_ms = 0;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::int64_t> captured_unix_s;
    std::optional<std::string> captured_date_path;
    bool duplicate = false;
    bool supported = true;
    std::optional<TaskError> error;
};

enum class PreviewPurpose
{
    kDevelop,
    kBrowse,
};

// Normalized rectangle in the current cropped, display-oriented photo.
struct PreviewNormRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct PreviewRequest
{
    std::string asset_id;
    std::uint32_t max_edge = kDefaultPreviewMaxEdge;
    std::uint64_t request_revision = 0;
    PreviewPurpose purpose = PreviewPurpose::kDevelop;
    bool ignore_edits = false;
    bool ignore_crop = false;
    bool ignore_straighten = false;
    bool persist_preview_record = true;
    std::optional<PreviewNormRect> roi;
    // Browse requests may use a RAW embedded JPEG. Develop, loupe, scopes, export and
    // interactive preview must leave this false so processed pixels stay on the CPU
    // RAW + Sigmoid contract. Purpose independently owns cache/scheduling isolation.
    bool prefer_embedded_preview = false;
    CancellationToken cancellation{};
    std::string correlation_id;
    std::optional<std::string> overlay_mask_id;
    // CLI, PNG cache, gold tests, and overlay compositing keep true. Studio
    // interactive GPU display sets false so the Engine can skip CPU readback.
    bool need_cpu_pixels = true;
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
    std::vector<float> mask_alpha;
    std::string gpu_backend;
    std::uint64_t gpu_display_generation = 0;
    std::uint32_t gpu_display_width = 0;
    std::uint32_t gpu_display_height = 0;
    std::uint64_t gpu_display_native_surface = 0;
};

struct PreviewRebuildItemResult
{
    std::string asset_id;
    std::optional<std::string> browse_cache_path;
    std::optional<std::string> develop_cache_path;
    std::optional<TaskError> error;
};

struct PreviewRebuildResult
{
    std::size_t total = 0U;
    std::size_t completed = 0U;
    std::size_t succeeded = 0U;
    std::size_t failed = 0U;
    std::vector<PreviewRebuildItemResult> items;
};

struct ExportOutputSharpenOptions
{
    bool enabled = false;
    double amount = 0.5;
    double radius = 0.5;
    double threshold = 0.0;

    [[nodiscard]] bool operator==(const ExportOutputSharpenOptions &) const noexcept = default;
};

struct ExportWatermarkOptions
{
    bool enabled = false;
    std::string text = "RAVO";
    std::array<double, 3> color{1.0, 1.0, 1.0};
    double opacity = 0.5;
    double scale_percent = 8.0;
    double x_offset = 0.0;
    double y_offset = 0.0;
    std::string alignment = "bottom_right";
    double rotation_degrees = 0.0;

    [[nodiscard]] bool operator==(const ExportWatermarkOptions &) const noexcept = default;
};

// ADR-0129: delivery colour override (ADR-0019 params; proof forced off on apply).
struct ExportColorOptions
{
    bool enabled = false;
    std::string output_profile = "srgb";
    std::string output_profile_filename;
    std::string rendering_intent = "perceptual";
    bool black_point_compensation = true;

    [[nodiscard]] bool operator==(const ExportColorOptions &) const noexcept = default;
};

// ADR-0129: delivery frame (ADR-0070 FrameParams fields as ExportOptions).
struct ExportFrameOptions
{
    bool enabled = false;
    std::array<double, 3> border_color{1.0, 1.0, 1.0};
    double aspect = -1.0;
    std::string orientation = "auto";
    double size = 0.1;
    double position_h = 0.5;
    double position_v = 0.5;
    double frame_size = 0.0;
    double frame_offset = 0.5;
    std::array<double, 3> frame_color{0.0, 0.0, 0.0};
    std::string basis = "auto";

    [[nodiscard]] bool operator==(const ExportFrameOptions &) const noexcept = default;
};

struct ExportOptions
{
    ExportFormat format = ExportFormat::kPng;
    JpegExportOptions jpeg_options;
    std::uint32_t max_edge = 0;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
    ExportOutputSharpenOptions output_sharpen;
    ExportColorOptions output_color;
    ExportFrameOptions frame;
    ExportWatermarkOptions watermark;
    PngExportOptions png_options;
    TiffExportOptions tiff_options;
    ExportMetadataMode metadata_mode = ExportMetadataMode::kFull;

    [[nodiscard]] bool operator==(const ExportOptions &) const noexcept = default;
};

struct ExportPreset
{
    std::int64_t schema_version = kExportPresetSchemaVersion;
    ExportOptions options;
};

enum class ExportJobItemStatus : std::uint8_t
{
    kPending = 0,
    kDelivered = 1,
    kFailed = 2,
};

struct ExportJobItem
{
    std::string asset_id;
    ExportJobItemStatus status = ExportJobItemStatus::kPending;
    std::string output_path;
    std::optional<std::string> error_reason;
    std::optional<std::string> error_message;
};

struct ExportJob
{
    std::int64_t schema_version = kExportJobSchemaVersion;
    std::string job_id;
    std::vector<std::string> asset_ids;
    ExportOptions options;
    std::string output_directory;
    std::string filename_template{"{stem}-{sequence}{ext}"};
    std::vector<ExportJobItem> items;
};

struct ExportRequest : ExportOptions
{
    std::string asset_id;
    std::string output_path;
    CancellationToken cancellation{};
    std::string correlation_id;
};

struct ExportBatchRequest
{
    std::vector<std::string> asset_ids;
    std::string output_directory;
    std::string filename_template{"{stem}-{sequence}{ext}"};
    ExportOptions options;
    CancellationToken cancellation{};
    std::string correlation_id;
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
    // Oriented source dimensions before optional proportional preview decode.
    // They preserve the density proof required by engine working buffers.
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
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
[[nodiscard]] std::string generate_folder_id();
[[nodiscard]] std::string generate_library_set_id();
[[nodiscard]] std::string generate_library_stack_id();
[[nodiscard]] std::string generate_keyword_id();
[[nodiscard]] std::string generate_ai_proposal_id();
[[nodiscard]] std::string generate_external_editor_open_intent_id();
[[nodiscard]] Result<std::string> normalize_keyword_name(std::string_view name);
[[nodiscard]] Result<std::vector<std::string>> parse_keyword_path(std::string_view path);
[[nodiscard]] Result<std::string> join_keyword_path(const std::vector<std::string> &segments);

[[nodiscard]] std::string_view catalog_restore_stage_name(CatalogRestoreStage stage) noexcept;
[[nodiscard]] std::string make_content_fingerprint(const FileIdentity &identity);
[[nodiscard]] std::string make_preview_cache_key(std::string_view asset_id, std::uint32_t width,
                                                 std::uint32_t height, std::string_view fingerprint,
                                                 std::string_view edit_digest = "identity");
void fit_within_max_edge(std::uint32_t source_width, std::uint32_t source_height,
                         std::uint32_t max_edge, std::uint32_t &output_width,
                         std::uint32_t &output_height) noexcept;
void fit_within_box(std::uint32_t source_width, std::uint32_t source_height,
                    std::uint32_t max_width, std::uint32_t max_height, std::uint32_t &output_width,
                    std::uint32_t &output_height) noexcept;
void fit_export_output_size(std::uint32_t source_width, std::uint32_t source_height,
                            std::uint32_t max_edge, std::uint32_t max_width,
                            std::uint32_t max_height, std::uint32_t &output_width,
                            std::uint32_t &output_height) noexcept;
[[nodiscard]] bool export_options_request_resize(const ExportOptions &options) noexcept;
[[nodiscard]] bool export_options_request_output_sharpen(const ExportOptions &options) noexcept;
[[nodiscard]] bool export_options_request_output_color(const ExportOptions &options) noexcept;
[[nodiscard]] bool export_options_request_frame(const ExportOptions &options) noexcept;
[[nodiscard]] bool export_options_request_watermark(const ExportOptions &options) noexcept;
[[nodiscard]] Result<void> validate_export_watermark_alignment(std::string_view alignment);
[[nodiscard]] Result<void>
validate_export_output_sharpen_options(const ExportOutputSharpenOptions &options);
[[nodiscard]] Result<void> validate_export_watermark_options(const ExportWatermarkOptions &options);
[[nodiscard]] Result<void> validate_export_color_options(const ExportColorOptions &options);
[[nodiscard]] Result<void> validate_export_frame_options(const ExportFrameOptions &options);
[[nodiscard]] Result<void> validate_export_options(const ExportOptions &options);
[[nodiscard]] Result<ExportPreset> parse_export_preset_json(std::string_view text);
[[nodiscard]] Result<std::string> serialize_export_preset(const ExportPreset &preset);
[[nodiscard]] Result<ExportOptions> apply_export_preset(const ExportPreset &preset);
[[nodiscard]] Result<ExportJob> parse_export_job_json(std::string_view text);
[[nodiscard]] Result<std::string> serialize_export_job(const ExportJob &job);
[[nodiscard]] std::string_view export_job_item_status_name(ExportJobItemStatus status) noexcept;
[[nodiscard]] Result<ExportJobItemStatus> parse_export_job_item_status(std::string_view name);
[[nodiscard]] Result<void> validate_rating(int rating);
[[nodiscard]] Result<void> validate_catalog_backup_policy(const CatalogBackupPolicy &policy);
[[nodiscard]] std::string_view color_label_name(ColorLabel label) noexcept;
[[nodiscard]] Result<ColorLabel> parse_color_label(std::string_view name);
[[nodiscard]] std::string_view export_format_name(ExportFormat format) noexcept;
[[nodiscard]] std::string_view export_format_extension(ExportFormat format) noexcept;
[[nodiscard]] Result<ExportFormat> parse_export_format(std::string_view name);
[[nodiscard]] std::string_view export_metadata_mode_name(ExportMetadataMode mode) noexcept;
[[nodiscard]] Result<ExportMetadataMode> parse_export_metadata_mode(std::string_view name);
[[nodiscard]] Result<std::string>
expand_export_filename_template(std::string_view filename_template, std::string_view source_stem,
                                std::string_view asset_id, std::size_t sequence,
                                std::string_view extension);
[[nodiscard]] Result<std::string>
expand_import_filename_template(std::string_view filename_template, std::string_view source_stem,
                                std::string_view capture_date, std::size_t sequence,
                                std::string_view extension);
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
[[nodiscard]] Result<void> validate_tiff_export_document_name(std::string_view name);

[[nodiscard]] Result<void> validate_capture_datetime(const CaptureDateTime &value);
[[nodiscard]] Result<void> validate_capture_location(const CaptureLocation &value);
[[nodiscard]] Result<void> validate_capture_metadata(const CaptureMetadata &capture);
[[nodiscard]] bool capture_metadata_has_values(const CaptureMetadata &capture) noexcept;
[[nodiscard]] std::array<ExportUnsignedRational, 3>
capture_microdegrees_to_dms(std::int32_t e6) noexcept;
[[nodiscard]] ExportUnsignedRational
capture_altitude_mm_to_rational(std::uint32_t magnitude_mm) noexcept;
[[nodiscard]] std::string format_capture_datetime_iso(const CaptureDateTime &value);
[[nodiscard]] std::string format_capture_utc_offset(std::int32_t utc_offset_minutes);
[[nodiscard]] std::string format_scaled_decimal(std::int64_t value, int scale);
[[nodiscard]] std::string format_gps_xmp_coordinate(std::int32_t e6, char positive_ref,
                                                    char negative_ref);
[[nodiscard]] Result<void> validate_export_metadata(const ExportMetadataSnapshot &metadata,
                                                    const CancellationToken &cancellation = {});
[[nodiscard]] Result<void>
validate_tiff_export_metadata(const ExportMetadataSnapshot &metadata,
                              const CancellationToken &cancellation = {});
[[nodiscard]] Result<std::vector<std::string>>
canonicalize_export_tags(const std::vector<std::string> &tags,
                         const CancellationToken &cancellation = {});
[[nodiscard]] Result<std::uint16_t> export_photographic_sensitivity(double iso);
[[nodiscard]] Result<ExportUnsignedRational> export_positive_rational(double value);
[[nodiscard]] std::string export_rational_xmp_text(ExportUnsignedRational value);
[[nodiscard]] bool export_color_space_is_srgb(const ColorProfileState &profile) noexcept;
[[nodiscard]] bool export_iptc_should_omit(const ExportMetadataSnapshot &metadata) noexcept;
[[nodiscard]] std::size_t xml_escaped_utf8_size(std::string_view text) noexcept;
[[nodiscard]] Result<ExportMetadataPacketSizes>
estimate_export_metadata_packets(const ExportMetadataSnapshot &metadata);
[[nodiscard]] Result<std::string> normalize_tag_name(std::string_view name);
[[nodiscard]] Result<std::vector<std::string>> parse_tag_list(std::string_view text);
[[nodiscard]] Result<void> validate_metadata_field(std::string_view name, std::string_view value);
[[nodiscard]] Result<WritableMetadataPatch>
writable_metadata_patch_for_field(std::string_view name, const std::optional<std::string> &value);
[[nodiscard]] WritableMetadataPatch writable_metadata_patch_all(const WritableMetadata &metadata);
void apply_writable_metadata_patch(WritableMetadata &metadata,
                                   const WritableMetadataPatch &patch) noexcept;
[[nodiscard]] std::string asset_display_name(const AssetRecord &asset);
[[nodiscard]] bool asset_matches_query(const AssetRecord &asset, const LibraryQuery &query);
[[nodiscard]] Result<void> validate_library_query(const LibraryQuery &query);
[[nodiscard]] Result<void> validate_library_page_request(const LibraryPageRequest &request);
[[nodiscard]] Result<std::string> normalize_library_set_name(std::string_view name);
[[nodiscard]] std::string_view library_set_kind_name(LibrarySetKind kind) noexcept;
[[nodiscard]] Result<LibrarySetKind> parse_library_set_kind(std::string_view name);
[[nodiscard]] Result<std::string> serialize_library_query_document(const LibraryQuery &query);
[[nodiscard]] Result<LibraryQuery> parse_library_query_document(std::string_view json);
[[nodiscard]] Result<void> validate_library_set_record(const LibrarySetRecord &set);
[[nodiscard]] std::vector<AssetRecord> filter_and_sort_assets(std::vector<AssetRecord> assets,
                                                              const LibraryQuery &query);
[[nodiscard]] bool asset_in_folder(const AssetRecord &asset, std::string_view folder_uri) noexcept;
[[nodiscard]] std::vector<FolderRecord> library_folders(const std::vector<AssetRecord> &assets);
[[nodiscard]] std::vector<FolderRecord>
library_folders_from_counts(const std::vector<FolderAssetCount> &direct_counts,
                            int total_asset_count);

} // namespace ravo
