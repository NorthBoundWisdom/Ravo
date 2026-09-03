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

namespace ravo::qt_raster_internal
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

[[nodiscard]] bool starts_with(const std::span<const std::uint8_t> bytes,
                               const std::span<const std::uint8_t> prefix) noexcept
{
    return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

[[nodiscard]] bool is_jpeg_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    return bytes.size() >= 2U && bytes[0] == 0xFFU && bytes[1] == 0xD8U;
}

[[nodiscard]] bool is_png_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    return starts_with(bytes, kPngSignature);
}

[[nodiscard]] bool is_tiff_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < 4U)
    {
        return false;
    }
    const bool little_endian = bytes[0] == 'I' && bytes[1] == 'I';
    const bool big_endian = bytes[0] == 'M' && bytes[1] == 'M';
    if (!little_endian && !big_endian)
    {
        return false;
    }
    const std::uint16_t magic = static_cast<std::uint16_t>(
        little_endian ?
            static_cast<std::uint16_t>(bytes[2]) | (static_cast<std::uint16_t>(bytes[3]) << 8U) :
            (static_cast<std::uint16_t>(bytes[2]) << 8U) | static_cast<std::uint16_t>(bytes[3]));
    return magic == 42U || magic == 43U;
}

[[nodiscard]] bool is_qoi_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    return starts_with(bytes, kQoiSignature);
}

[[nodiscard]] bool is_rgbe_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    return starts_with(bytes, kRadianceSignature) || starts_with(bytes, kRgbeSignature);
}

[[nodiscard]] bool brand_equals(const std::span<const std::uint8_t> brand,
                                const char expected[5]) noexcept
{
    return brand.size() >= 4U && brand[0] == static_cast<std::uint8_t>(expected[0]) &&
           brand[1] == static_cast<std::uint8_t>(expected[1]) &&
           brand[2] == static_cast<std::uint8_t>(expected[2]) &&
           brand[3] == static_cast<std::uint8_t>(expected[3]);
}

[[nodiscard]] bool is_avif_major_brand(const std::span<const std::uint8_t> brand) noexcept
{
    return brand_equals(brand, "avif") || brand_equals(brand, "avis");
}

[[nodiscard]] bool is_heic_heif_brand(const std::span<const std::uint8_t> brand) noexcept
{
    return brand_equals(brand, "heic") || brand_equals(brand, "heix") ||
           brand_equals(brand, "hevc") || brand_equals(brand, "hevx") ||
           brand_equals(brand, "heim") || brand_equals(brand, "heis") ||
           brand_equals(brand, "hevm") || brand_equals(brand, "hevs") ||
           brand_equals(brand, "heif") || brand_equals(brand, "heifs") ||
           brand_equals(brand, "mif1") || brand_equals(brand, "msf1") ||
           brand_equals(brand, "avci") || brand_equals(brand, "avcs");
}

