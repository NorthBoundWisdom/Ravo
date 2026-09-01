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

[[nodiscard]] Result<TiffFileCandidate>
read_tiff_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
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
    const QByteArray prefix = file.peek(4);
    TiffFileCandidate result;
    result.recognized = is_tiff_payload(byte_span(prefix));
    if (!result.recognized)
    {
        return result;
    }
    if (info.size() < 0 || info.size() > std::numeric_limits<qsizetype>::max() ||
        static_cast<std::uint64_t>(info.size()) > kTiffMaxEncodedBytes)
    {
        return tiff_error(ErrorCode::kValidation, "TIFF input is too large", path,
                          "oversized_tiff_input", {{"path", std::string(path)}});
    }
    result.bytes.reserve(static_cast<qsizetype>(info.size()));
    while (!file.atEnd())
    {
        if (cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty())
        {
            if (file.error() != QFileDevice::NoError || file.atEnd())
            {
                break;
            }
            return tiff_error(ErrorCode::kIo, "TIFF input read made no progress", path,
                              "tiff_read_stalled", {{"path", std::string(path)}});
        }
        result.bytes.append(chunk);
    }
    if (file.error() != QFileDevice::NoError || result.bytes.size() != info.size())
    {
        return tiff_error(
            ErrorCode::kIo, "Unable to read complete TIFF input", path, "tiff_read_failed",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    return result;
}

[[nodiscard]] bool is_tiff_qt_format(const QByteArray &format)
{
    const QByteArray lowered = format.toLower();
    return lowered == QByteArrayLiteral("tif") || lowered == QByteArrayLiteral("tiff");
}

[[nodiscard]] QByteArray tiff_decoder_bytes(const QByteArray &bytes, const TiffContract &contract)
{
    if (!contract.alpha_value_offset)
    {
        return bytes;
    }
    QByteArray result = bytes;
    const std::size_t offset = *contract.alpha_value_offset;
    // Qt normalizes both associated and unassociated alpha through a premultiplied
    // QImage, which changes stored RGB through integer round trips. The retired
    // input owner discarded the extra plane and preserved stored RGB, so mark it
    // unspecified only in this private decoder copy before handing it to Qt.
    result[static_cast<qsizetype>(offset)] = 0;
    result[static_cast<qsizetype>(offset + 1U)] = 0;
    return result;
}

class TiffCancellationBuffer final : public QBuffer
{
public:
    TiffCancellationBuffer(QByteArray *const data, const CancellationToken &cancellation)
        : QBuffer(data)
        , cancellation_(cancellation)
    {
    }

protected:
    qint64 readData(char *const data, const qint64 maximum_size) override
    {
        const auto active = cancellation_.check();
        if (!active)
        {
            return -1;
        }
        return QBuffer::readData(data, maximum_size);
    }

private:
    CancellationToken cancellation_;
};

[[nodiscard]] Result<void>
prepare_tiff_reader(QImageReader &reader, const TiffContract &contract,
                    const std::string_view source,
                    const CancellationToken *const cancellation = nullptr)
{
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || !is_tiff_qt_format(reader.format()))
    {
        if (cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        return tiff_error(ErrorCode::kValidation, "Unable to read TIFF image header", source,
                          "tiff_header_decode_failed",
                          {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    const QSize size = reader.size();
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    if (size.width() != static_cast<int>(contract.width) ||
        size.height() != static_cast<int>(contract.height))
    {
        return tiff_error(ErrorCode::kValidation,
                          "TIFF decoder dimensions disagree with the primary IFD", source,
                          "tiff_dimension_mismatch",
                          {{"ifd_height", std::to_string(contract.height)},
                           {"ifd_width", std::to_string(contract.width)},
                           {"qt_height", std::to_string(size.height())},
                           {"qt_width", std::to_string(size.width())}});
    }
    return {};
}

[[nodiscard]] Result<QImage> read_tiff_pixels(QImageReader &reader,
                                              const CancellationToken *const cancellation,
                                              const std::string_view source)
{
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    QImage image = reader.read();
    if (cancellation != nullptr)
    {
        auto active = cancellation->check();
        if (!active)
        {
            return active.error();
        }
    }
    if (image.isNull() || reader.error() != QImageReader::UnknownError)
    {
        return tiff_error(ErrorCode::kValidation, "Unable to decode complete TIFF pixels", source,
                          "tiff_pixel_decode_failed",
                          {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    return image;
}

[[nodiscard]] Result<RasterInfo> probe_tiff_bytes(const QByteArray &bytes,
                                                  const std::string_view source)
{
    auto contract = parse_tiff_contract(byte_span(bytes), source);
    if (!contract)
    {
        return contract.error();
    }
    QByteArray decoder_bytes = tiff_decoder_bytes(bytes, contract.value());
    QBuffer buffer(&decoder_bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return tiff_error(ErrorCode::kIo, "Unable to open TIFF input buffer", source,
                          "tiff_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    auto prepared = prepare_tiff_reader(reader, contract.value(), source);
    if (!prepared)
    {
        return prepared.error();
    }
    auto image = read_tiff_pixels(reader, nullptr, source);
    if (!image)
    {
        return image.error();
    }
    image.value() = apply_png_orientation(std::move(image).value(), contract.value().orientation);
    const std::uint32_t expected_width =
        contract.value().orientation >= 5U ? contract.value().height : contract.value().width;
    const std::uint32_t expected_height =
        contract.value().orientation >= 5U ? contract.value().width : contract.value().height;
    if (image.value().width() != static_cast<int>(expected_width) ||
        image.value().height() != static_cast<int>(expected_height))
    {
        return tiff_error(ErrorCode::kValidation,
                          "TIFF decoder produced unexpected oriented dimensions", source,
                          "tiff_transformed_dimension_mismatch");
    }
    RasterInfo result;
    result.media_type = std::string(kMediaTypeTiff);
    result.width = expected_width;
    result.height = expected_height;
    return result;
}

[[nodiscard]] Result<DecodedRaster> decode_tiff_raster(QImage image, const std::uint32_t max_edge,
                                                       const CancellationToken &cancellation,
                                                       const std::string_view source,
                                                       ColorProfileState color_profile,
                                                       const QSize source_size)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (image.isNull())
    {
        return tiff_error(ErrorCode::kValidation, "Unable to decode complete TIFF pixels", source,
                          "tiff_pixel_decode_failed");
    }
    if (!source_size.isValid() || source_size.width() <= 0 || source_size.height() <= 0)
    {
        return tiff_error(ErrorCode::kValidation, "TIFF source dimensions are invalid", source,
                          "invalid_tiff_source_dimensions");
    }
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    fit_within_max_edge(static_cast<std::uint32_t>(image.width()),
                        static_cast<std::uint32_t>(image.height()), max_edge, width, height);
    if (width != static_cast<std::uint32_t>(image.width()) ||
        height != static_cast<std::uint32_t>(image.height()))
    {
        image = image.scaled(static_cast<int>(width), static_cast<int>(height),
                             Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    image = image.convertToFormat(QImage::Format_RGBX64);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
    {
        return tiff_error(ErrorCode::kValidation, "Unable to normalize TIFF pixels", source,
                          "tiff_pixel_normalization_failed");
    }

    DecodedRaster result;
    result.width = static_cast<std::uint32_t>(image.width());
    result.height = static_cast<std::uint32_t>(image.height());
    result.source_width = static_cast<std::uint32_t>(source_size.width());
    result.source_height = static_cast<std::uint32_t>(source_size.height());
    result.color_profile = std::move(color_profile);
    result.pixel_format = RasterPixelFormat::kRgb8;
    result.alpha_mode = RasterAlphaMode::kOpaque;
    const std::size_t row_bytes = static_cast<std::size_t>(result.width) * 3U;
    result.rgb.resize(row_bytes * result.height);
    for (std::uint32_t row = 0U; row < result.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const auto *source_pixels =
            reinterpret_cast<const QRgba64 *>(image.constScanLine(static_cast<int>(row)));
        auto destination = result.rgb.begin() + static_cast<std::ptrdiff_t>(row * row_bytes);
        for (std::uint32_t column = 0U; column < result.width; ++column)
        {
            const QRgba64 pixel = source_pixels[column];
            *destination++ = png_channel_to_u8(pixel.red());
            *destination++ = png_channel_to_u8(pixel.green());
            *destination++ = png_channel_to_u8(pixel.blue());
        }
    }
    return result;
}

[[nodiscard]] Result<DecodedRaster> decode_tiff_bytes(const QByteArray &bytes,
                                                      const std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      const std::string_view source,
                                                      const int rotate_quarters)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto contract = parse_tiff_contract(byte_span(bytes), source, &cancellation);
    if (!contract)
    {
        return contract.error();
    }
    QByteArray decoder_bytes = tiff_decoder_bytes(bytes, contract.value());
    TiffCancellationBuffer buffer(&decoder_bytes, cancellation);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return tiff_error(ErrorCode::kIo, "Unable to open TIFF input buffer", source,
                          "tiff_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    auto prepared = prepare_tiff_reader(reader, contract.value(), source, &cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    QSize source_size(static_cast<int>(contract.value().width),
                      static_cast<int>(contract.value().height));
    if (contract.value().orientation >= 5U)
    {
        source_size.transpose();
    }
    source_size = apply_display_rotation_to_size(source_size, rotate_quarters);
    apply_scaled_decode_size(reader, max_edge);
    auto image = read_tiff_pixels(reader, &cancellation, source);
    if (!image)
    {
        return image.error();
    }
    image.value() = apply_png_orientation(std::move(image).value(), contract.value().orientation);
    image.value() = apply_display_rotation(std::move(image).value(), rotate_quarters);
    return decode_tiff_raster(std::move(image).value(), max_edge, cancellation, source,
                              std::move(contract).value().color_profile, source_size);
}

} // namespace ravo::qt_raster_internal
