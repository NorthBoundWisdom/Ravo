#include "ravo/adapters/radiance_rgbe_decoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QString>

namespace ravo
{
namespace
{

inline constexpr std::string_view kRadianceMagic = "#?RADIANCE\n";
inline constexpr std::string_view kRgbeMagic = "#?RGBE\n";
inline constexpr std::string_view kRgbeFormat = "FORMAT=32-bit_rle_rgbe";
inline constexpr std::uint64_t kHardMaxEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kHardMaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kHardMaxHeaderBytes = 1024U * 1024U;
inline constexpr std::size_t kHardMaxHeaderLineBytes = 127U;
inline constexpr std::size_t kFloatBytesPerPixel = 3U * sizeof(float);
inline constexpr std::size_t kFileReadChunkBytes = 256U * 1024U;
inline constexpr std::size_t kPixelCancellationGranularity = 1024U;

using ByteSpan = std::span<const std::uint8_t>;

struct DecodeContext
{
    const CancellationToken &cancellation;
    RadianceRgbeCheckpointObserver observer;

    [[nodiscard]] Result<void> checkpoint(const RadianceRgbeDecodeCheckpoint checkpoint,
                                          const std::size_t progress) const
    {
        if (observer.callback != nullptr)
        {
            observer.callback(observer.context, checkpoint, progress);
        }
        return cancellation.check();
    }
};

struct ParsedFloat
{
    float value = 0.0F;
    std::size_t consumed = 0U;
};

struct ParsedHeader
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::size_t pixel_offset = 0U;
    RadianceRgbeMetadata metadata;
};

struct HeaderCursor
{
    ByteSpan bytes;
    std::size_t offset = 0U;
};

[[nodiscard]] TaskError rgbe_error(const ErrorCode code, std::string message,
                                   const std::string_view source, const std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "rgbe");
    context.emplace("reason", reason);
    context.emplace("source", source);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] RadianceRgbeDecodeLimits sanitized_limits(RadianceRgbeDecodeLimits limits) noexcept
{
    limits.max_encoded_bytes = std::min(limits.max_encoded_bytes, kHardMaxEncodedBytes);
    limits.max_decoded_bytes = std::min(limits.max_decoded_bytes, kHardMaxDecodedBytes);
    limits.max_header_bytes = std::min(limits.max_header_bytes, kHardMaxHeaderBytes);
    limits.max_header_line_bytes =
        std::min({limits.max_header_line_bytes, kHardMaxHeaderLineBytes, limits.max_header_bytes});
    return limits;
}

[[nodiscard]] bool starts_with(const ByteSpan bytes, const std::string_view prefix) noexcept
{
    return bytes.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), reinterpret_cast<const char *>(bytes.data()));
}

[[nodiscard]] std::optional<std::string_view> rgbe_program_type(const ByteSpan bytes) noexcept
{
    if (starts_with(bytes, kRadianceMagic))
    {
        return "RADIANCE";
    }
    if (starts_with(bytes, kRgbeMagic))
    {
        return "RGBE";
    }
    return std::nullopt;
}

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return {};
    }
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] Result<std::string_view> read_header_line(HeaderCursor &cursor,
                                                        const RadianceRgbeDecodeLimits &limits,
                                                        const DecodeContext &decode,
                                                        const std::string_view source)
{
    if (cursor.offset >= cursor.bytes.size())
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE header is truncated", source,
                          "truncated_rgbe_header");
    }
    const std::size_t begin = cursor.offset;
    std::size_t end = begin;
    while (end < cursor.bytes.size() && cursor.bytes[end] != static_cast<std::uint8_t>('\n'))
    {
        ++end;
        if (end - begin > limits.max_header_line_bytes)
        {
            return rgbe_error(
                ErrorCode::kValidation, "Radiance RGBE header line exceeds the fixed limit", source,
                "oversized_rgbe_header_line",
                {{"max_header_line_bytes", std::to_string(limits.max_header_line_bytes)}});
        }
        if (end > limits.max_header_bytes)
        {
            return rgbe_error(ErrorCode::kValidation,
                              "Radiance RGBE header exceeds the fixed limit", source,
                              "oversized_rgbe_header",
                              {{"max_header_bytes", std::to_string(limits.max_header_bytes)}});
        }
    }
    if (end >= cursor.bytes.size())
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE header is truncated", source,
                          "truncated_rgbe_header");
    }
    const std::size_t next = end + 1U;
    if (next > limits.max_header_bytes)
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE header exceeds the fixed limit",
                          source, "oversized_rgbe_header",
                          {{"max_header_bytes", std::to_string(limits.max_header_bytes)}});
    }
    cursor.offset = next;
    auto active = decode.checkpoint(RadianceRgbeDecodeCheckpoint::kHeader, cursor.offset);
    if (!active)
    {
        return active.error();
    }
    return std::string_view(reinterpret_cast<const char *>(cursor.bytes.data() + begin),
                            end - begin);
}

