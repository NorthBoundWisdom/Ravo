#include "ravo/adapters/qt_raster_decoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
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

inline constexpr std::array<std::uint8_t, 12> kJpegIccSignature{'I', 'C', 'C', '_', 'P', 'R',
                                                                'O', 'F', 'I', 'L', 'E', 0};
inline constexpr std::array<std::uint8_t, 11> kJpegIccPrefix{'I', 'C', 'C', '_', 'P', 'R',
                                                             'O', 'F', 'I', 'L', 'E'};

struct JpegContract
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t components = 0;
    ColorProfileState color_profile;
};

struct JpegFileCandidate
{
    bool recognized = false;
    QByteArray bytes;
};

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

[[nodiscard]] std::span<const std::uint8_t> byte_span(const QByteArray &bytes) noexcept
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] TaskError jpeg_error(const ErrorCode code, std::string message,
                                   const std::string_view source, const std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "jpeg");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] bool is_start_of_frame_marker(const std::uint8_t marker) noexcept
{
    return (marker >= 0xC0U && marker <= 0xC3U) || (marker >= 0xC5U && marker <= 0xC7U) ||
           (marker >= 0xC9U && marker <= 0xCBU) || (marker >= 0xCDU && marker <= 0xCFU);
}

[[nodiscard]] Result<JpegContract>
parse_jpeg_contract(const std::span<const std::uint8_t> bytes, const std::string_view source,
                    const CancellationToken *const cancellation = nullptr)
{
    if (!is_jpeg_payload(bytes))
    {
        return jpeg_error(ErrorCode::kUnsupported, "Input is not a JPEG image", source,
                          "unrecognized_jpeg_content");
    }

    std::size_t position = 2U;
    bool in_scan = false;
    bool saw_scan = false;
    bool saw_eoi = false;
    bool saw_frame = false;
    JpegContract result;
    std::optional<std::uint8_t> icc_segment_count;
    std::vector<std::optional<std::vector<std::uint8_t>>> icc_segments;

    const auto invalid =
        [&](std::string message, const std::string_view reason,
            std::map<std::string, std::string, std::less<>> context = {}) -> Result<JpegContract>
    {
        return jpeg_error(ErrorCode::kValidation, std::move(message), source, reason,
                          std::move(context));
    };

    while (position < bytes.size())
    {
        if (cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        if (!in_scan && bytes[position] != 0xFFU)
        {
            return invalid("JPEG marker stream is malformed", "jpeg_marker_sync_lost",
                           {{"offset", std::to_string(position)}});
        }
        if (in_scan)
        {
            std::size_t bytes_until_cancellation_check = 64U * 1024U;
            while (position < bytes.size() && bytes[position] != 0xFFU)
            {
                ++position;
                if (--bytes_until_cancellation_check == 0U)
                {
                    if (cancellation != nullptr)
                    {
                        auto active = cancellation->check();
                        if (!active)
                        {
                            return active.error();
                        }
                    }
                    bytes_until_cancellation_check = 64U * 1024U;
                }
            }
            if (position == bytes.size())
            {
                break;
            }
        }

        while (position < bytes.size() && bytes[position] == 0xFFU)
        {
            ++position;
            if ((position & 0xFFFFU) == 0U && cancellation != nullptr)
            {
                auto active = cancellation->check();
                if (!active)
                {
                    return active.error();
                }
            }
        }
        if (position == bytes.size())
        {
            break;
        }
        const std::uint8_t marker = bytes[position++];
        if (in_scan && marker == 0x00U)
        {
            continue;
        }
        if (marker >= 0xD0U && marker <= 0xD7U)
        {
            if (!in_scan)
            {
                return invalid("JPEG restart marker appears outside scan data",
                               "jpeg_restart_outside_scan");
            }
            continue;
        }
        in_scan = false;

        if (marker == 0xD9U)
        {
            saw_eoi = true;
            break;
        }
        if (marker == 0xD8U)
        {
            return invalid("JPEG contains a duplicate start marker", "duplicate_jpeg_soi");
        }
        if (marker == 0x01U)
        {
            continue;
        }
        if (marker == 0x00U)
        {
            return invalid("JPEG contains an unexpected stuffed byte",
                           "jpeg_stuffed_byte_outside_scan");
        }
        if (position + 2U > bytes.size())
        {
            return invalid("JPEG marker length is truncated", "truncated_jpeg_marker_length");
        }
        const std::size_t marker_length =
            (static_cast<std::size_t>(bytes[position]) << 8U) | bytes[position + 1U];
        if (marker_length < 2U || marker_length > bytes.size() - position)
        {
            return invalid("JPEG marker payload is truncated", "truncated_jpeg_marker_payload",
                           {{"marker", std::to_string(marker)}});
        }
        const std::size_t payload_begin = position + 2U;
        const std::size_t payload_size = marker_length - 2U;
        const auto payload = bytes.subspan(payload_begin, payload_size);
        position += marker_length;

        if (is_start_of_frame_marker(marker))
        {
            if (saw_frame)
            {
                return invalid("JPEG contains more than one frame header",
                               "duplicate_jpeg_frame_header");
            }
            if (payload.size() < 6U)
            {
                return invalid("JPEG frame header is truncated", "truncated_jpeg_frame_header");
            }
            const auto components = payload[5];
            if (components == 0U || payload.size() != 6U + 3U * components)
            {
                return invalid("JPEG frame component table is malformed",
                               "malformed_jpeg_components");
            }
            result.height = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payload[1]) << 8U) | payload[2]);
            result.width = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payload[3]) << 8U) | payload[4]);
            result.components = components;
            if (result.width == 0U || result.height == 0U)
            {
                return invalid("JPEG frame has invalid dimensions", "invalid_jpeg_dimensions");
            }
            if (components != 1U && components != 3U)
            {
                return jpeg_error(ErrorCode::kUnsupported, "JPEG component layout is unsupported",
                                  source, "unsupported_jpeg_components",
                                  {{"components", std::to_string(components)}});
            }
            saw_frame = true;
        }
        else if (marker == 0xE2U)
        {
            const bool has_icc_prefix = starts_with(payload, kJpegIccPrefix);
            if (!has_icc_prefix)
            {
                continue;
            }
            if (!starts_with(payload, kJpegIccSignature) || payload.size() < 14U)
            {
                return invalid("JPEG ICC marker header is malformed", "malformed_jpeg_icc_header");
            }
            const std::uint8_t sequence = payload[12];
            const std::uint8_t count = payload[13];
            if (count == 0U || sequence == 0U || sequence > count)
            {
                return invalid(
                    "JPEG ICC marker sequence is invalid", "invalid_jpeg_icc_sequence",
                    {{"count", std::to_string(count)}, {"sequence", std::to_string(sequence)}});
            }
            if (!icc_segment_count)
            {
                icc_segment_count = count;
                icc_segments.resize(count);
            }
            else if (*icc_segment_count != count)
            {
                return invalid("JPEG ICC markers disagree on segment count",
                               "inconsistent_jpeg_icc_segment_count",
                               {{"actual", std::to_string(count)},
                                {"expected", std::to_string(*icc_segment_count)}});
            }
            auto &segment = icc_segments[static_cast<std::size_t>(sequence - 1U)];
            if (segment)
            {
                return invalid("JPEG ICC marker sequence is duplicated",
                               "duplicate_jpeg_icc_segment",
                               {{"sequence", std::to_string(sequence)}});
            }
            segment = std::vector<std::uint8_t>(payload.begin() + 14, payload.end());
        }
        else if (marker == 0xDAU)
        {
            if (!saw_frame || payload.size() < 4U)
            {
                return invalid("JPEG scan header is malformed", "malformed_jpeg_scan_header");
            }
            const auto components = payload[0];
            if (components == 0U || payload.size() != 1U + 2U * components + 3U)
            {
                return invalid("JPEG scan component table is malformed",
                               "malformed_jpeg_scan_components");
            }
            saw_scan = true;
            in_scan = true;
        }
    }

    if (!saw_frame || !saw_scan || !saw_eoi)
    {
        return invalid("JPEG image is truncated or incomplete", "incomplete_jpeg_stream",
                       {{"frame", saw_frame ? "present" : "missing"},
                        {"scan", saw_scan ? "present" : "missing"},
                        {"eoi", saw_eoi ? "present" : "missing"}});
    }

    if (icc_segment_count)
    {
        std::size_t profile_size = 0U;
        for (std::size_t index = 0; index < icc_segments.size(); ++index)
        {
            if (!icc_segments[index])
            {
                return invalid("JPEG ICC marker sequence is incomplete", "missing_jpeg_icc_segment",
                               {{"sequence", std::to_string(index + 1U)}});
            }
            profile_size += icc_segments[index]->size();
        }
        std::vector<std::uint8_t> profile;
        profile.reserve(profile_size);
        for (const auto &segment : icc_segments)
        {
            profile.insert(profile.end(), segment->begin(), segment->end());
        }
        if (profile.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        {
            return invalid("JPEG ICC profile is too large", "oversized_jpeg_icc_profile");
        }
        const QByteArray profile_bytes(reinterpret_cast<const char *>(profile.data()),
                                       static_cast<qsizetype>(profile.size()));
        const QColorSpace color_space = QColorSpace::fromIccProfile(profile_bytes);
        if (!color_space.isValid())
        {
            return invalid("JPEG ICC profile is corrupt", "corrupt_jpeg_icc_profile");
        }
        if (color_space.colorModel() != QColorSpace::ColorModel::Rgb)
        {
            return jpeg_error(ErrorCode::kUnsupported, "JPEG ICC profile is not an RGB profile",
                              source, "unsupported_jpeg_icc_color_model");
        }
        result.color_profile.kind = ColorProfileKind::kIcc;
        result.color_profile.model = ColorModel::kRgb;
        result.color_profile.identifier = "embedded_icc";
        result.color_profile.icc_bytes = std::move(profile);
    }
    return result;
}