[[nodiscard]] bool is_heic_heif_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < 16U)
    {
        return false;
    }
    if (!(bytes[4] == static_cast<std::uint8_t>('f') &&
          bytes[5] == static_cast<std::uint8_t>('t') &&
          bytes[6] == static_cast<std::uint8_t>('y') && bytes[7] == static_cast<std::uint8_t>('p')))
    {
        return false;
    }
    const std::uint32_t box_size = read_u32_be(bytes.subspan(0U, 4U));
    // size 0 means "to EOF"; size 1 is a 64-bit extended box we do not need to fully parse
    // for brand recognition. Reject undersized declared boxes.
    if (box_size != 0U && box_size != 1U && box_size < 16U)
    {
        return false;
    }
    const auto major = bytes.subspan(8U, 4U);
    // AVIF remains a separate undecided format even when mif1 appears as a compatible brand.
    if (is_avif_major_brand(major))
    {
        return false;
    }
    if (is_heic_heif_brand(major))
    {
        return true;
    }
    std::size_t end = bytes.size();
    if (box_size > 1U)
    {
        end = std::min(end, static_cast<std::size_t>(box_size));
    }
    // Compatible brands start at offset 16.
    for (std::size_t offset = 16U; offset + 4U <= end; offset += 4U)
    {
        if (is_heic_heif_brand(bytes.subspan(offset, 4U)))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::span<const std::uint8_t> byte_span(const QByteArray &bytes) noexcept
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] TaskError jpeg_error(const ErrorCode code, std::string message,
                                   const std::string_view source, const std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context)
{
    context.emplace("format", "jpeg");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError png_error(const ErrorCode code, std::string message,
                                  const std::string_view source, const std::string_view reason,
                                  std::map<std::string, std::string, std::less<>> context)
{
    context.emplace("format", "png");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError tiff_error(const ErrorCode code, std::string message,
                                   const std::string_view source, const std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context)
{
    context.emplace("format", "tiff");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError qoi_unsupported_error(const std::string_view source)
{
    return make_error(
        ErrorCode::kUnsupported, "QOI input is explicitly unsupported",
        {{"format", "qoi"}, {"reason", "unsupported_qoi_input"}, {"source", std::string(source)}});
}

[[nodiscard]] TaskError rgbe_unsupported_error(const std::string_view source)
{
    return make_error(ErrorCode::kUnsupported,
                      "Radiance RGBE input requires the dedicated HDR float pipeline",
                      {{"format", "rgbe"},
                       {"reason", "unsupported_rgbe_input"},
                       {"source", std::string(source)}});
}

[[nodiscard]] TaskError heic_unsupported_error(const std::string_view source)
{
    return make_error(ErrorCode::kUnsupported,
                      "HEIC/HEIF input is explicitly unsupported until an owned decoder ships",
                      {{"format", "heic"},
                       {"reason", "unsupported_heic_input"},
                       {"source", std::string(source)}});
}

[[nodiscard]] std::uint32_t read_u32_be(const std::span<const std::uint8_t> bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::string png_chunk_name(const std::span<const std::uint8_t> type)
{
    return {reinterpret_cast<const char *>(type.data()), type.size()};
}

[[nodiscard]] Result<QoiFileCandidate>
read_qoi_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
{
    if (path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must not be empty");
    }
    const QString file_name = qstring_from_utf8(path);
    const QFileInfo info(file_name);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Raster input does not exist",
                          {{"path", std::string(path)}});
    }
    if (!info.isFile())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must reference a regular file",
                          {{"path", std::string(path)}});
    }
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(
            ErrorCode::kIo, "Unable to open raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    const QByteArray prefix = file.peek(static_cast<qint64>(kQoiSignature.size()));
    if (file.error() != QFileDevice::NoError)
    {
        return make_error(
            ErrorCode::kIo, "Unable to inspect raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    return QoiFileCandidate{is_qoi_payload(byte_span(prefix))};
}

[[nodiscard]] Result<RgbeFileCandidate>
read_rgbe_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
{
    if (path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must not be empty");
    }
    const QString file_name = qstring_from_utf8(path);
    const QFileInfo info(file_name);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Raster input does not exist",
                          {{"path", std::string(path)}});
    }
    if (!info.isFile())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must reference a regular file",
                          {{"path", std::string(path)}});
    }
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(
            ErrorCode::kIo, "Unable to open raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    const QByteArray prefix = file.peek(static_cast<qint64>(kRadianceSignature.size()));
    if (file.error() != QFileDevice::NoError)
    {
        return make_error(
            ErrorCode::kIo, "Unable to inspect raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    return RgbeFileCandidate{is_rgbe_payload(byte_span(prefix))};
}

[[nodiscard]] Result<HeicFileCandidate>
read_heic_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
{
    if (path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must not be empty");
    }
    const QString file_name = qstring_from_utf8(path);
    const QFileInfo info(file_name);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Raster input does not exist",
                          {{"path", std::string(path)}});
    }
    if (!info.isFile())
    {
        return make_error(ErrorCode::kInvalidArgument, "Raster path must reference a regular file",
                          {{"path", std::string(path)}});
    }
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(
            ErrorCode::kIo, "Unable to open raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    // HEIC/HEIF brands live in the leading ftyp box; peek a bounded prefix only.
    constexpr qint64 kHeicProbeBytes = 256;
    const QByteArray prefix = file.peek(kHeicProbeBytes);
    if (file.error() != QFileDevice::NoError)
    {
        return make_error(
            ErrorCode::kIo, "Unable to inspect raster input",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    return HeicFileCandidate{is_heic_heif_payload(byte_span(prefix))};
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

[[nodiscard]] QSize apply_display_rotation_to_size(QSize size, const int rotate_quarters)
{
    const int turns = ((rotate_quarters % 4) + 4) % 4;
    if (turns % 2 != 0)
    {
        size.transpose();
    }
    return size;
}

[[nodiscard]] QImage apply_png_orientation(QImage image, const std::uint16_t orientation)
{
    if (image.isNull() || orientation == 1U)
    {
        return image;
    }
    if (orientation == 2U)
    {
        return image.flipped(Qt::Horizontal);
    }
    if (orientation == 4U)
    {
        return image.flipped(Qt::Vertical);
    }
    QTransform transform;
    if (orientation == 3U)
    {
        transform.rotate(180.0);
        return image.transformed(transform, Qt::FastTransformation);
    }
    if (orientation == 8U)
    {
        transform.rotate(270.0);
        return image.transformed(transform, Qt::FastTransformation);
    }
    transform.rotate(90.0);
    image = image.transformed(transform, Qt::FastTransformation);
    if (orientation == 5U)
    {
        image = image.flipped(Qt::Horizontal);
    }
    else if (orientation == 7U)
    {
        image = image.flipped(Qt::Vertical);
    }
    return image;
}

void apply_scaled_decode_size(QImageReader &reader, const std::uint32_t max_edge)
{
    // QImageReader::setScaledSize() takes the encoded, pre-transformation dimensions.
    // The maximum edge is invariant under an EXIF transpose, so scale the native size
    // and let autoTransform swap the decoded dimensions afterwards.
    const QSize size = reader.size();
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
                                                  const std::string_view context,
                                                  std::optional<ColorProfileState> color_profile,
                                                  const QSize source_size)
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

    const QSize original_size = source_size;
    if (original_size.width() <= 0 || original_size.height() <= 0)
    {
        return make_error(
            ErrorCode::kValidation, "Raster source dimensions are invalid",
            {{"path", std::string(context)}, {"reason", "invalid_raster_source_dimensions"}});
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
    result.source_width = static_cast<std::uint32_t>(original_size.width());
    result.source_height = static_cast<std::uint32_t>(original_size.height());
    result.color_profile =
        color_profile ? std::move(*color_profile) : color_profile_for_image(image);
    result.pixel_format = RasterPixelFormat::kRgb8;
    result.alpha_mode = RasterAlphaMode::kOpaque;
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

} // namespace ravo::qt_raster_internal