[[nodiscard]] bool is_ascii_space(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] std::optional<ParsedFloat> parse_float_prefix(const std::string_view text) noexcept
{
    std::size_t offset = 0U;
    while (offset < text.size() && is_ascii_space(text[offset]))
    {
        ++offset;
    }
    if (offset < text.size() && text[offset] == '+')
    {
        ++offset;
    }
    if (offset >= text.size())
    {
        return std::nullopt;
    }
    float value = 0.0F;
    const char *const begin = text.data() + static_cast<std::ptrdiff_t>(offset);
    const char *const end = text.data() + static_cast<std::ptrdiff_t>(text.size());
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ptr == begin || parsed.ec != std::errc{})
    {
        return std::nullopt;
    }
    return ParsedFloat{value, offset + static_cast<std::size_t>(parsed.ptr - begin)};
}

[[nodiscard]] std::optional<std::array<float, 8>>
parse_primaries(const std::string_view text) noexcept
{
    std::array<float, 8> values{};
    std::size_t offset = 0U;
    for (float &value : values)
    {
        const auto parsed = parse_float_prefix(text.substr(offset));
        if (!parsed)
        {
            return std::nullopt;
        }
        value = parsed->value;
        offset += parsed->consumed;
    }
    return values;
}

[[nodiscard]] std::vector<std::string_view> split_ascii_words(const std::string_view line)
{
    std::vector<std::string_view> words;
    std::size_t offset = 0U;
    while (offset < line.size())
    {
        while (offset < line.size() && is_ascii_space(line[offset]))
        {
            ++offset;
        }
        const std::size_t begin = offset;
        while (offset < line.size() && !is_ascii_space(line[offset]))
        {
            ++offset;
        }
        if (begin != offset)
        {
            words.push_back(line.substr(begin, offset - begin));
        }
    }
    return words;
}

