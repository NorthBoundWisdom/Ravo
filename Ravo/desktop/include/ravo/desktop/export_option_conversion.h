#pragma once

#include <string>
#include <string_view>

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"

namespace ravo
{

inline constexpr std::string_view kStudioExportOptionQuality = "quality";
inline constexpr std::string_view kStudioExportOptionJpegSubsampling = "jpegSubsampling";
inline constexpr std::string_view kStudioExportOptionPngBitDepth = "pngBitDepth";
inline constexpr std::string_view kStudioExportOptionPngCompression = "pngCompression";
inline constexpr std::string_view kStudioExportOptionTiffSampleType = "tiffSampleType";
inline constexpr std::string_view kStudioExportOptionTiffCompression = "tiffCompression";
inline constexpr std::string_view kStudioExportOptionTiffCompressionLevel = "tiffCompressionLevel";
inline constexpr std::string_view kStudioExportOptionTiffGrayscaleIfNeutral =
    "tiffGrayscaleIfNeutral";
inline constexpr std::string_view kStudioExportOptionTiffResolutionDpi = "tiffResolutionDpi";
inline constexpr std::string_view kStudioExportOptionMetadataMode = "metadataMode";
inline constexpr std::string_view kStudioExportOptionMaxEdge = "maxEdge";
inline constexpr std::string_view kStudioExportOptionMaxWidth = "maxWidth";
inline constexpr std::string_view kStudioExportOptionMaxHeight = "maxHeight";
inline constexpr std::string_view kStudioExportOptionOutputSharpenEnabled = "outputSharpenEnabled";
inline constexpr std::string_view kStudioExportOptionOutputSharpenAmount = "outputSharpenAmount";
inline constexpr std::string_view kStudioExportOptionOutputSharpenRadius = "outputSharpenRadius";
inline constexpr std::string_view kStudioExportOptionOutputSharpenThreshold =
    "outputSharpenThreshold";

struct StudioExportSelection
{
    ExportFormat format = ExportFormat::kPng;
    JpegExportOptions jpeg_options;
    PngExportOptions png_options;
    TiffExportOptions tiff_options;
    ExportMetadataMode metadata_mode = ExportMetadataMode::kFull;
    std::uint32_t max_edge = 0;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
    ExportOutputSharpenOptions output_sharpen;
};

[[nodiscard]] Result<StudioExportSelection>
studio_export_options_from_presentation(const QString &format_name, const QVariantMap &options);

[[nodiscard]] Result<QString> normalize_studio_export_path(const QString &path,
                                                           ExportFormat format);

[[nodiscard]] Result<ExportOptions> make_studio_export_options(const QString &format_name,
                                                               const QVariantMap &options);

[[nodiscard]] Result<ExportRequest> make_studio_export_request(std::string asset_id,
                                                               const QString &path,
                                                               const QString &format_name,
                                                               const QVariantMap &options);

[[nodiscard]] QVariantList studio_export_format_choices();
[[nodiscard]] QVariantList studio_jpeg_subsampling_choices();
[[nodiscard]] QVariantList studio_png_bit_depth_choices();
[[nodiscard]] QVariantList studio_tiff_sample_type_choices();
[[nodiscard]] QVariantList studio_tiff_compression_choices();
[[nodiscard]] QVariantList studio_export_metadata_mode_choices();
[[nodiscard]] QVariantMap studio_export_default_options();
[[nodiscard]] QVariantMap studio_export_option_bounds();

} // namespace ravo
