#include "ravo/adapters/qt_raster_decoder.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QIODevice>
#include <QtGui/QColorSpace>
#include <QtGui/QImage>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QTransform>

namespace ravo
{
namespace
{

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return {};
    }
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] bool is_allowed_raster_format(const QByteArray &format)
{
    const QByteArray lowered = format.toLower();
    return lowered == QByteArrayLiteral("png") || lowered == QByteArrayLiteral("jpeg") ||
           lowered == QByteArrayLiteral("jpg") || lowered == QByteArrayLiteral("bmp") ||
           lowered == QByteArrayLiteral("gif") || lowered == QByteArrayLiteral("webp") ||
           lowered == QByteArrayLiteral("tif") || lowered == QByteArrayLiteral("tiff");
}

[[nodiscard]] std::string media_type_for_format(const QByteArray &format)
{
    const QByteArray lowered = format.toLower();
    if (lowered == QByteArrayLiteral("png"))
    {
        return std::string(kMediaTypePng);
    }
    if (lowered == QByteArrayLiteral("jpeg") || lowered == QByteArrayLiteral("jpg"))
    {
        return std::string(kMediaTypeJpeg);
    }
    if (lowered == QByteArrayLiteral("tif") || lowered == QByteArrayLiteral("tiff"))
    {
        return std::string(kMediaTypeTiff);
    }
    if (lowered == QByteArrayLiteral("bmp"))
    {
        return std::string(kMediaTypeBmp);
    }
    if (lowered == QByteArrayLiteral("gif"))
    {
        return std::string(kMediaTypeGif);
    }
    if (lowered == QByteArrayLiteral("webp"))
    {
        return std::string(kMediaTypeWebp);
    }
    return std::string("image/") + lowered.toStdString();
}

