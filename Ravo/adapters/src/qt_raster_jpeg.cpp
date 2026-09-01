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
read_jpeg_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
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
    const QSize source_size =
        apply_display_rotation_to_size(transformed_reader_size(reader), rotate_quarters);
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
                         std::move(contract).value().color_profile, source_size);
}

} // namespace ravo::qt_raster_internal