[[nodiscard]] std::optional<std::uint32_t>
parse_positive_dimension(const std::string_view text) noexcept
{
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U ||
        value > std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool valid_chromaticity(const float x, const float y) noexcept
{
    return std::isfinite(x) && std::isfinite(y) && x >= 0.0F && y > 0.0F && x <= 1.0F &&
           y <= 1.0F && x + y <= 1.0F;
}

[[nodiscard]] bool all_finite(const std::array<float, 9> &matrix) noexcept
{
    return std::all_of(matrix.begin(), matrix.end(),
                       [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] Result<void> derive_legacy_matrices(RadianceRgbeMetadata &metadata,
                                                  const std::string_view source)
{
    const auto &p = metadata.primaries_xy;
    if (!valid_chromaticity(p[0], p[1]) || !valid_chromaticity(p[2], p[3]) ||
        !valid_chromaticity(p[4], p[5]) || !valid_chromaticity(p[6], p[7]))
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE primaries are invalid", source,
                          "invalid_rgbe_primaries");
    }

    const float *const red = &p[0];
    const float *const green = &p[2];
    const float *const blue = &p[4];
    const float *const white = &p[6];
    const float luminance = 1.0F;
    const float white_x = white[0] * luminance / white[1];
    const float white_z = (1.0F - white[0] - white[1]) * luminance / white[1];
    const float primary_determinant = red[0] * (blue[1] - green[1]) +
                                      blue[0] * (green[1] - red[1]) + green[0] * (red[1] - blue[1]);
    if (!std::isfinite(primary_determinant) || std::fabs(primary_determinant) < 1.0e-7F)
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE primaries are degenerate", source,
                          "invalid_rgbe_primaries");
    }

    const float red_scale =
        (white_x * (blue[1] - green[1]) -
         green[0] * (luminance * (blue[1] - 1.0F) + blue[1] * (white_x + white_z)) +
         blue[0] * (luminance * (green[1] - 1.0F) + green[1] * (white_x + white_z))) /
        primary_determinant;
    const float green_scale =
        (white_x * (red[1] - blue[1]) +
         red[0] * (luminance * (blue[1] - 1.0F) + blue[1] * (white_x + white_z)) -
         blue[0] * (luminance * (red[1] - 1.0F) + red[1] * (white_x + white_z))) /
        primary_determinant;
    const float blue_scale =
        (white_x * (green[1] - red[1]) -
         red[0] * (luminance * (green[1] - 1.0F) + green[1] * (white_x + white_z)) +
         green[0] * (luminance * (red[1] - 1.0F) + red[1] * (white_x + white_z))) /
        primary_determinant;

    const std::array<float, 9> row_primaries{
        red_scale * red[0],     red_scale * red[1],     red_scale * (1.0F - red[0] - red[1]),
        green_scale * green[0], green_scale * green[1], green_scale * (1.0F - green[0] - green[1]),
        blue_scale * blue[0],   blue_scale * blue[1],   blue_scale * (1.0F - blue[0] - blue[1]),
    };
    metadata.rgb_to_xyz = {
        row_primaries[0], row_primaries[3], row_primaries[6], row_primaries[1], row_primaries[4],
        row_primaries[7], row_primaries[2], row_primaries[5], row_primaries[8],
    };
    if (!all_finite(metadata.rgb_to_xyz))
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE matrix is not finite", source,
                          "invalid_rgbe_primaries");
    }

    const auto &a = metadata.rgb_to_xyz;
    const float determinant = a[0] * (a[8] * a[4] - a[7] * a[5]) -
                              a[3] * (a[8] * a[1] - a[7] * a[2]) +
                              a[6] * (a[5] * a[1] - a[4] * a[2]);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-7F)
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE matrix is degenerate", source,
                          "invalid_rgbe_primaries");
    }
    const float inverse_determinant = 1.0F / determinant;
    metadata.xyz_to_rgb = {
        inverse_determinant * (a[8] * a[4] - a[7] * a[5]),
        -inverse_determinant * (a[8] * a[1] - a[7] * a[2]),
        inverse_determinant * (a[5] * a[1] - a[4] * a[2]),
        -inverse_determinant * (a[8] * a[3] - a[6] * a[5]),
        inverse_determinant * (a[8] * a[0] - a[6] * a[2]),
        -inverse_determinant * (a[5] * a[0] - a[3] * a[2]),
        inverse_determinant * (a[7] * a[3] - a[6] * a[4]),
        -inverse_determinant * (a[7] * a[0] - a[6] * a[1]),
        inverse_determinant * (a[4] * a[0] - a[3] * a[1]),
    };
    if (!all_finite(metadata.xyz_to_rgb))
    {
        return rgbe_error(ErrorCode::kValidation, "Radiance RGBE inverse matrix is not finite",
                          source, "invalid_rgbe_primaries");
    }
    return {};
}

