#include "ravo/adapters/qt_raster_decoder.h"

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
inline constexpr std::array<std::uint8_t, 8> kPngSignature{0x89U, 'P',   'N',   'G',
                                                           0x0DU, 0x0AU, 0x1AU, 0x0AU};
inline constexpr std::uint64_t kPngMaxEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kPngMaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kPngMaxIccBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kCancellationCheckBytes = 64U * 1024U;
// QColorSpace's ICC LUT round-trip can differ by a few 16-bit code points even
// for the frozen matching encoding. Keep the bound below one eighth of an RGB8
// code so a redundant cICP declaration cannot change the published pixels.
inline constexpr std::uint16_t kPngProfileCompatibilityTolerance = 32U;

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

struct PngContract
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    bool has_alpha = false;
    std::uint16_t orientation = 1;
    ColorProfileState color_profile;
};

struct PngFileCandidate
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

[[nodiscard]] bool is_png_payload(const std::span<const std::uint8_t> bytes) noexcept
{
    return starts_with(bytes, kPngSignature);
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

[[nodiscard]] TaskError png_error(const ErrorCode code, std::string message,
                                  const std::string_view source, const std::string_view reason,
                                  std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "png");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
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

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                                     const bool little_endian) noexcept
{
    if (little_endian)
    {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                          (static_cast<std::uint16_t>(bytes[1]) << 8U));
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                                     const bool little_endian) noexcept
{
    if (little_endian)
    {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }
    return read_u32_be(bytes);
}

[[nodiscard]] Result<std::uint16_t>
png_exif_orientation(const std::span<const std::uint8_t> payload, const std::string_view source,
                     const CancellationToken *const cancellation)
{
    if (payload.size() < 8U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf TIFF header is truncated", source,
                         "truncated_png_exif_header");
    }
    const bool little_endian = payload[0] == 'I' && payload[1] == 'I';
    const bool big_endian = payload[0] == 'M' && payload[1] == 'M';
    if ((!little_endian && !big_endian) || read_u16(payload.subspan(2U, 2U), little_endian) != 42U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf TIFF header is malformed", source,
                         "malformed_png_exif_header");
    }
    const std::uint32_t ifd_offset = read_u32(payload.subspan(4U, 4U), little_endian);
    if (ifd_offset > payload.size() || payload.size() - ifd_offset < 2U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf IFD offset is invalid", source,
                         "invalid_png_exif_ifd_offset");
    }
    const std::size_t entry_count =
        read_u16(payload.subspan(static_cast<std::size_t>(ifd_offset), 2U), little_endian);
    const std::size_t entries_begin = static_cast<std::size_t>(ifd_offset) + 2U;
    if (entry_count > (payload.size() - entries_begin) / 12U)
    {
        return png_error(ErrorCode::kValidation, "PNG eXIf IFD entries are truncated", source,
                         "truncated_png_exif_ifd");
    }
    std::optional<std::uint16_t> orientation;
    for (std::size_t index = 0U; index < entry_count; ++index)
    {
        if ((index & 0xFFFU) == 0U && cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                return active.error();
            }
        }
        const auto entry = payload.subspan(entries_begin + index * 12U, 12U);
        if (read_u16(entry.first(2U), little_endian) != 0x0112U)
        {
            continue;
        }
        if (orientation || read_u16(entry.subspan(2U, 2U), little_endian) != 3U ||
            read_u32(entry.subspan(4U, 4U), little_endian) != 1U)
        {
            return png_error(ErrorCode::kValidation,
                             "PNG eXIf orientation entry is duplicated or malformed", source,
                             "malformed_png_exif_orientation");
        }
        const std::uint16_t value = read_u16(entry.subspan(8U, 2U), little_endian);
        if (value < 1U || value > 8U)
        {
            return png_error(ErrorCode::kValidation, "PNG eXIf orientation is out of range", source,
                             "invalid_png_exif_orientation",
                             {{"orientation", std::to_string(value)}});
        }
        orientation = value;
    }
    return orientation.value_or(1U);
}

