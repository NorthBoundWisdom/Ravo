#include "ravo/adapters/qt_raster_decoder.h"

#include "jpeg_encoder.h"
#include "png_encoder.h"
#include "tiff_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zlib.h>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtGui/QColorSpace>
#include <QtGui/QColorTransform>
#include <QtGui/QImage>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageReader>
#include <QtGui/QTransform>

#include "qt_raster_internal.h"

namespace ravo
{
using namespace qt_raster_internal;

namespace qt_raster_internal
{
[[nodiscard]] Result<QColorSpace> qt_output_color_space(const ColorProfileState &profile)
{
    if (!profile.icc_bytes.empty())
    {
        if (profile.icc_bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        {
            return make_error(ErrorCode::kValidation, "Output ICC profile is too large");
        }
        const QByteArray bytes(reinterpret_cast<const char *>(profile.icc_bytes.data()),
                               static_cast<qsizetype>(profile.icc_bytes.size()));
        const QColorSpace result = QColorSpace::fromIccProfile(bytes);
        if (result.isValid())
        {
            return result;
        }
        return make_error(ErrorCode::kValidation, "Output ICC profile is corrupt");
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "srgb")
    {
        return QColorSpace(QColorSpace::SRgb);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "linear_rec709")
    {
        return QColorSpace(QColorSpace::SRgbLinear);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "adobe_rgb")
    {
        return QColorSpace(QColorSpace::AdobeRgb);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "display_p3")
    {
        return QColorSpace(QColorSpace::DisplayP3);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "linear_rec2020")
    {
        return QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "rec709")
    {
        return QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Bt2020);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "prophoto_rgb")
    {
        return QColorSpace(QColorSpace::Primaries::ProPhotoRgb,
                           QColorSpace::TransferFunction::Linear);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "pq_rec2020")
    {
        return QColorSpace(QColorSpace::Bt2100Pq);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "hlg_rec2020")
    {
        return QColorSpace(QColorSpace::Bt2100Hlg);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "pq_p3")
    {
        return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::St2084);
    }
    if (profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "hlg_p3")
    {
        return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Hlg);
    }
    return make_error(ErrorCode::kUnsupported, "Raster encoder output profile is unsupported",
                      {{"profile", profile.identifier}});
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 4U>>
png_cicp_for_profile(const ColorProfileState &profile) noexcept
{
    if (profile.kind != ColorProfileKind::kBuiltin || profile.model != ColorModel::kRgb ||
        profile.camera_input)
    {
        return std::nullopt;
    }
    const std::string_view identifier = profile.identifier;
    if (identifier == "srgb")
    {
        return std::array<std::uint8_t, 4U>{1U, 13U, 0U, 1U};
    }
    if (identifier == "rec709")
    {
        return std::array<std::uint8_t, 4U>{1U, 1U, 0U, 1U};
    }
    if (identifier == "linear_rec709")
    {
        return std::array<std::uint8_t, 4U>{1U, 8U, 0U, 1U};
    }
    if (identifier == "linear_rec2020")
    {
        return std::array<std::uint8_t, 4U>{9U, 8U, 0U, 1U};
    }
    if (identifier == "pq_rec2020")
    {
        return std::array<std::uint8_t, 4U>{9U, 16U, 0U, 1U};
    }
    if (identifier == "hlg_rec2020")
    {
        return std::array<std::uint8_t, 4U>{9U, 18U, 0U, 1U};
    }
    if (identifier == "pq_p3")
    {
        return std::array<std::uint8_t, 4U>{12U, 16U, 0U, 1U};
    }
    if (identifier == "hlg_p3")
    {
        return std::array<std::uint8_t, 4U>{12U, 18U, 0U, 1U};
    }
    if (identifier == "display_p3")
    {
        return std::array<std::uint8_t, 4U>{12U, 13U, 0U, 1U};
    }
    return std::nullopt;
}

Result<std::vector<std::uint8_t>>
encode_export_pixels(const std::uint32_t width, const std::uint32_t height,
                     const ColorProfileState &color_profile, const ExportSampleView &samples,
                     const ExportFormat format, const JpegExportOptions &jpeg_options,
                     const CancellationToken &cancellation, const PngExportOptions &png_options,
                     const TiffExportOptions &tiff_options, const ExportMetadataSnapshot &metadata)
{
    const auto *const rgb8 = std::get_if<std::span<const std::uint8_t>>(&samples);
    const auto *const rgb16 = std::get_if<std::span<const std::uint16_t>>(&samples);
    const auto *const rgb_float = std::get_if<std::span<const float>>(&samples);
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (format == ExportFormat::kOriginalCopy)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Original-copy export does not encode pixels");
    }
    if (format == ExportFormat::kJpeg)
    {
        if (rgb8 == nullptr)
        {
            return make_error(ErrorCode::kValidation, "JPEG export requires an 8-bit RGB source",
                              {{"format", "jpeg"}, {"reason", "jpeg_source_sample_mismatch"}});
        }
        if (width == 0U || height == 0U || width > detail::kJpegMaxDimension ||
            height > detail::kJpegMaxDimension)
        {
            return make_error(ErrorCode::kValidation, "JPEG dimensions exceed the encoder limit",
                              {{"format", "jpeg"},
                               {"height", std::to_string(height)},
                               {"maximum_dimension", std::to_string(detail::kJpegMaxDimension)},
                               {"reason", "invalid_jpeg_dimensions"},
                               {"width", std::to_string(width)}});
        }
        const std::uint64_t source_bytes =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 3U;
        if (source_bytes > detail::kJpegMaxSourceBytes)
        {
            return make_error(ErrorCode::kValidation, "JPEG RGB source exceeds the safe bound",
                              {{"format", "jpeg"},
                               {"maximum_bytes", std::to_string(detail::kJpegMaxSourceBytes)},
                               {"reason", "jpeg_source_too_large"},
                               {"size_bytes", std::to_string(source_bytes)}});
        }
        if (rgb8->size() != source_bytes)
        {
            return make_error(ErrorCode::kValidation,
                              "JPEG RGB source does not match its dimensions",
                              {{"actual_bytes", std::to_string(rgb8->size())},
                               {"expected_bytes", std::to_string(source_bytes)},
                               {"format", "jpeg"},
                               {"reason", "jpeg_source_size_mismatch"}});
        }
        auto configuration = detail::jpeg_encode_configuration(jpeg_options);
        if (!configuration)
        {
            return configuration.error();
        }
        auto output_color_space = qt_output_color_space(color_profile);
        if (!output_color_space)
        {
            return output_color_space.error();
        }
        if (output_color_space.value().colorModel() != QColorSpace::ColorModel::Rgb)
        {
            return make_error(
                ErrorCode::kUnsupported, "JPEG output ICC is not an RGB profile",
                {{"format", "jpeg"}, {"reason", "unsupported_jpeg_output_icc_color_model"}});
        }
        const QByteArray icc = output_color_space.value().iccProfile();
        if (icc.isEmpty())
        {
            return make_error(ErrorCode::kValidation, "JPEG output ICC could not be resolved",
                              {{"format", "jpeg"}, {"reason", "missing_jpeg_output_icc"}});
        }
        return detail::encode_jpeg_rgb8(width, height, *rgb8,
                                        {reinterpret_cast<const std::uint8_t *>(icc.constData()),
                                         static_cast<std::size_t>(icc.size())},
                                        jpeg_options, metadata,
                                        export_color_space_is_srgb(color_profile), cancellation);
    }
    if (format == ExportFormat::kPng)
    {
        auto valid_options = validate_png_export_options(png_options);
        if (!valid_options)
        {
            return valid_options.error();
        }
        if (png_options.bit_depth == PngBitDepth::k16)
        {
            if (rgb8 != nullptr)
            {
                return detail::encode_png_rgb8(width, height, *rgb8, {}, png_options, cancellation);
            }
            if (rgb16 == nullptr)
            {
                return make_error(ErrorCode::kValidation,
                                  "PNG 16-bit export requires a 16-bit RGB source",
                                  {{"format", "png"}, {"reason", "png_source_sample_mismatch"}});
            }
        }
        else if (rgb8 == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "PNG 8-bit export requires an 8-bit RGB source",
                              {{"format", "png"}, {"reason", "png_source_sample_mismatch"}});
        }
        if (color_profile.model != ColorModel::kRgb)
        {
            return make_error(
                ErrorCode::kUnsupported, "PNG output ICC is not an RGB profile",
                {{"format", "png"}, {"reason", "unsupported_png_output_icc_color_model"}});
        }
        auto output_color_space = qt_output_color_space(color_profile);
        if (!output_color_space)
        {
            const bool unsupported = output_color_space.error().code == ErrorCode::kUnsupported;
            return make_error(output_color_space.error().code, output_color_space.error().message,
                              {{"format", "png"},
                               {"profile", color_profile.identifier},
                               {"reason", unsupported ? "unsupported_png_output_profile" :
                                                        "invalid_png_output_icc"}});
        }
        if (output_color_space.value().colorModel() != QColorSpace::ColorModel::Rgb)
        {
            return make_error(
                ErrorCode::kUnsupported, "PNG output ICC is not an RGB profile",
                {{"format", "png"}, {"reason", "unsupported_png_output_icc_color_model"}});
        }
        const QByteArray icc = output_color_space.value().iccProfile();
        if (icc.isEmpty())
        {
            return make_error(ErrorCode::kValidation, "PNG output ICC could not be resolved",
                              {{"format", "png"}, {"reason", "missing_png_output_icc"}});
        }
        detail::PngEncodeColorMetadata png_metadata;
        png_metadata.resolved_rgb_icc = {reinterpret_cast<const std::uint8_t *>(icc.constData()),
                                         static_cast<std::size_t>(icc.size())};
        const auto cicp = png_cicp_for_profile(color_profile);
        if (cicp)
        {
            png_metadata.has_cicp = true;
            png_metadata.cicp = *cicp;
        }
        if (png_options.bit_depth == PngBitDepth::k16)
        {
            return detail::encode_png_rgb16(width, height, *rgb16, png_metadata, png_options,
                                            metadata, export_color_space_is_srgb(color_profile),
                                            cancellation);
        }
        return detail::encode_png_rgb8(width, height, *rgb8, png_metadata, png_options, metadata,
                                       export_color_space_is_srgb(color_profile), cancellation);
    }
    if (format == ExportFormat::kTiff)
    {
        auto valid_options = validate_tiff_export_options(tiff_options);
        if (!valid_options)
        {
            return valid_options.error();
        }
        auto valid_metadata = validate_tiff_export_metadata(metadata);
        if (!valid_metadata)
        {
            return valid_metadata.error();
        }
        if (tiff_options.sample_type == TiffSampleType::kUint8)
        {
            if (rgb8 == nullptr)
            {
                return make_error(ErrorCode::kValidation,
                                  "TIFF uint8 export requires an 8-bit RGB source",
                                  {{"format", "tiff"}, {"reason", "tiff_source_sample_mismatch"}});
            }
        }
        else if (rgb8 != nullptr)
        {
            return make_error(
                ErrorCode::kUnsupported,
                "TIFF high-precision output requires a high-precision source",
                {{"format", "tiff"},
                 {"reason", "unsupported_tiff_high_precision_source"},
                 {"sample_type", std::string(tiff_sample_type_name(tiff_options.sample_type))}});
        }
        else if (tiff_options.sample_type == TiffSampleType::kUint16 && rgb16 == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "TIFF uint16 export requires a 16-bit RGB source",
                              {{"format", "tiff"}, {"reason", "tiff_source_sample_mismatch"}});
        }
        else if ((tiff_options.sample_type == TiffSampleType::kFloat16 ||
                  tiff_options.sample_type == TiffSampleType::kFloat32) &&
                 rgb_float == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "TIFF float export requires a finite float RGB source",
                              {{"format", "tiff"}, {"reason", "tiff_source_sample_mismatch"}});
        }
        if (color_profile.model != ColorModel::kRgb)
        {
            return make_error(
                ErrorCode::kUnsupported, "TIFF output ICC is not an RGB profile",
                {{"format", "tiff"}, {"reason", "unsupported_tiff_output_icc_color_model"}});
        }
        auto output_color_space = qt_output_color_space(color_profile);
        if (!output_color_space)
        {
            const bool unsupported = output_color_space.error().code == ErrorCode::kUnsupported;
            return make_error(output_color_space.error().code, output_color_space.error().message,
                              {{"format", "tiff"},
                               {"profile", color_profile.identifier},
                               {"reason", unsupported ? "unsupported_tiff_output_profile" :
                                                        "invalid_tiff_output_icc"}});
        }
        if (output_color_space.value().colorModel() != QColorSpace::ColorModel::Rgb)
        {
            return make_error(
                ErrorCode::kUnsupported, "TIFF output ICC is not an RGB profile",
                {{"format", "tiff"}, {"reason", "unsupported_tiff_output_icc_color_model"}});
        }
        const QByteArray icc = output_color_space.value().iccProfile();
        if (icc.isEmpty())
        {
            return make_error(ErrorCode::kValidation, "TIFF output ICC could not be resolved",
                              {{"format", "tiff"}, {"reason", "missing_tiff_output_icc"}});
        }
        const std::span<const std::uint8_t> icc_bytes{
            reinterpret_cast<const std::uint8_t *>(icc.constData()),
            static_cast<std::size_t>(icc.size())};
        if (tiff_options.sample_type == TiffSampleType::kUint8)
        {
            return detail::encode_tiff_rgb8(width, height, *rgb8, icc_bytes, tiff_options, metadata,
                                            export_color_space_is_srgb(color_profile),
                                            cancellation);
        }
        if (tiff_options.sample_type == TiffSampleType::kUint16)
        {
            return detail::encode_tiff_rgb16(width, height, *rgb16, icc_bytes, tiff_options,
                                             metadata, export_color_space_is_srgb(color_profile),
                                             cancellation);
        }
        return detail::encode_tiff_rgb_float(width, height, *rgb_float, icc_bytes, tiff_options,
                                             metadata, export_color_space_is_srgb(color_profile),
                                             cancellation);
    }
    return make_error(ErrorCode::kUnsupported, "Raster export format is unsupported",
                      {{"format", std::string(export_format_name(format))},
                       {"reason", "unsupported_raster_export_format"}});
}

} // namespace qt_raster_internal

Result<std::vector<std::uint8_t>>
QtRasterDecoder::encode(const ExportPixelBuffer &source, const ExportFormat format,
                        const JpegExportOptions &jpeg_options,
                        const CancellationToken &cancellation, const PngExportOptions &png_options,
                        const TiffExportOptions &tiff_options,
                        const ExportMetadataSnapshot &metadata) const
{
    const ExportSampleView samples =
        std::visit([](const auto &owned_samples) -> ExportSampleView
                   { return std::span{owned_samples}; }, source.samples);
    return encode_export_pixels(source.width, source.height, source.color_profile, samples, format,
                                jpeg_options, cancellation, png_options, tiff_options, metadata);
}

} // namespace ravo