[[nodiscard]] Result<ParsedHeader> parse_rgbe_header(const ByteSpan bytes,
                                                     const RadianceRgbeDecodeLimits &limits,
                                                     const DecodeContext &decode,
                                                     const std::string_view source)
{
    HeaderCursor cursor{bytes};
    auto magic = read_header_line(cursor, limits, decode, source);
    if (!magic)
    {
        return magic.error();
    }
    ParsedHeader result;
    if (magic.value() == "#?RADIANCE")
    {
        result.metadata.program_type = "RADIANCE";
    }
    else if (magic.value() == "#?RGBE")
    {
        result.metadata.program_type = "RGBE";
    }
    else
    {
        return rgbe_error(ErrorCode::kUnsupported,
                          "Input does not have a frozen Radiance RGBE signature", source,
                          "unsupported_rgbe_signature");
    }

    bool format_seen = false;
    for (;;)
    {
        auto line = read_header_line(cursor, limits, decode, source);
        if (!line)
        {
            return line.error();
        }
        if (line.value().empty())
        {
            break;
        }
        if (line.value() == kRgbeFormat)
        {
            if (format_seen)
            {
                return rgbe_error(ErrorCode::kValidation, "Radiance RGBE FORMAT is duplicated",
                                  source, "duplicate_rgbe_format");
            }
            format_seen = true;
        }
        else if (line.value().starts_with("FORMAT="))
        {
            return rgbe_error(ErrorCode::kUnsupported,
                              "Radiance pixel format is outside the frozen RGBE contract", source,
                              "unsupported_rgbe_format");
        }
        else if (line.value().starts_with("GAMMA="))
        {
            const auto gamma = parse_float_prefix(line.value().substr(6U));
            if (gamma)
            {
                result.metadata.gamma = gamma->value;
                result.metadata.has_gamma = true;
            }
        }
        else if (line.value().starts_with("EXPOSURE="))
        {
            const auto exposure = parse_float_prefix(line.value().substr(9U));
            if (exposure)
            {
                result.metadata.exposure = exposure->value;
                result.metadata.has_exposure = true;
            }
        }
        else if (line.value().starts_with("PRIMARIES="))
        {
            const auto primaries = parse_primaries(line.value().substr(10U));
            if (primaries)
            {
                result.metadata.primaries_xy = *primaries;
                result.metadata.has_custom_primaries = true;
            }
        }
    }
    if (!format_seen)
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE header is missing its exact FORMAT declaration", source,
                          "missing_rgbe_format");
    }

    Result<std::string_view> resolution =
        rgbe_error(ErrorCode::kValidation, "Radiance RGBE resolution is missing", source,
                   "invalid_rgbe_resolution");
    do
    {
        resolution = read_header_line(cursor, limits, decode, source);
        if (!resolution)
        {
            return resolution.error();
        }
    } while (resolution.value().empty());

    if (resolution.value().empty() || is_ascii_space(resolution.value().front()) ||
        is_ascii_space(resolution.value().back()))
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE resolution declaration is malformed", source,
                          "invalid_rgbe_resolution");
    }
    const auto words = split_ascii_words(resolution.value());
    if (words.size() == 4U && (words[0] != "-Y" || words[2] != "+X"))
    {
        return rgbe_error(ErrorCode::kUnsupported,
                          "Radiance orientation is outside the frozen -Y +X contract", source,
                          "unsupported_rgbe_orientation");
    }
    if (words.size() != 4U || words[0] != "-Y" || words[2] != "+X")
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE resolution declaration is malformed", source,
                          "invalid_rgbe_resolution");
    }
    const auto height = parse_positive_dimension(words[1]);
    const auto width = parse_positive_dimension(words[3]);
    if (!width || !height)
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE dimensions must be positive 32-bit values", source,
                          "invalid_rgbe_dimensions");
    }
    result.width = *width;
    result.height = *height;
    result.pixel_offset = cursor.offset;

    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(result.width) * static_cast<std::uint64_t>(result.height);
    if (pixel_count > limits.max_decoded_bytes / kFloatBytesPerPixel)
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE decoded raster exceeds the fixed limit", source,
                          "oversized_rgbe_decoded_raster",
                          {{"decoded_bytes_per_pixel", std::to_string(kFloatBytesPerPixel)},
                           {"height", std::to_string(result.height)},
                           {"max_decoded_bytes", std::to_string(limits.max_decoded_bytes)},
                           {"width", std::to_string(result.width)}});
    }
    auto matrices = derive_legacy_matrices(result.metadata, source);
    if (!matrices)
    {
        return matrices.error();
    }
    return result;
}

[[nodiscard]] float decode_channel(const std::uint8_t mantissa,
                                   const std::uint8_t exponent) noexcept
{
    if (exponent == 0U)
    {
        return 0.0F;
    }
    const float scale = std::ldexp(1.0F, static_cast<int>(exponent) - 136);
    const float value = static_cast<float>(mantissa) * scale;
    return std::fmax(0.0F, std::fmin(10000.0F, value));
}

[[nodiscard]] bool is_old_rle_marker(const std::array<std::uint8_t, 4> &pixel) noexcept
{
    return pixel[0] == 1U && pixel[1] == 1U && pixel[2] == 1U && pixel[3] != 0U;
}