[[nodiscard]] QColorSpace png_builtin_color_space(const std::string_view identifier)
{
    if (identifier == "srgb")
    {
        return QColorSpace(QColorSpace::SRgb);
    }
    if (identifier == "linear_rec709")
    {
        return QColorSpace(QColorSpace::SRgbLinear);
    }
    if (identifier == "rec709")
    {
        return QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Bt2020);
    }
    if (identifier == "linear_rec2020")
    {
        return QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear);
    }
    if (identifier == "pq_rec2020")
    {
        return QColorSpace(QColorSpace::Bt2100Pq);
    }
    if (identifier == "hlg_rec2020")
    {
        return QColorSpace(QColorSpace::Bt2100Hlg);
    }
    if (identifier == "display_p3")
    {
        return QColorSpace(QColorSpace::DisplayP3);
    }
    if (identifier == "pq_p3")
    {
        return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::St2084);
    }
    if (identifier == "hlg_p3")
    {
        return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Hlg);
    }
    return {};
}

[[nodiscard]] std::uint16_t png_profile_max_difference(const QColorSpace &icc,
                                                       const std::string_view cicp_identifier)
{
    const QColorSpace cicp = png_builtin_color_space(cicp_identifier);
    if (!icc.isValid() || !cicp.isValid())
    {
        return std::numeric_limits<std::uint16_t>::max();
    }
    const auto difference = [](const QRgba64 left, const QRgba64 right) noexcept
    {
        return std::max({std::abs(static_cast<int>(left.red()) - static_cast<int>(right.red())),
                         std::abs(static_cast<int>(left.green()) - static_cast<int>(right.green())),
                         std::abs(static_cast<int>(left.blue()) - static_cast<int>(right.blue()))});
    };
    const QColorTransform to_cicp = icc.transformationToColorSpace(cicp);
    const QColorTransform to_icc = cicp.transformationToColorSpace(icc);
    static constexpr std::array<std::uint16_t, 5> kSamples{0U, 8192U, 32768U, 49152U, 65535U};
    int maximum = 0;
    for (const std::uint16_t red : kSamples)
    {
        for (const std::uint16_t green : kSamples)
        {
            for (const std::uint16_t blue : kSamples)
            {
                const QRgba64 sample = QRgba64::fromRgba64(red, green, blue, 65535U);
                maximum = std::max(maximum, difference(to_cicp.map(sample), sample));
                maximum = std::max(maximum, difference(to_icc.map(sample), sample));
            }
        }
    }
    return static_cast<std::uint16_t>(
        std::min(maximum, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
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

[[nodiscard]] Result<std::vector<std::uint8_t>>
inflate_png_icc_profile(const std::span<const std::uint8_t> compressed,
                        const std::string_view source, const CancellationToken *const cancellation)
{
    if (compressed.empty() || compressed.size() > std::numeric_limits<uInt>::max())
    {
        return png_error(ErrorCode::kValidation, "PNG iCCP payload is empty or too large", source,
                         "invalid_png_iccp_payload");
    }

    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit(&stream) != Z_OK)
    {
        return png_error(ErrorCode::kInternal, "Unable to initialize PNG ICC decompression", source,
                         "png_iccp_inflate_init_failed");
    }

    std::vector<std::uint8_t> profile;
    // Keep the output window small enough that ordinary ICC fixtures exercise
    // incremental inflate progress instead of only the one-shot path.
    std::array<std::uint8_t, 256U> output{};
    int status = Z_OK;
    while (status != Z_STREAM_END)
    {
        if (cancellation != nullptr)
        {
            auto active = cancellation->check();
            if (!active)
            {
                inflateEnd(&stream);
                return active.error();
            }
        }
        stream.next_out = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        const uInt input_before = stream.avail_in;
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
        {
            inflateEnd(&stream);
            return png_error(ErrorCode::kValidation, "PNG iCCP profile is corrupt", source,
                             "corrupt_png_iccp_profile");
        }
        const std::size_t produced = output.size() - stream.avail_out;
        if (produced > kPngMaxIccBytes - profile.size())
        {
            inflateEnd(&stream);
            return png_error(ErrorCode::kValidation, "PNG iCCP profile is too large", source,
                             "oversized_png_iccp_profile");
        }
        profile.insert(profile.end(), output.begin(),
                       output.begin() + static_cast<std::ptrdiff_t>(produced));
        if (produced == 0U && stream.avail_in == input_before && status != Z_STREAM_END)
        {
            inflateEnd(&stream);
            return png_error(ErrorCode::kValidation, "PNG iCCP profile is truncated", source,
                             "truncated_png_iccp_profile");
        }
    }
    const bool has_trailing_bytes = stream.avail_in != 0U;
    inflateEnd(&stream);
    if (has_trailing_bytes || profile.empty())
    {
        return png_error(ErrorCode::kValidation, "PNG iCCP profile payload is malformed", source,
                         "malformed_png_iccp_profile");
    }
    return profile;
}

[[nodiscard]] Result<PngContract>
parse_png_contract(const std::span<const std::uint8_t> bytes, const std::string_view source,
                   const CancellationToken *const cancellation = nullptr)
{
    if (!is_png_payload(bytes))
    {
        return png_error(ErrorCode::kUnsupported, "Input is not a PNG image", source,
                         "unrecognized_png_content");
    }

    const auto invalid =
        [&](std::string message, const std::string_view reason,
            std::map<std::string, std::string, std::less<>> context = {}) -> Result<PngContract>
    {
        return png_error(ErrorCode::kValidation, std::move(message), source, reason,
                         std::move(context));
    };
    const auto unsupported =
        [&](std::string message, const std::string_view reason,
            std::map<std::string, std::string, std::less<>> context = {}) -> Result<PngContract>
    {
        return png_error(ErrorCode::kUnsupported, std::move(message), source, reason,
                         std::move(context));
    };

    PngContract result;
    std::size_t position = kPngSignature.size();
    bool saw_ihdr = false;
    bool saw_plte = false;
    bool saw_idat = false;
    bool ended_idat = false;
    bool saw_iend = false;
    bool saw_iccp = false;
    bool saw_srgb = false;
    bool saw_cicp = false;
    bool saw_trns = false;
    bool saw_exif = false;
    bool saw_gama = false;
    bool saw_chrm = false;
    std::uint32_t gamma_value = 0U;
    std::optional<QColorSpace> iccp_color_space;
    std::optional<std::string> cicp_identifier;
    std::size_t palette_entries = 0U;

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
        if (bytes.size() - position < 12U)
        {
            return invalid("PNG chunk framing is truncated", "truncated_png_chunk_header",
                           {{"offset", std::to_string(position)}});
        }
        const std::uint32_t length = read_u32_be(bytes.subspan(position, 4U));
        const auto type = bytes.subspan(position + 4U, 4U);
        const std::string name = png_chunk_name(type);
        if (static_cast<std::size_t>(length) > bytes.size() - position - 12U)
        {
            return invalid("PNG chunk payload is truncated", "truncated_png_chunk_payload",
                           {{"chunk", name}, {"offset", std::to_string(position)}});
        }
        for (const auto character : type)
        {
            if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')))
            {
                return invalid("PNG chunk type is malformed", "malformed_png_chunk_type",
                               {{"offset", std::to_string(position)}});
            }
        }
        if ((type[2] & 0x20U) != 0U)
        {
            return invalid("PNG chunk type has an invalid reserved bit",
                           "invalid_png_chunk_reserved_bit", {{"chunk", name}});
        }

        const auto payload = bytes.subspan(position + 8U, length);
        const std::uint32_t stored_crc = read_u32_be(bytes.subspan(position + 8U + length, 4U));
        uLong calculated_crc = crc32(0L, Z_NULL, 0);
        calculated_crc = crc32(calculated_crc, reinterpret_cast<const Bytef *>(type.data()), 4U);
        std::size_t crc_position = 0U;
        while (crc_position < payload.size())
        {
            if (cancellation != nullptr)
            {
                auto active = cancellation->check();
                if (!active)
                {
                    return active.error();
                }
            }
            const std::size_t count =
                std::min(kCancellationCheckBytes, payload.size() - crc_position);
            calculated_crc = crc32(calculated_crc,
                                   reinterpret_cast<const Bytef *>(payload.data() + crc_position),
                                   static_cast<uInt>(count));
            crc_position += count;
        }
        if (static_cast<std::uint32_t>(calculated_crc) != stored_crc)
        {
            return invalid("PNG chunk CRC does not match its payload", "png_chunk_crc_mismatch",
                           {{"chunk", name}, {"offset", std::to_string(position)}});
        }
        position += 12U + static_cast<std::size_t>(length);

        if (!saw_ihdr && name != "IHDR")
        {
            return invalid("PNG IHDR must be the first chunk", "missing_png_ihdr");
        }
        if (saw_idat && name != "IDAT")
        {
            ended_idat = true;
        }

        if (name == "IHDR")
        {
            if (saw_ihdr)
            {
                return invalid("PNG contains more than one IHDR", "duplicate_png_ihdr");
            }
            if (length != 13U)
            {
                return invalid("PNG IHDR has an invalid size", "invalid_png_ihdr_size");
            }
            result.width = read_u32_be(payload.first(4U));
            result.height = read_u32_be(payload.subspan(4U, 4U));
            result.bit_depth = payload[8];
            result.color_type = payload[9];
            if (result.width == 0U || result.height == 0U ||
                result.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                result.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                return invalid("PNG image has invalid dimensions", "invalid_png_dimensions",
                               {{"height", std::to_string(result.height)},
                                {"width", std::to_string(result.width)}});
            }
            const std::uint64_t native_bytes_per_pixel = result.bit_depth == 16U ? 8U : 4U;
            const std::uint64_t worst_case_native_bytes =
                static_cast<std::uint64_t>(result.width) *
                static_cast<std::uint64_t>(result.height) * native_bytes_per_pixel;
            if (worst_case_native_bytes > kPngMaxDecodedBytes)
            {
                return invalid(
                    "PNG native decoded dimensions exceed the allocation bound",
                    "png_dimensions_exceed_allocation_limit",
                    {{"native_bytes_per_pixel", std::to_string(native_bytes_per_pixel)},
                     {"worst_case_native_bytes", std::to_string(worst_case_native_bytes)},
                     {"limit_bytes", std::to_string(kPngMaxDecodedBytes)}});
            }
            const bool legal_depth =
                (result.color_type == 0U && (result.bit_depth == 8U || result.bit_depth == 16U)) ||
                (result.color_type == 2U && (result.bit_depth == 8U || result.bit_depth == 16U)) ||
                (result.color_type == 3U && result.bit_depth == 8U) ||
                (result.color_type == 4U && (result.bit_depth == 8U || result.bit_depth == 16U)) ||
                (result.color_type == 6U && (result.bit_depth == 8U || result.bit_depth == 16U));
            if (result.color_type != 0U && result.color_type != 2U && result.color_type != 3U &&
                result.color_type != 4U && result.color_type != 6U)
            {
                return unsupported("PNG color layout is unsupported", "unsupported_png_color_type",
                                   {{"color_type", std::to_string(result.color_type)}});
            }
            if (!legal_depth && (result.color_type == 0U || result.color_type == 3U) &&
                (result.bit_depth == 1U || result.bit_depth == 2U || result.bit_depth == 4U))
            {
                return unsupported("Packed low-bit PNG input is unsupported",
                                   "unsupported_png_packed_bit_depth",
                                   {{"bit_depth", std::to_string(result.bit_depth)},
                                    {"color_type", std::to_string(result.color_type)}});
            }
            if (!legal_depth)
            {
                return invalid("PNG bit depth is invalid for its color layout",
                               "invalid_png_bit_depth",
                               {{"bit_depth", std::to_string(result.bit_depth)},
                                {"color_type", std::to_string(result.color_type)}});
            }
            if (payload[10] != 0U || payload[11] != 0U || payload[12] > 1U)
            {
                return unsupported("PNG compression, filter, or interlace mode is unsupported",
                                   "unsupported_png_encoding",
                                   {{"compression", std::to_string(payload[10])},
                                    {"filter", std::to_string(payload[11])},
                                    {"interlace", std::to_string(payload[12])}});
            }
            if (payload[12] == 1U)
            {
                return unsupported("Adam7-interlaced PNG input is unsupported",
                                   "unsupported_png_adam7");
            }
            result.has_alpha = result.color_type == 4U || result.color_type == 6U;
            saw_ihdr = true;
        }
        else if (name == "PLTE")
        {
            if (saw_plte || saw_idat)
            {
                return invalid("PNG PLTE is duplicated or out of order", "invalid_png_plte_order");
            }
            if (result.color_type == 0U || result.color_type == 4U)
            {
                return invalid("PNG grayscale image must not contain PLTE", "forbidden_png_plte");
            }
            if (length == 0U || length % 3U != 0U || length > 768U)
            {
                return invalid("PNG PLTE has an invalid size", "invalid_png_plte_size");
            }
            palette_entries = length / 3U;
            if (result.color_type == 3U &&
                palette_entries > (static_cast<std::size_t>(1U) << result.bit_depth))
            {
                return invalid("PNG palette has too many entries for its bit depth",
                               "oversized_png_palette");
            }
            saw_plte = true;
        }
        else if (name == "IDAT")
        {
            if (ended_idat)
            {
                return invalid("PNG IDAT chunks must be consecutive", "nonconsecutive_png_idat");
            }
            if (result.color_type == 3U && !saw_plte)
            {
                return invalid("Indexed PNG is missing its palette", "missing_png_plte");
            }
            saw_idat = true;
        }
        else if (name == "IEND")
        {
            if (length != 0U || !saw_idat)
            {
                return invalid("PNG IEND is malformed or precedes image data", "invalid_png_iend");
            }
            saw_iend = true;
            break;
        }
        else if (name == "iCCP")
        {
            if (saw_iccp || saw_srgb || saw_plte || saw_idat)
            {
                return invalid("PNG iCCP is duplicated, conflicting, or out of order",
                               "invalid_png_iccp_order");
            }
            const auto keyword = payload.first(std::min<std::size_t>(payload.size(), 80U));
            const auto separator = std::find(keyword.begin(), keyword.end(), 0U);
            const std::size_t keyword_size = static_cast<std::size_t>(separator - payload.begin());
            if (keyword_size == 0U || keyword_size > 79U || separator == keyword.end() ||
                payload.size() < keyword_size + 3U || payload[keyword_size + 1U] != 0U)
            {
                return invalid("PNG iCCP header is malformed", "malformed_png_iccp_header");
            }
            auto profile =
                inflate_png_icc_profile(payload.subspan(keyword_size + 2U), source, cancellation);
            if (!profile)
            {
                return profile.error();
            }
            if (profile.value().size() >
                static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
            {
                return invalid("PNG iCCP profile is too large", "oversized_png_iccp_profile");
            }
            const QByteArray profile_bytes(reinterpret_cast<const char *>(profile.value().data()),
                                           static_cast<qsizetype>(profile.value().size()));
            const QColorSpace color_space = QColorSpace::fromIccProfile(profile_bytes);
            if (!color_space.isValid())
            {
                return invalid("PNG iCCP profile is corrupt", "corrupt_png_iccp_profile");
            }
            if (color_space.colorModel() != QColorSpace::ColorModel::Rgb)
            {
                return unsupported("PNG iCCP profile is not an RGB profile",
                                   "unsupported_png_iccp_color_model");
            }
            const std::uint16_t profile_difference =
                cicp_identifier ? png_profile_max_difference(color_space, *cicp_identifier) : 0U;
            if (profile_difference > kPngProfileCompatibilityTolerance)
            {
                return invalid("PNG iCCP and cICP declarations conflict",
                               "conflicting_png_color_profiles",
                               {{"cicp_profile", *cicp_identifier},
                                {"max_channel_difference", std::to_string(profile_difference)}});
            }
            iccp_color_space = color_space;
            result.color_profile.kind = ColorProfileKind::kIcc;
            result.color_profile.model = ColorModel::kRgb;
            result.color_profile.identifier = "embedded_icc";
            result.color_profile.icc_bytes = std::move(profile).value();
            saw_iccp = true;
        }
        else if (name == "sRGB")
        {
            if (saw_srgb || saw_iccp || saw_plte || saw_idat || length != 1U || payload[0] > 3U)
            {
                return invalid("PNG sRGB chunk is malformed, conflicting, or out of order",
                               "invalid_png_srgb");
            }
            if (saw_cicp && result.color_profile.identifier != "srgb")
            {
                return invalid("PNG sRGB and cICP declarations conflict",
                               "conflicting_png_color_profiles",
                               {{"cicp_profile", result.color_profile.identifier}});
            }
            result.color_profile.kind = ColorProfileKind::kBuiltin;
            result.color_profile.model = ColorModel::kRgb;
            result.color_profile.identifier = "srgb";
            saw_srgb = true;
        }
        else if (name == "cICP")
        {
            if (saw_cicp || saw_plte || saw_idat || length != 4U)
            {
                return invalid("PNG cICP is malformed, conflicting, or out of order",
                               "invalid_png_cicp");
            }
            if (payload[2] != 0U || payload[3] != 1U)
            {
                return unsupported("PNG cICP must declare full-range RGB pixels",
                                   "unsupported_png_cicp_layout",
                                   {{"matrix", std::to_string(payload[2])},
                                    {"range", std::to_string(payload[3])}});
            }
            const std::pair<std::uint8_t, std::uint8_t> profile_codes{payload[0], payload[1]};
            static constexpr std::array<
                std::pair<std::pair<std::uint8_t, std::uint8_t>, std::string_view>, 9>
                kCicpProfiles{{{{1U, 13U}, "srgb"},
                               {{1U, 8U}, "linear_rec709"},
                               {{1U, 1U}, "rec709"},
                               {{9U, 8U}, "linear_rec2020"},
                               {{9U, 16U}, "pq_rec2020"},
                               {{9U, 18U}, "hlg_rec2020"},
                               {{12U, 13U}, "display_p3"},
                               {{12U, 16U}, "pq_p3"},
                               {{12U, 18U}, "hlg_p3"}}};
            const auto mapped =
                std::find_if(kCicpProfiles.begin(), kCicpProfiles.end(),
                             [&](const auto &entry) { return entry.first == profile_codes; });
            if (mapped == kCicpProfiles.end())
            {
                return unsupported("PNG cICP primaries and transfer are unsupported",
                                   "unsupported_png_cicp_profile",
                                   {{"primaries", std::to_string(payload[0])},
                                    {"transfer", std::to_string(payload[1])}});
            }
            if (saw_srgb && mapped->second != "srgb")
            {
                return invalid("PNG sRGB and cICP declarations conflict",
                               "conflicting_png_color_profiles",
                               {{"cicp_profile", std::string(mapped->second)}});
            }
            const std::uint16_t profile_difference =
                iccp_color_space ? png_profile_max_difference(*iccp_color_space, mapped->second) :
                                   0U;
            if (profile_difference > kPngProfileCompatibilityTolerance)
            {
                return invalid("PNG iCCP and cICP declarations conflict",
                               "conflicting_png_color_profiles",
                               {{"cicp_profile", std::string(mapped->second)},
                                {"max_channel_difference", std::to_string(profile_difference)}});
            }
            cicp_identifier = std::string(mapped->second);
            if (!saw_iccp)
            {
                result.color_profile.kind = ColorProfileKind::kBuiltin;
                result.color_profile.model = ColorModel::kRgb;
                result.color_profile.identifier = *cicp_identifier;
                result.color_profile.icc_bytes.clear();
            }
            saw_cicp = true;
        }
        else if (name == "tRNS")
        {
            if (saw_trns || saw_idat || result.color_type == 4U || result.color_type == 6U)
            {
                return invalid("PNG tRNS is duplicated, out of order, or conflicts with alpha",
                               "invalid_png_trns");
            }
            const bool valid_size = (result.color_type == 0U && length == 2U) ||
                                    (result.color_type == 2U && length == 6U) ||
                                    (result.color_type == 3U && saw_plte && length > 0U &&
                                     static_cast<std::size_t>(length) <= palette_entries);
            if (!valid_size)
            {
                return invalid("PNG tRNS has an invalid size or ordering", "invalid_png_trns_size");
            }
            result.has_alpha = true;
            saw_trns = true;
        }
        else if (name == "eXIf")
        {
            if (saw_exif || saw_idat)
            {
                return invalid("PNG eXIf is duplicated or out of order", "invalid_png_exif_order");
            }
            auto orientation = png_exif_orientation(payload, source, cancellation);
            if (!orientation)
            {
                return orientation.error();
            }
            result.orientation = orientation.value();
            saw_exif = true;
        }
        else if (name == "gAMA" || name == "cHRM")
        {
            bool &seen = name == "gAMA" ? saw_gama : saw_chrm;
            const std::uint32_t expected_size = name == "gAMA" ? 4U : 32U;
            if (seen || saw_plte || saw_idat || length != expected_size)
            {
                return invalid("PNG legacy color metadata is malformed or out of order",
                               "invalid_png_legacy_color_metadata", {{"chunk", name}});
            }
            seen = true;
            if (name == "gAMA")
            {
                gamma_value = read_u32_be(payload);
            }
        }
        else if ((type[0] & 0x20U) == 0U)
        {
            return unsupported("PNG contains an unsupported critical chunk",
                               "unsupported_png_critical_chunk", {{"chunk", name}});
        }
    }

    if (!saw_ihdr || !saw_idat || !saw_iend || position != bytes.size())
    {
        return invalid("PNG image is truncated, incomplete, or has trailing data",
                       "incomplete_png_stream",
                       {{"idat", saw_idat ? "present" : "missing"},
                        {"iend", saw_iend ? "present" : "missing"},
                        {"trailing_bytes", position == bytes.size() ? "absent" : "present"}});
    }
    if (saw_chrm ||
        (saw_gama && (result.color_profile.identifier != "srgb" || gamma_value != 45455U)))
    {
        return unsupported("PNG gAMA/cHRM metadata cannot be represented exactly or redundantly",
                           "unsupported_png_legacy_color_profile");
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

[[nodiscard]] Result<PngFileCandidate>
read_png_file_candidate(const std::string_view path,
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
    const QByteArray prefix = file.peek(static_cast<qint64>(kPngSignature.size()));
    PngFileCandidate result;
    result.recognized = is_png_payload(byte_span(prefix));
    if (!result.recognized)
    {
        return result;
    }
    if (info.size() < 0 || info.size() > std::numeric_limits<qsizetype>::max() ||
        static_cast<std::uint64_t>(info.size()) > kPngMaxEncodedBytes)
    {
        return png_error(ErrorCode::kValidation, "PNG input is too large", path,
                         "oversized_png_input", {{"path", std::string(path)}});
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
            return png_error(ErrorCode::kIo, "PNG input read made no progress", path,
                             "png_read_stalled", {{"path", std::string(path)}});
        }
        result.bytes.append(chunk);
    }
    if (file.error() != QFileDevice::NoError || result.bytes.size() != info.size())
    {
        return png_error(
            ErrorCode::kIo, "Unable to read complete PNG input", path, "png_read_failed",
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

[[nodiscard]] Result<RasterInfo> probe_png_bytes(const QByteArray &bytes,
                                                 const std::string_view source)
{
    auto contract = parse_png_contract(byte_span(bytes), source);
    if (!contract)
    {
        return contract.error();
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return png_error(ErrorCode::kIo, "Unable to open PNG input buffer", source,
                         "png_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || reader.format().toLower() != QByteArrayLiteral("png"))
    {
        return png_error(ErrorCode::kValidation, "Unable to read PNG image header", source,
                         "png_header_decode_failed",
                         {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    const QSize native_size = reader.size();
    if (native_size.width() != static_cast<int>(contract.value().width) ||
        native_size.height() != static_cast<int>(contract.value().height))
    {
        return png_error(ErrorCode::kValidation, "PNG decoder dimensions disagree with IHDR",
                         source, "png_dimension_mismatch",
                         {{"ihdr_height", std::to_string(contract.value().height)},
                          {"ihdr_width", std::to_string(contract.value().width)},
                          {"qt_height", std::to_string(native_size.height())},
                          {"qt_width", std::to_string(native_size.width())}});
    }
    QSize expected_size = native_size;
    if (contract.value().orientation >= 5U)
    {
        expected_size.transpose();
    }
    QImage image = reader.read();
    if (image.isNull() || reader.error() != QImageReader::UnknownError)
    {
        return png_error(ErrorCode::kValidation, "Unable to decode complete PNG image", source,
                         "png_pixel_decode_failed",
                         {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    image = apply_png_orientation(std::move(image), contract.value().orientation);
    if (expected_size.width() <= 0 || expected_size.height() <= 0 || image.size() != expected_size)
    {
        return png_error(ErrorCode::kValidation,
                         "PNG decoder produced unexpected transformed dimensions", source,
                         "png_transformed_dimension_mismatch",
                         {{"actual_height", std::to_string(image.height())},
                          {"actual_width", std::to_string(image.width())},
                          {"expected_height", std::to_string(expected_size.height())},
                          {"expected_width", std::to_string(expected_size.width())}});
    }
    RasterInfo info;
    info.media_type = std::string(kMediaTypePng);
    info.width = static_cast<std::uint32_t>(image.width());
    info.height = static_cast<std::uint32_t>(image.height());
    return info;
}

[[nodiscard]] std::uint8_t png_channel_to_u8(const std::uint16_t value) noexcept
{
    return static_cast<std::uint8_t>(value >> 8U);
}

[[nodiscard]] Result<DecodedRaster> decode_png_raster(QImage image, const std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      const std::string_view source,
                                                      ColorProfileState color_profile)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (image.isNull())
    {
        return png_error(ErrorCode::kValidation, "Unable to decode complete PNG image", source,
                         "png_pixel_decode_failed");
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
        return png_error(ErrorCode::kValidation, "Unable to normalize PNG pixels", source,
                         "png_pixel_normalization_failed");
    }

    DecodedRaster result;
    result.width = static_cast<std::uint32_t>(image.width());
    result.height = static_cast<std::uint32_t>(image.height());
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

[[nodiscard]] Result<DecodedRaster> decode_png_bytes(const QByteArray &bytes,
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
    auto contract = parse_png_contract(byte_span(bytes), source, &cancellation);
    if (!contract)
    {
        return contract.error();
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return png_error(ErrorCode::kIo, "Unable to open PNG input buffer", source,
                         "png_buffer_open_failed");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead() || reader.format().toLower() != QByteArrayLiteral("png"))
    {
        return png_error(ErrorCode::kValidation, "Unable to read PNG image header", source,
                         "png_header_decode_failed",
                         {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    const QSize native_size = reader.size();
    if (native_size.width() != static_cast<int>(contract.value().width) ||
        native_size.height() != static_cast<int>(contract.value().height))
    {
        return png_error(ErrorCode::kValidation, "PNG decoder dimensions disagree with IHDR",
                         source, "png_dimension_mismatch",
                         {{"ihdr_height", std::to_string(contract.value().height)},
                          {"ihdr_width", std::to_string(contract.value().width)},
                          {"qt_height", std::to_string(native_size.height())},
                          {"qt_width", std::to_string(native_size.width())}});
    }
    apply_scaled_decode_size(reader, max_edge);
    QImage image = reader.read();
    if (image.isNull() || reader.error() != QImageReader::UnknownError)
    {
        return png_error(ErrorCode::kValidation, "Unable to decode complete PNG image", source,
                         "png_pixel_decode_failed",
                         {{"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    image = apply_png_orientation(std::move(image), contract.value().orientation);
    image = apply_display_rotation(std::move(image), rotate_quarters);
    return decode_png_raster(std::move(image), max_edge, cancellation, source,
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
    auto png_candidate = read_png_file_candidate(path);
    if (!png_candidate)
    {
        return png_candidate.error();
    }
    if (png_candidate.value().recognized)
    {
        return probe_png_bytes(png_candidate.value().bytes, path);
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
    auto png_candidate = read_png_file_candidate(path, &cancellation);
    if (!png_candidate)
    {
        return png_candidate.error();
    }
    if (png_candidate.value().recognized)
    {
        return decode_png_bytes(png_candidate.value().bytes, max_edge, cancellation, path, 0);
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
    const std::span<const std::uint8_t> encoded_bytes(encoded);
    if (is_png_payload(encoded_bytes) && encoded.size() > kPngMaxEncodedBytes)
    {
        return png_error(ErrorCode::kValidation, "PNG input is too large", "memory",
                         "oversized_png_input");
    }
    QByteArray bytes(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<qsizetype>(encoded.size()));
    if (is_jpeg_payload(byte_span(bytes)))
    {
        return decode_jpeg_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
    }
    if (is_png_payload(byte_span(bytes)))
    {
        return decode_png_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
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
