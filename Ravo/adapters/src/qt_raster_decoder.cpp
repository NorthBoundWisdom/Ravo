#include "ravo/adapters/qt_raster_decoder.h"

#include <limits>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QIODevice>
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

[[nodiscard]] Result<EncodedPng> encode_preview(QImage image, const std::uint32_t max_edge,
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

    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to encode PNG preview",
                          {{"path", std::string(context)}});
    }
    QImageWriter writer(&buffer, "png");
    writer.setCompression(1);
    if (!writer.write(image))
    {
        return make_error(ErrorCode::kIo, "Unable to encode PNG preview",
                          {{"path", std::string(context)},
                           {"qt_error", writer.errorString().toUtf8().toStdString()}});
    }

    EncodedPng result;
    result.width = static_cast<std::uint32_t>(image.width());
    result.height = static_cast<std::uint32_t>(image.height());
    result.bytes.assign(encoded.cbegin(), encoded.cend());
    return result;
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

Result<EncodedPng> QtRasterDecoder::decode(const std::string_view path,
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
    return encode_preview(reader.read(), max_edge, cancellation, path);
}

Result<EncodedPng> QtRasterDecoder::decode_memory(const std::vector<std::uint8_t> &encoded,
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
    return encode_preview(apply_display_rotation(reader.read(), rotate_quarters), max_edge,
                          cancellation, "memory");
}

Result<std::vector<std::uint8_t>>
QtRasterDecoder::encode(const std::uint32_t width, const std::uint32_t height,
                        const std::vector<std::uint8_t> &rgb, const ExportFormat format,
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
    if (!writer.write(owned))
    {
        return make_error(ErrorCode::kIo, "Unable to encode export image",
                          {{"format", std::string(export_format_name(format))},
                           {"qt_error", writer.errorString().toUtf8().toStdString()}});
    }
    return std::vector<std::uint8_t>(encoded.cbegin(), encoded.cend());
}

} // namespace ravo