[[nodiscard]] Result<void> prepare_raster_reader(QImageReader &reader, const std::string_view path)
{
    if (path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must not be empty");
    }
    reader.setFileName(qstring_from_utf8(path));
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
    {
        return make_error(ErrorCode::kUnsupported, "File is not a readable raster image",
                          {{"path", std::string(path)},
                           {"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    if (!is_allowed_raster_format(reader.format()))
    {
        return make_error(ErrorCode::kUnsupported, "Raster format is not enabled for import",
                          {{"path", std::string(path)}, {"format", reader.format().toStdString()}});
    }
    return {};
}

[[nodiscard]] QSize transformed_reader_size(const QImageReader &reader)
{
    QSize size = reader.size();
    const auto transformation = reader.transformation();
    if (transformation == QImageIOHandler::TransformationRotate90 ||
        transformation == QImageIOHandler::TransformationRotate270 ||
        transformation == QImageIOHandler::TransformationMirrorAndRotate90 ||
        transformation == QImageIOHandler::TransformationFlipAndRotate90)
    {
        size.transpose();
    }
    return size;
}

[[nodiscard]] QImage apply_display_rotation(QImage image, const int rotate_quarters)
{
    const int turns = ((rotate_quarters % 4) + 4) % 4;
    if (turns == 0 || image.isNull())
    {
        return image;
    }
    QTransform transform;
    transform.rotate(static_cast<qreal>(turns) * 90.0);
    return image.transformed(transform, Qt::FastTransformation);
}

void apply_scaled_decode_size(QImageReader &reader, const std::uint32_t max_edge)
{
    const QSize size = transformed_reader_size(reader);
    if (size.width() <= 0 || size.height() <= 0)
    {
        return;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(static_cast<std::uint32_t>(size.width()),
                        static_cast<std::uint32_t>(size.height()), max_edge, width, height);
    if (width != static_cast<std::uint32_t>(size.width()) ||
        height != static_cast<std::uint32_t>(size.height()))
    {
        reader.setScaledSize(QSize(static_cast<int>(width), static_cast<int>(height)));
    }
}

[[nodiscard]] ColorProfileState color_profile_for_image(const QImage &image)
{
    ColorProfileState result;
    const QColorSpace color_space = image.colorSpace();
    if (!color_space.isValid())
    {
        return result;
    }
    const QByteArray icc = color_space.iccProfile();
    if (!icc.isEmpty())
    {
        result.kind = ColorProfileKind::kIcc;
        result.model = ColorModel::kRgb;
        result.identifier = "embedded_icc";
        result.icc_bytes.assign(icc.cbegin(), icc.cend());
        return result;
    }

    result.kind = ColorProfileKind::kBuiltin;
    result.model = ColorModel::kRgb;
    if (color_space == QColorSpace(QColorSpace::SRgb))
    {
        result.identifier = "srgb";
    }
    else if (color_space == QColorSpace(QColorSpace::SRgbLinear))
    {
        result.identifier = "linear_rec709";
    }
    else if (color_space == QColorSpace(QColorSpace::AdobeRgb))
    {
        result.identifier = "adobe_rgb";
    }
    else if (color_space == QColorSpace(QColorSpace::DisplayP3))
    {
        result.identifier = "display_p3";
    }
    else if (color_space == QColorSpace(QColorSpace::Bt2100Pq))
    {
        result.identifier = "pq_rec2020";
    }
    else if (color_space == QColorSpace(QColorSpace::Bt2100Hlg))
    {
        result.identifier = "hlg_rec2020";
    }
    else
    {
        result.kind = ColorProfileKind::kMissing;
        result.identifier.clear();
    }
    return result;
}

[[nodiscard]] Result<DecodedRaster> decode_raster(QImage image, const std::uint32_t max_edge,
                                                  const CancellationToken &cancellation,
                                                  const std::string_view context)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (image.isNull())
    {
        return make_error(ErrorCode::kIo, "Unable to decode raster image",
                          {{"path", std::string(context)}});
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(static_cast<std::uint32_t>(image.width()),
                        static_cast<std::uint32_t>(image.height()), max_edge, width, height);
    if (width != static_cast<std::uint32_t>(image.width()) ||
        height != static_cast<std::uint32_t>(image.height()))
    {
        image = image.scaled(static_cast<int>(width), static_cast<int>(height),
                             Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGB888);

    DecodedRaster result;
    result.width = static_cast<std::uint32_t>(image.width());
    result.height = static_cast<std::uint32_t>(image.height());
    result.color_profile = color_profile_for_image(image);
    const std::size_t row_bytes = static_cast<std::size_t>(result.width) * 3U;
    result.rgb.resize(row_bytes * result.height);
    for (std::uint32_t row = 0; row < result.height; ++row)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const auto *source = image.constScanLine(static_cast<int>(row));
        std::copy_n(source, row_bytes,
                    result.rgb.begin() + static_cast<std::ptrdiff_t>(row * row_bytes));
    }
    return result;
}

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

} // namespace

Result<RasterInfo> QtRasterDecoder::probe(const std::string_view path) const
{
    QImageReader reader;
    auto prepared = prepare_raster_reader(reader, path);
    if (!prepared)
    {
        return prepared.error();
    }
    QSize size = reader.size();
    const auto transformation = reader.transformation();
    if (transformation == QImageIOHandler::TransformationRotate90 ||
        transformation == QImageIOHandler::TransformationRotate270 ||
        transformation == QImageIOHandler::TransformationMirrorAndRotate90 ||
        transformation == QImageIOHandler::TransformationFlipAndRotate90)
    {
        size.transpose();
    }
    if (size.width() <= 0 || size.height() <= 0)
    {
        return make_error(ErrorCode::kValidation, "Raster image has invalid dimensions",
                          {{"path", std::string(path)}});
    }
    RasterInfo info;
    info.media_type = media_type_for_format(reader.format());
    info.width = static_cast<std::uint32_t>(size.width());
    info.height = static_cast<std::uint32_t>(size.height());
    return info;
}

Result<DecodedRaster> QtRasterDecoder::decode(const std::string_view path,
                                              const std::uint32_t max_edge,
                                              const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    QImageReader reader;
    auto prepared = prepare_raster_reader(reader, path);
    if (!prepared)
    {
        return prepared.error();
    }
    apply_scaled_decode_size(reader, max_edge);
    return decode_raster(reader.read(), max_edge, cancellation, path);
}

Result<DecodedRaster> QtRasterDecoder::decode_memory(const std::vector<std::uint8_t> &encoded,
                                                     const std::uint32_t max_edge,
                                                     const CancellationToken &cancellation,
                                                     const int rotate_quarters) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (encoded.empty() ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kValidation, "Embedded preview payload is empty or too large");
    }
    QByteArray bytes(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<qsizetype>(encoded.size()));
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open embedded preview payload");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    apply_scaled_decode_size(reader, max_edge);
    return decode_raster(apply_display_rotation(reader.read(), rotate_quarters), max_edge,
                         cancellation, "memory");
}

Result<std::vector<std::uint8_t>>
QtRasterDecoder::encode(const std::uint32_t width, const std::uint32_t height,
                        const std::vector<std::uint8_t> &rgb,
                        const ColorProfileState &color_profile, const ExportFormat format,
                        const int jpeg_quality, const CancellationToken &cancellation) const
{
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
    const auto expected =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 3U;
    if (width == 0 || height == 0 || rgb.size() != expected)
    {
        return make_error(ErrorCode::kValidation, "Export image buffer does not match dimensions",
                          {{"height", std::to_string(height)},
                           {"size_bytes", std::to_string(rgb.size())},
                           {"width", std::to_string(width)}});
    }
    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kValidation, "Export image is too large to encode");
    }
    QByteArray format_id;
    if (format == ExportFormat::kPng)
    {
        format_id = QByteArrayLiteral("png");
    }
    else if (format == ExportFormat::kJpeg)
    {
        format_id = QByteArrayLiteral("jpeg");
    }
    else if (format == ExportFormat::kTiff)
    {
        format_id = QByteArrayLiteral("tiff");
    }
    if (!QImageWriter::supportedImageFormats().contains(format_id))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Export format is not available in this Qt build",
                          {{"format", std::string(export_format_name(format))}});
    }
    QImage image(rgb.data(), static_cast<int>(width), static_cast<int>(height),
                 static_cast<int>(width * 3U), QImage::Format_RGB888);
    if (image.isNull())
    {
        return make_error(ErrorCode::kIo, "Unable to wrap export pixels");
    }
    const QImage owned = image.copy();
    auto output_color_space = qt_output_color_space(color_profile);
    if (!output_color_space)
    {
        return output_color_space.error();
    }
    QImage profiled = owned;
    profiled.setColorSpace(output_color_space.value());
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open export encoder buffer");
    }
    QImageWriter writer(&buffer, format_id);
    if (format == ExportFormat::kJpeg)
    {
        writer.setQuality(jpeg_quality);
    }
    if (format == ExportFormat::kPng)
    {
        writer.setCompression(1);
    }
    if (!writer.write(profiled))
    {
        return make_error(ErrorCode::kIo, "Unable to encode export image",
                          {{"format", std::string(export_format_name(format))},
                           {"qt_error", writer.errorString().toUtf8().toStdString()}});
    }
    return std::vector<std::uint8_t>(encoded.cbegin(), encoded.cend());
}

} // namespace ravo