[[nodiscard]] Result<JpegFileCandidate>
read_jpeg_file_candidate(const std::string_view path,
                         const CancellationToken *const cancellation = nullptr)
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
    const QByteArray prefix = file.peek(3);
    JpegFileCandidate result;
    result.recognized = is_jpeg_payload(byte_span(prefix));
    if (!result.recognized)
    {
        return result;
    }
    if (info.size() < 0 || info.size() > std::numeric_limits<qsizetype>::max())
    {
        return jpeg_error(ErrorCode::kValidation, "JPEG input is too large", path,
                          "oversized_jpeg_input", {{"path", std::string(path)}});
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
            return jpeg_error(ErrorCode::kIo, "JPEG input read made no progress", path,
                              "jpeg_read_stalled", {{"path", std::string(path)}});
        }
        result.bytes.append(chunk);
    }
    if (file.error() != QFileDevice::NoError || result.bytes.size() != info.size())
    {
        return jpeg_error(
            ErrorCode::kIo, "Unable to read complete JPEG input", path, "jpeg_read_failed",
            {{"path", std::string(path)}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    return result;
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

[[nodiscard]] Result<DecodedRaster>
decode_raster(QImage image, const std::uint32_t max_edge, const CancellationToken &cancellation,
              const std::string_view context,
              std::optional<ColorProfileState> color_profile = std::nullopt)
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

[[nodiscard]] Result<RasterInfo> probe_jpeg_bytes(const QByteArray &bytes,
                                                  const std::string_view source)
{
    auto contract = parse_jpeg_contract(byte_span(bytes), source);
    if (!contract)
    {
        return contract.error();
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return jpeg_error(ErrorCode::kIo, "Unable to open JPEG input buffer", source,
                          "jpeg_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || reader.format().toLower() != QByteArrayLiteral("jpeg"))
    {
        return jpeg_error(ErrorCode::kValidation, "Unable to read JPEG image header", source,
                          "jpeg_header_decode_failed",
                          {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    const QSize native_size = reader.size();
    if (native_size.width() != contract.value().width ||
        native_size.height() != contract.value().height)
    {
        return jpeg_error(ErrorCode::kValidation,
                          "JPEG decoder dimensions disagree with the frame header", source,
                          "jpeg_dimension_mismatch",
                          {{"frame_height", std::to_string(contract.value().height)},
                           {"frame_width", std::to_string(contract.value().width)},
                           {"qt_height", std::to_string(native_size.height())},
                           {"qt_width", std::to_string(native_size.width())}});
    }
    const QSize size = transformed_reader_size(reader);
    if (size.width() <= 0 || size.height() <= 0)
    {
        return jpeg_error(ErrorCode::kValidation, "JPEG image has invalid dimensions", source,
                          "invalid_jpeg_dimensions");
    }
    RasterInfo info;
    info.media_type = std::string(kMediaTypeJpeg);
    info.width = static_cast<std::uint32_t>(size.width());
    info.height = static_cast<std::uint32_t>(size.height());
    return info;
}

[[nodiscard]] Result<DecodedRaster> decode_jpeg_bytes(const QByteArray &bytes,
                                                      const std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      const std::string_view source,
                                                      const int rotate_quarters)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto contract = parse_jpeg_contract(byte_span(bytes), source, &cancellation);
    if (!contract)
    {
        return contract.error();
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return jpeg_error(ErrorCode::kIo, "Unable to open JPEG input buffer", source,
                          "jpeg_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || reader.format().toLower() != QByteArrayLiteral("jpeg"))
    {
        return jpeg_error(ErrorCode::kValidation, "Unable to read JPEG image header", source,
                          "jpeg_header_decode_failed",
                          {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    apply_scaled_decode_size(reader, max_edge);
    QImage image = reader.read();
    if (image.isNull())
    {
        return jpeg_error(ErrorCode::kValidation, "Unable to decode JPEG image", source,
                          "jpeg_pixel_decode_failed",
                          {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    image = apply_display_rotation(std::move(image), rotate_quarters);
    return decode_raster(std::move(image), max_edge, cancellation, source,
                         std::move(contract).value().color_profile);
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
    auto candidate = read_jpeg_file_candidate(path);
    if (!candidate)
    {
        return candidate.error();
    }
    if (candidate.value().recognized)
    {
        return probe_jpeg_bytes(candidate.value().bytes, path);
    }
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
    auto candidate = read_jpeg_file_candidate(path, &cancellation);
    if (!candidate)
    {
        return candidate.error();
    }
    if (candidate.value().recognized)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return decode_jpeg_bytes(candidate.value().bytes, max_edge, cancellation, path, 0);
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
    if (is_jpeg_payload(byte_span(bytes)))
    {
        return decode_jpeg_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
    }
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open embedded preview payload");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
    {
        return make_error(
            ErrorCode::kUnsupported, "Embedded preview is not a readable raster image",
            {{"source", "memory"}, {"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    if (!is_allowed_raster_format(reader.format()))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Embedded preview format is not enabled for import",
                          {{"format", reader.format().toStdString()}, {"source", "memory"}});
    }
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
