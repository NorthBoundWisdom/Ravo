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
            const std::size_t keyword_size = static_cast<std::size_t>(separator - keyword.begin());
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

[[nodiscard]] Result<PngFileCandidate>
read_png_file_candidate(const std::string_view path, const CancellationToken *const cancellation)
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
        return png_error(ErrorCode::kValidation, "Unable to decode complete PNG image", source,
                         "png_pixel_decode_failed");
    }
    if (!source_size.isValid() || source_size.width() <= 0 || source_size.height() <= 0)
    {
        return png_error(ErrorCode::kValidation, "PNG source dimensions are invalid", source,
                         "invalid_png_source_dimensions");
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
    QSize source_size = native_size;
    if (contract.value().orientation >= 5U)
    {
        source_size.transpose();
    }
    source_size = apply_display_rotation_to_size(source_size, rotate_quarters);
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
                             std::move(contract).value().color_profile, source_size);
}

} // namespace ravo::qt_raster_internal