void store_pixel(const std::array<std::uint8_t, 4> &pixel, std::vector<float> &output,
                 const std::size_t pixel_index)
{
    const std::size_t destination = pixel_index * 3U;
    output[destination] = decode_channel(pixel[0], pixel[3]);
    output[destination + 1U] = decode_channel(pixel[1], pixel[3]);
    output[destination + 2U] = decode_channel(pixel[2], pixel[3]);
}

[[nodiscard]] Result<void>
decode_flat_pixels(const ByteSpan bytes, std::size_t &offset, const std::size_t pixel_count,
                   std::vector<float> &output, const std::size_t output_pixel_offset,
                   const DecodeContext &decode, const std::string_view source)
{
    for (std::size_t pixel_index = 0U; pixel_index < pixel_count; ++pixel_index)
    {
        if (bytes.size() - std::min(bytes.size(), offset) < 4U)
        {
            return rgbe_error(ErrorCode::kValidation, "Radiance RGBE pixels are truncated", source,
                              "truncated_rgbe_pixels");
        }
        const std::array<std::uint8_t, 4> pixel{bytes[offset], bytes[offset + 1U],
                                                bytes[offset + 2U], bytes[offset + 3U]};
        offset += 4U;
        if (is_old_rle_marker(pixel))
        {
            return rgbe_error(ErrorCode::kUnsupported,
                              "Radiance old-RLE markers are outside the product contract", source,
                              "unsupported_rgbe_old_rle");
        }
        store_pixel(pixel, output, output_pixel_offset + pixel_index);
        const std::size_t progress = output_pixel_offset + pixel_index + 1U;
        if (progress % kPixelCancellationGranularity == 0U)
        {
            auto active = decode.checkpoint(RadianceRgbeDecodeCheckpoint::kPixels, progress);
            if (!active)
            {
                return active.error();
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> decode_new_rle_pixels(const ByteSpan bytes, std::size_t &offset,
                                                 const ParsedHeader &header,
                                                 std::vector<float> &output,
                                                 const DecodeContext &decode,
                                                 const std::string_view source)
{
    std::vector<std::uint8_t> scanline;
    try
    {
        scanline.resize(static_cast<std::size_t>(header.width) * 4U);
    }
    catch (const std::bad_alloc &)
    {
        return rgbe_error(ErrorCode::kIo, "Unable to allocate Radiance scanline", source,
                          "rgbe_allocation_failed");
    }

    for (std::uint32_t row = 0U; row < header.height; ++row)
    {
        if (bytes.size() - std::min(bytes.size(), offset) < 4U)
        {
            return rgbe_error(ErrorCode::kValidation, "Radiance RGBE pixels are truncated", source,
                              "truncated_rgbe_pixels");
        }
        const std::array<std::uint8_t, 4> prefix{bytes[offset], bytes[offset + 1U],
                                                 bytes[offset + 2U], bytes[offset + 3U]};
        const bool rle_prefix = prefix[0] == 2U && prefix[1] == 2U && (prefix[2] & 0x80U) == 0U;
        if (!rle_prefix)
        {
            const std::size_t remaining =
                static_cast<std::size_t>(header.height - row) * header.width;
            return decode_flat_pixels(bytes, offset, remaining, output,
                                      static_cast<std::size_t>(row) * header.width, decode, source);
        }
        const std::uint32_t encoded_width =
            (static_cast<std::uint32_t>(prefix[2]) << 8U) | prefix[3];
        if (encoded_width != header.width)
        {
            return rgbe_error(ErrorCode::kValidation,
                              "Radiance RLE scanline width does not match the header", source,
                              "rgbe_rle_width_mismatch");
        }
        offset += 4U;

        for (std::size_t channel = 0U; channel < 4U; ++channel)
        {
            const std::size_t channel_begin = channel * header.width;
            std::size_t written = 0U;
            while (written < header.width)
            {
                if (bytes.size() - std::min(bytes.size(), offset) < 2U)
                {
                    return rgbe_error(ErrorCode::kValidation, "Radiance RLE packet is truncated",
                                      source, "truncated_rgbe_rle_packet");
                }
                const std::uint8_t code = bytes[offset++];
                const std::uint8_t first_value = bytes[offset++];
                const std::size_t count = code > 128U ? code - 128U : code;
                if (count == 0U || count > static_cast<std::size_t>(header.width) - written)
                {
                    return rgbe_error(ErrorCode::kValidation,
                                      "Radiance RLE packet exceeds its channel", source,
                                      "invalid_rgbe_rle_packet");
                }
                if (code > 128U)
                {
                    std::fill_n(scanline.begin() +
                                    static_cast<std::ptrdiff_t>(channel_begin + written),
                                static_cast<std::ptrdiff_t>(count), first_value);
                }
                else
                {
                    scanline[channel_begin + written] = first_value;
                    if (count > 1U)
                    {
                        const std::size_t remaining = count - 1U;
                        if (bytes.size() - std::min(bytes.size(), offset) < remaining)
                        {
                            return rgbe_error(ErrorCode::kValidation,
                                              "Radiance RLE literal packet is truncated", source,
                                              "truncated_rgbe_rle_packet");
                        }
                        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                    static_cast<std::ptrdiff_t>(remaining),
                                    scanline.begin() +
                                        static_cast<std::ptrdiff_t>(channel_begin + written + 1U));
                        offset += remaining;
                    }
                }
                written += count;
                auto active =
                    decode.checkpoint(RadianceRgbeDecodeCheckpoint::kPixels,
                                      static_cast<std::size_t>(row) * header.width + written);
                if (!active)
                {
                    return active.error();
                }
            }
        }

        for (std::uint32_t column = 0U; column < header.width; ++column)
        {
            const std::array<std::uint8_t, 4> pixel{
                scanline[column], scanline[header.width + column],
                scanline[static_cast<std::size_t>(header.width) * 2U + column],
                scanline[static_cast<std::size_t>(header.width) * 3U + column]};
            store_pixel(pixel, output, static_cast<std::size_t>(row) * header.width + column);
        }
        auto active = decode.checkpoint(RadianceRgbeDecodeCheckpoint::kPixels,
                                        static_cast<std::size_t>(row + 1U) * header.width);
        if (!active)
        {
            return active.error();
        }
    }
    return {};
}

[[nodiscard]] Result<DecodedHdrRaster> decode_rgbe_bytes(const ByteSpan bytes,
                                                         const RadianceRgbeDecodeLimits &limits,
                                                         const DecodeContext &decode,
                                                         const std::string_view source)
{
    auto active = decode.cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (!rgbe_program_type(bytes))
    {
        return rgbe_error(ErrorCode::kUnsupported,
                          "Input does not have a frozen Radiance RGBE signature", source,
                          "unsupported_rgbe_signature");
    }
    if (bytes.size() > limits.max_encoded_bytes)
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE input exceeds the fixed encoded limit", source,
                          "oversized_rgbe_input",
                          {{"max_encoded_bytes", std::to_string(limits.max_encoded_bytes)}});
    }

    auto header = parse_rgbe_header(bytes, limits, decode, source);
    if (!header)
    {
        return header.error();
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(header.value().width) * header.value().height;
    DecodedHdrRaster result;
    result.width = header.value().width;
    result.height = header.value().height;
    result.radiance = header.value().metadata;
    result.pixel_format = HdrPixelFormat::kLinearRgbF32;
    result.alpha_mode = HdrAlphaMode::kOpaque;
    try
    {
        result.rgb.resize(pixel_count * 3U);
    }
    catch (const std::bad_alloc &)
    {
        return rgbe_error(ErrorCode::kIo, "Unable to allocate Radiance RGBE pixels", source,
                          "rgbe_allocation_failed");
    }

    std::size_t offset = header.value().pixel_offset;
    Result<void> decoded;
    if (header.value().width >= 8U && header.value().width <= 0x7FFFU)
    {
        decoded = decode_new_rle_pixels(bytes, offset, header.value(), result.rgb, decode, source);
    }
    else
    {
        decoded = decode_flat_pixels(bytes, offset, pixel_count, result.rgb, 0U, decode, source);
    }
    if (!decoded)
    {
        return decoded.error();
    }
    active = decode.cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return result;
}

} // namespace

RadianceRgbeDecoder::RadianceRgbeDecoder(RadianceRgbeDecodeLimits limits,
                                         RadianceRgbeCheckpointObserver checkpoint_observer)
    : limits_(sanitized_limits(limits))
    , checkpoint_observer_(checkpoint_observer)
{
}

Result<DecodedHdrRaster> RadianceRgbeDecoder::decode(const std::string_view path,
                                                     const CancellationToken &cancellation) const
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (path.empty())
    {
        return rgbe_error(ErrorCode::kInvalidArgument, "Radiance RGBE path must not be empty", path,
                          "invalid_rgbe_path");
    }
    const QString file_name = qstring_from_utf8(path);
    if (file_name.isNull())
    {
        return rgbe_error(ErrorCode::kInvalidArgument, "Radiance RGBE path is too long", path,
                          "invalid_rgbe_path");
    }
    const QFileInfo info(file_name);
    if (!info.exists())
    {
        return rgbe_error(ErrorCode::kNotFound, "Radiance RGBE input does not exist", path,
                          "rgbe_input_not_found");
    }
    if (!info.isFile())
    {
        return rgbe_error(ErrorCode::kIo, "Radiance RGBE input is not a regular file", path,
                          "rgbe_input_not_regular");
    }
    const qint64 original_size = info.size();
    if (original_size < 0)
    {
        return rgbe_error(ErrorCode::kIo, "Unable to determine Radiance RGBE input size", path,
                          "rgbe_input_read_failed");
    }
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly))
    {
        return rgbe_error(ErrorCode::kIo, "Unable to open Radiance RGBE input", path,
                          "rgbe_input_open_failed",
                          {{"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    const DecodeContext decode_context{cancellation, checkpoint_observer_};
    active = decode_context.checkpoint(RadianceRgbeDecodeCheckpoint::kBeforeFileRead, 0U);
    if (!active)
    {
        return active.error();
    }
    const QByteArray prefix = file.peek(static_cast<qint64>(kRadianceMagic.size()));
    if (file.error() != QFileDevice::NoError || (original_size > 0 && prefix.isEmpty()))
    {
        return rgbe_error(ErrorCode::kIo, "Unable to inspect Radiance RGBE input", path,
                          "rgbe_input_read_failed",
                          {{"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    if (!rgbe_program_type(ByteSpan(reinterpret_cast<const std::uint8_t *>(prefix.constData()),
                                    static_cast<std::size_t>(prefix.size()))))
    {
        return rgbe_error(ErrorCode::kUnsupported,
                          "Input does not have a frozen Radiance RGBE signature", path,
                          "unsupported_rgbe_signature");
    }
    if (static_cast<std::uint64_t>(original_size) > limits_.max_encoded_bytes ||
        original_size > std::numeric_limits<qsizetype>::max())
    {
        return rgbe_error(ErrorCode::kValidation,
                          "Radiance RGBE input exceeds the fixed encoded limit", path,
                          "oversized_rgbe_input",
                          {{"max_encoded_bytes", std::to_string(limits_.max_encoded_bytes)}});
    }

    QByteArray bytes;
    try
    {
        bytes.resize(static_cast<qsizetype>(original_size));
    }
    catch (const std::bad_alloc &)
    {
        return rgbe_error(ErrorCode::kIo, "Unable to allocate Radiance RGBE input buffer", path,
                          "rgbe_allocation_failed");
    }
    if (!file.seek(0))
    {
        return rgbe_error(ErrorCode::kIo, "Unable to seek Radiance RGBE input", path,
                          "rgbe_input_read_failed");
    }
    qint64 total = 0;
    while (total < original_size)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const qint64 requested =
            std::min<qint64>(static_cast<qint64>(kFileReadChunkBytes), original_size - total);
        const qint64 count = file.read(bytes.data() + total, requested);
        if (count <= 0)
        {
            return rgbe_error(ErrorCode::kIo, "Unable to read complete Radiance RGBE input", path,
                              "rgbe_input_read_failed",
                              {{"qt_error", file.errorString().toUtf8().toStdString()}});
        }
        total += count;
    }
    return decode_rgbe_bytes(ByteSpan(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                      static_cast<std::size_t>(bytes.size())),
                             limits_, decode_context, path);
}

Result<DecodedHdrRaster>
RadianceRgbeDecoder::decode_memory(const std::vector<std::uint8_t> &encoded,
                                   const CancellationToken &cancellation) const
{
    const DecodeContext decode_context{cancellation, checkpoint_observer_};
    return decode_rgbe_bytes(encoded, limits_, decode_context, "memory");
}

} // namespace ravo
