#include "ravo/domain/types.h"

#include "ravo/domain/uri.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <set>
#include <sstream>
#include <tuple>

namespace ravo
{
namespace
{

[[nodiscard]] std::string random_hex_id(const std::string_view prefix)
{
    std::random_device device;
    std::uniform_int_distribution<int> distribution(0, 255);
    std::string id{prefix};
    static constexpr char hex[] = "0123456789abcdef";
    id.reserve(prefix.size() + 32U);
    for (int index = 0; index < 16; ++index)
    {
        const auto value = static_cast<unsigned int>(distribution(device)) & 0xffU;
        id.push_back(hex[value >> 4U]);
        id.push_back(hex[value & 0x0fU]);
    }
    return id;
}

[[nodiscard]] bool is_xml_1_0_character(const std::uint32_t scalar) noexcept
{
    return scalar == 0x09U || scalar == 0x0AU || scalar == 0x0DU ||
           (scalar >= 0x20U && scalar <= 0xD7FFU) || (scalar >= 0xE000U && scalar <= 0xFFFDU) ||
           (scalar >= 0x10000U && scalar <= 0x10FFFFU);
}

[[nodiscard]] bool is_valid_utf8(const std::string_view text,
                                 const bool require_xml_1_0 = false) noexcept
{
    std::size_t offset = 0U;
    while (offset < text.size())
    {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        if (first <= 0x7FU)
        {
            if (require_xml_1_0 && !is_xml_1_0_character(first))
            {
                return false;
            }
            ++offset;
            continue;
        }

        std::size_t length = 0U;
        std::uint32_t scalar = 0U;
        std::uint32_t minimum = 0U;
        if ((first & 0xE0U) == 0xC0U)
        {
            length = 2U;
            scalar = first & 0x1FU;
            minimum = 0x80U;
        }
        else if ((first & 0xF0U) == 0xE0U)
        {
            length = 3U;
            scalar = first & 0x0FU;
            minimum = 0x800U;
        }
        else if ((first & 0xF8U) == 0xF0U)
        {
            length = 4U;
            scalar = first & 0x07U;
            minimum = 0x10000U;
        }
        else
        {
            return false;
        }
        if (length > text.size() - offset)
        {
            return false;
        }
        for (std::size_t index = 1U; index < length; ++index)
        {
            const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
            if ((continuation & 0xC0U) != 0x80U)
            {
                return false;
            }
            scalar = (scalar << 6U) | (continuation & 0x3FU);
        }
        if (scalar < minimum || scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU) ||
            (require_xml_1_0 && !is_xml_1_0_character(scalar)))
        {
            return false;
        }
        offset += length;
    }
    return true;
}

[[nodiscard]] bool has_iptc_control_character(const std::string_view text,
                                              const bool allow_line_breaks) noexcept
{
    std::size_t offset = 0U;
    while (offset < text.size())
    {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        if (first <= 0x7FU)
        {
            if (first < 0x20U && !(allow_line_breaks && (first == '\r' || first == '\n')))
            {
                return true;
            }
            if (first == 0x7FU)
            {
                return true;
            }
            ++offset;
            continue;
        }
        if (first == 0xC2U && offset + 1U < text.size())
        {
            const auto second = static_cast<std::uint8_t>(text[offset + 1U]);
            if (second >= 0x80U && second <= 0x9FU)
            {
                return true;
            }
        }
        if ((first & 0xE0U) == 0xC0U)
        {
            offset += 2U;
        }
        else if ((first & 0xF0U) == 0xE0U)
        {
            offset += 3U;
        }
        else
        {
            offset += 4U;
        }
    }
    return false;
}

} // namespace

std::string generate_catalog_id()
{
    return random_hex_id("cat_");
}

std::string generate_asset_id()
{
    return random_hex_id("ast_");
}

std::string make_content_fingerprint(const FileIdentity &identity)
{
    return std::to_string(identity.size_bytes) + "-" + std::to_string(identity.mtime_unix_ms);
}

std::string make_preview_cache_key(const std::string_view asset_id, const std::uint32_t width,
                                   const std::uint32_t height, const std::string_view fingerprint,
                                   const std::string_view edit_digest)
{
    std::ostringstream key;
    key << "v" << kPreviewContractVersion << "_" << asset_id << "_" << width << "x" << height << "_"
        << fingerprint << "_" << (edit_digest.empty() ? "identity" : edit_digest);
    return key.str();
}

void fit_within_max_edge(const std::uint32_t source_width, const std::uint32_t source_height,
                         const std::uint32_t max_edge, std::uint32_t &output_width,
                         std::uint32_t &output_height) noexcept
{
    if (source_width == 0 || source_height == 0)
    {
        output_width = 0;
        output_height = 0;
        return;
    }
    if (max_edge == 0 || (source_width <= max_edge && source_height <= max_edge))
    {
        output_width = source_width;
        output_height = source_height;
        return;
    }
    if (source_width >= source_height)
    {
        output_width = max_edge;
        output_height = std::max<std::uint32_t>(
            1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(source_height) * max_edge /
                                           source_width));
    }
    else
    {
        output_height = max_edge;
        output_width = std::max<std::uint32_t>(
            1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(source_width) * max_edge /
                                           source_height));
    }
}

Result<void> validate_rating(const int rating)
{
    if (rating < 0 || rating > 5)
    {
        return make_error(ErrorCode::kValidation, "Rating must be between 0 and 5",
                          {{"rating", std::to_string(rating)}});
    }
    return {};
}

std::string_view color_label_name(const ColorLabel label) noexcept
{
    switch (label)
    {
    case ColorLabel::kNone:
        return "none";
    case ColorLabel::kRed:
        return "red";
    case ColorLabel::kYellow:
        return "yellow";
    case ColorLabel::kGreen:
        return "green";
    case ColorLabel::kBlue:
        return "blue";
    case ColorLabel::kPurple:
        return "purple";
    }
    return "none";
}

Result<ColorLabel> parse_color_label(const std::string_view name)
{
    if (name == "none" || name.empty())
    {
        return ColorLabel::kNone;
    }
    if (name == "red")
    {
        return ColorLabel::kRed;
    }
    if (name == "yellow")
    {
        return ColorLabel::kYellow;
    }
    if (name == "green")
    {
        return ColorLabel::kGreen;
    }
    if (name == "blue")
    {
        return ColorLabel::kBlue;
    }
    if (name == "purple")
    {
        return ColorLabel::kPurple;
    }
    return make_error(ErrorCode::kValidation, "Unknown color label",
                      {{"color_label", std::string(name)}});
}

std::string_view export_format_name(const ExportFormat format) noexcept
{
    switch (format)
    {
    case ExportFormat::kPng:
        return "png";
    case ExportFormat::kJpeg:
        return "jpeg";
    case ExportFormat::kTiff:
        return "tiff";
    case ExportFormat::kOriginalCopy:
        return "original";
    }
    return "png";
}

std::string_view export_format_extension(const ExportFormat format) noexcept
{
    switch (format)
    {
    case ExportFormat::kPng:
        return ".png";
    case ExportFormat::kJpeg:
        return ".jpg";
    case ExportFormat::kTiff:
        return ".tif";
    case ExportFormat::kOriginalCopy:
        return {};
    }
    return ".png";
}

Result<ExportFormat> parse_export_format(const std::string_view name)
{
    if (name == "png")
    {
        return ExportFormat::kPng;
    }
    if (name == "jpeg" || name == "jpg")
    {
        return ExportFormat::kJpeg;
    }
    if (name == "tiff" || name == "tif")
    {
        return ExportFormat::kTiff;
    }
    if (name == "original" || name == "copy" || name == "original-copy")
    {
        return ExportFormat::kOriginalCopy;
    }
    return make_error(ErrorCode::kValidation, "Unknown export format",
                      {{"format", std::string(name)}});
}

std::string_view export_metadata_mode_name(const ExportMetadataMode mode) noexcept
{
    switch (mode)
    {
    case ExportMetadataMode::kFull:
        return "full";
    case ExportMetadataMode::kNoLocation:
        return "no-location";
    case ExportMetadataMode::kNone:
        return "none";
    }
    return "full";
}

Result<ExportMetadataMode> parse_export_metadata_mode(const std::string_view name)
{
    if (name == "full")
        return ExportMetadataMode::kFull;
    if (name == "no-location")
        return ExportMetadataMode::kNoLocation;
    if (name == "none")
        return ExportMetadataMode::kNone;
    return make_error(
        ErrorCode::kValidation, "Unknown export metadata mode",
        {{"metadata_mode", std::string(name)}, {"reason", "invalid_export_metadata_mode"}});
}

Result<std::string> expand_export_filename_template(const std::string_view filename_template,
                                                    const std::string_view source_stem,
                                                    const std::string_view asset_id,
                                                    const std::size_t sequence,
                                                    const std::string_view extension)
{
    if (filename_template.empty() || filename_template.size() > kExportFilenameTemplateMaxBytes ||
        !is_valid_utf8(filename_template))
    {
        return make_error(ErrorCode::kValidation,
                          "Export filename template must be bounded UTF-8 text",
                          {{"reason", "invalid_export_filename_template"},
                           {"max_bytes", std::to_string(kExportFilenameTemplateMaxBytes)}});
    }
    if (source_stem.empty() || source_stem.size() > kExportFilenameMaxBytes || asset_id.empty() ||
        asset_id.size() > kExportFilenameMaxBytes || !is_valid_utf8(source_stem) ||
        !is_valid_utf8(asset_id))
    {
        return make_error(ErrorCode::kValidation, "Export filename inputs are invalid",
                          {{"reason", "invalid_export_filename_input"}});
    }
    if (sequence == 0 || sequence > kExportBatchMaxAssets)
    {
        return make_error(
            ErrorCode::kValidation, "Export sequence is out of range",
            {{"reason", "invalid_export_sequence"}, {"sequence", std::to_string(sequence)}});
    }
    if (extension.size() > kExportFilenameMaxBytes ||
        (!extension.empty() &&
         (extension.front() != '.' ||
          extension.find_first_of("/\\{}") != std::string_view::npos ||
          !is_valid_utf8(extension))))
    {
        return make_error(
            ErrorCode::kValidation, "Export extension is invalid",
            {{"reason", "invalid_export_extension"}, {"extension", std::string(extension)}});
    }

    std::string sequence_text = std::to_string(sequence);
    if (sequence_text.size() < 4U)
        sequence_text.insert(sequence_text.begin(), 4U - sequence_text.size(), '0');
    std::string filename;
    filename.reserve(std::min<std::size_t>(kExportFilenameMaxBytes,
                                           filename_template.size() + source_stem.size() +
                                               asset_id.size() + extension.size() + 8U));
    bool has_extension_token = false;
    for (std::size_t offset = 0; offset < filename_template.size();)
    {
        if (filename_template[offset] == '}')
        {
            return make_error(ErrorCode::kValidation, "Export filename template has stray brace",
                              {{"reason", "invalid_export_filename_template"},
                               {"offset", std::to_string(offset)}});
        }
        if (filename_template[offset] != '{')
        {
            filename.push_back(filename_template[offset++]);
            if (filename.size() > kExportFilenameMaxBytes)
            {
                return make_error(ErrorCode::kValidation,
                                  "Expanded export filename is too large",
                                  {{"reason", "invalid_expanded_export_filename"},
                                   {"max_bytes", std::to_string(kExportFilenameMaxBytes)}});
            }
            continue;
        }
        const auto close = filename_template.find('}', offset + 1U);
        if (close == std::string_view::npos)
        {
            return make_error(ErrorCode::kValidation,
                              "Export filename template has an unterminated token",
                              {{"reason", "invalid_export_filename_template"},
                               {"offset", std::to_string(offset)}});
        }
        const auto token = filename_template.substr(offset + 1U, close - offset - 1U);
        if (token == "stem")
            filename.append(source_stem);
        else if (token == "asset_id")
            filename.append(asset_id);
        else if (token == "sequence")
            filename.append(sequence_text);
        else if (token == "ext")
        {
            filename.append(extension);
            has_extension_token = true;
        }
        else
        {
            return make_error(
                ErrorCode::kValidation, "Unknown export filename template token",
                {{"reason", "unknown_export_filename_token"}, {"token", std::string(token)}});
        }
        if (filename.size() > kExportFilenameMaxBytes)
        {
            return make_error(ErrorCode::kValidation, "Expanded export filename is too large",
                              {{"reason", "invalid_expanded_export_filename"},
                               {"max_bytes", std::to_string(kExportFilenameMaxBytes)}});
        }
        offset = close + 1U;
    }
    if (!has_extension_token)
    {
        filename.append(extension);
        if (filename.size() > kExportFilenameMaxBytes)
        {
            return make_error(ErrorCode::kValidation, "Expanded export filename is too large",
                              {{"reason", "invalid_expanded_export_filename"},
                               {"max_bytes", std::to_string(kExportFilenameMaxBytes)}});
        }
    }

    if (filename.empty() || filename.size() > kExportFilenameMaxBytes || !is_valid_utf8(filename) ||
        filename == "." || filename == ".." || filename.back() == '.' || filename.back() == ' ')
    {
        return make_error(ErrorCode::kValidation, "Expanded export filename is invalid",
                          {{"reason", "invalid_expanded_export_filename"},
                           {"filename", filename},
                           {"max_bytes", std::to_string(kExportFilenameMaxBytes)}});
    }
    for (const char raw_character : filename)
    {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character < 0x20U || character == 0x7fU || character == '/' || character == '\\' ||
            character == ':' || character == '*' || character == '?' || character == '"' ||
            character == '<' || character == '>' || character == '|')
        {
            return make_error(ErrorCode::kValidation, "Expanded export filename is not portable",
                              {{"reason", "nonportable_export_filename"}, {"filename", filename}});
        }
    }
    std::string device_name = filename.substr(0, filename.find('.'));
    std::transform(device_name.begin(), device_name.end(), device_name.begin(),
                   [](const unsigned char character)
                   {
                       return character >= 'a' && character <= 'z' ?
                                  static_cast<char>(character - 'a' + 'A') :
                                  static_cast<char>(character);
                   });
    static const std::set<std::string, std::less<>> reserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (reserved.contains(device_name))
    {
        return make_error(ErrorCode::kValidation, "Expanded export filename is reserved",
                          {{"reason", "reserved_export_filename"}, {"filename", filename}});
    }
    return filename;
}

std::string_view jpeg_subsampling_name(const JpegSubsampling subsampling) noexcept
{
    switch (subsampling)
    {
    case JpegSubsampling::kAuto:
        return "auto";
    case JpegSubsampling::k444:
        return "444";
    case JpegSubsampling::k440:
        return "440";
    case JpegSubsampling::k422:
        return "422";
    case JpegSubsampling::k420:
        return "420";
    }
    return "unknown";
}

Result<JpegSubsampling> parse_jpeg_subsampling(const std::string_view name)
{
    if (name == "auto")
    {
        return JpegSubsampling::kAuto;
    }
    if (name == "444")
    {
        return JpegSubsampling::k444;
    }
    if (name == "440")
    {
        return JpegSubsampling::k440;
    }
    if (name == "422")
    {
        return JpegSubsampling::k422;
    }
    if (name == "420")
    {
        return JpegSubsampling::k420;
    }
    return make_error(ErrorCode::kValidation, "Unknown JPEG subsampling mode",
                      {{"format", "jpeg"},
                       {"reason", "invalid_jpeg_subsampling"},
                       {"subsampling", std::string(name)}});
}

Result<void> validate_jpeg_export_options(const JpegExportOptions &options)
{
    if (options.quality < kJpegQualityMin || options.quality > kJpegQualityMax)
    {
        return make_error(ErrorCode::kValidation, "JPEG quality must be between 5 and 100",
                          {{"format", "jpeg"},
                           {"maximum", std::to_string(kJpegQualityMax)},
                           {"minimum", std::to_string(kJpegQualityMin)},
                           {"quality", std::to_string(options.quality)},
                           {"reason", "invalid_jpeg_quality"}});
    }
    switch (options.subsampling)
    {
    case JpegSubsampling::kAuto:
    case JpegSubsampling::k444:
    case JpegSubsampling::k440:
    case JpegSubsampling::k422:
    case JpegSubsampling::k420:
        return {};
    }
    return make_error(
        ErrorCode::kValidation, "JPEG subsampling mode is invalid",
        {{"format", "jpeg"},
         {"reason", "invalid_jpeg_subsampling"},
         {"subsampling", std::to_string(static_cast<std::uint8_t>(options.subsampling))}});
}

std::string_view png_bit_depth_name(const PngBitDepth bit_depth) noexcept
{
    switch (bit_depth)
    {
    case PngBitDepth::k8:
        return "8";
    case PngBitDepth::k16:
        return "16";
    }
    return "unknown";
}

Result<PngBitDepth> parse_png_bit_depth(const std::string_view name)
{
    if (name == "8")
    {
        return PngBitDepth::k8;
    }
    if (name == "16")
    {
        return PngBitDepth::k16;
    }
    return make_error(
        ErrorCode::kValidation, "Unknown PNG bit depth",
        {{"bit_depth", std::string(name)}, {"format", "png"}, {"reason", "invalid_png_bit_depth"}});
}

Result<void> validate_png_export_options(const PngExportOptions &options)
{
    switch (options.bit_depth)
    {
    case PngBitDepth::k8:
    case PngBitDepth::k16:
        break;
    default:
        return make_error(
            ErrorCode::kValidation, "PNG bit depth is invalid",
            {{"bit_depth", std::to_string(static_cast<std::uint8_t>(options.bit_depth))},
             {"format", "png"},
             {"reason", "invalid_png_bit_depth"}});
    }
    if (options.compression < kPngCompressionMin || options.compression > kPngCompressionMax)
    {
        return make_error(ErrorCode::kValidation, "PNG compression must be between 0 and 9",
                          {{"compression", std::to_string(options.compression)},
                           {"format", "png"},
                           {"maximum", std::to_string(kPngCompressionMax)},
                           {"minimum", std::to_string(kPngCompressionMin)},
                           {"reason", "invalid_png_compression"}});
    }
    return {};
}

std::string_view tiff_sample_type_name(const TiffSampleType sample_type) noexcept
{
    switch (sample_type)
    {
    case TiffSampleType::kUint8:
        return "uint8";
    case TiffSampleType::kUint16:
        return "uint16";
    case TiffSampleType::kFloat16:
        return "float16";
    case TiffSampleType::kFloat32:
        return "float32";
    }
    return "unknown";
}

Result<TiffSampleType> parse_tiff_sample_type(const std::string_view name)
{
    if (name == "uint8")
    {
        return TiffSampleType::kUint8;
    }
    if (name == "uint16")
    {
        return TiffSampleType::kUint16;
    }
    if (name == "float16")
    {
        return TiffSampleType::kFloat16;
    }
    if (name == "float32")
    {
        return TiffSampleType::kFloat32;
    }
    return make_error(ErrorCode::kValidation, "Unknown TIFF sample type",
                      {{"format", "tiff"},
                       {"reason", "invalid_tiff_sample_type"},
                       {"sample_type", std::string(name)}});
}

std::string_view tiff_compression_name(const TiffCompression compression) noexcept
{
    switch (compression)
    {
    case TiffCompression::kNone:
        return "none";
    case TiffCompression::kDeflate:
        return "deflate";
    case TiffCompression::kDeflatePredictor:
        return "deflate_predictor";
    }
    return "unknown";
}

Result<TiffCompression> parse_tiff_compression(const std::string_view name)
{
    if (name == "none")
    {
        return TiffCompression::kNone;
    }
    if (name == "deflate")
    {
        return TiffCompression::kDeflate;
    }
    if (name == "deflate_predictor")
    {
        return TiffCompression::kDeflatePredictor;
    }
    return make_error(ErrorCode::kValidation, "Unknown TIFF compression mode",
                      {{"compression", std::string(name)},
                       {"format", "tiff"},
                       {"reason", "invalid_tiff_compression"}});
}

Result<void> validate_tiff_export_options(const TiffExportOptions &options)
{
    switch (options.sample_type)
    {
    case TiffSampleType::kUint8:
    case TiffSampleType::kUint16:
    case TiffSampleType::kFloat16:
    case TiffSampleType::kFloat32:
        break;
    default:
        return make_error(
            ErrorCode::kValidation, "TIFF sample type is invalid",
            {{"format", "tiff"},
             {"reason", "invalid_tiff_sample_type"},
             {"sample_type", std::to_string(static_cast<std::uint8_t>(options.sample_type))}});
    }
    switch (options.compression)
    {
    case TiffCompression::kNone:
    case TiffCompression::kDeflate:
    case TiffCompression::kDeflatePredictor:
        break;
    default:
        return make_error(
            ErrorCode::kValidation, "TIFF compression mode is invalid",
            {{"compression", std::to_string(static_cast<std::uint8_t>(options.compression))},
             {"format", "tiff"},
             {"reason", "invalid_tiff_compression"}});
    }
    if (options.compression_level < kTiffCompressionLevelMin ||
        options.compression_level > kTiffCompressionLevelMax)
    {
        return make_error(ErrorCode::kValidation, "TIFF compression level must be between 1 and 9",
                          {{"compression_level", std::to_string(options.compression_level)},
                           {"format", "tiff"},
                           {"maximum", std::to_string(kTiffCompressionLevelMax)},
                           {"minimum", std::to_string(kTiffCompressionLevelMin)},
                           {"reason", "invalid_tiff_compression_level"}});
    }
    if (options.resolution_dpi < kTiffResolutionDpiMin ||
        options.resolution_dpi > kTiffResolutionDpiMax)
    {
        return make_error(ErrorCode::kValidation, "TIFF resolution must be between 72 and 9600 DPI",
                          {{"format", "tiff"},
                           {"maximum", std::to_string(kTiffResolutionDpiMax)},
                           {"minimum", std::to_string(kTiffResolutionDpiMin)},
                           {"reason", "invalid_tiff_resolution"},
                           {"resolution_dpi", std::to_string(options.resolution_dpi)}});
    }
    return {};
}

Result<void> validate_tiff_export_document_name(const std::string_view name)
{
    if (name.size() > kExportDocumentNameMaxBytes || name.find('\0') != std::string_view::npos ||
        !is_valid_utf8(name))
    {
        return make_error(ErrorCode::kValidation, "TIFF document name is not a bounded UTF-8 path",
                          {{"field", "document_name"},
                           {"format", "tiff"},
                           {"maximum_bytes", std::to_string(kExportDocumentNameMaxBytes)},
                           {"reason", "invalid_tiff_document_name"},
                           {"size_bytes", std::to_string(name.size())}});
    }
    return {};
}

namespace
{

[[nodiscard]] Result<void> validate_optional_utf8_field(const std::string_view name,
                                                        const std::optional<std::string> &value,
                                                        const std::string_view reason,
                                                        const std::size_t maximum)
{
    if (!value)
    {
        return {};
    }
    const bool valid_utf8 = is_valid_utf8(*value);
    const bool valid_xml = valid_utf8 && is_valid_utf8(*value, true);
    if (value->size() > maximum || value->find('\0') != std::string::npos || !valid_xml)
    {
        std::string detail = valid_utf8 ? "invalid_xml_character" : "invalid_utf8";
        if (value->size() > maximum)
        {
            detail = "field_too_long";
        }
        else if (value->find('\0') != std::string::npos)
        {
            detail = "contains_nul";
        }
        return make_error(ErrorCode::kValidation,
                          "Export metadata field is not bounded XML-compatible UTF-8 text",
                          {{"detail", std::move(detail)},
                           {"field", std::string(name)},
                           {"maximum_bytes", std::to_string(maximum)},
                           {"reason", std::string(reason)},
                           {"size_bytes", std::to_string(value->size())}});
    }
    return {};
}

[[nodiscard]] Result<void> validate_iptc_text_value(const std::string_view name,
                                                    const std::string_view value,
                                                    const std::size_t maximum,
                                                    const bool allow_line_breaks)
{
    if (value.size() > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "Export metadata exceeds the IPTC-IIM dataset bound",
                          {{"field", std::string(name)},
                           {"maximum_bytes", std::to_string(maximum)},
                           {"reason", "export_iptc_dataset_too_large"},
                           {"size_bytes", std::to_string(value.size())}});
    }
    if (has_iptc_control_character(value, allow_line_breaks))
    {
        return make_error(ErrorCode::kValidation,
                          "Export metadata contains text unsupported by IPTC-IIM",
                          {{"field", std::string(name)}, {"reason", "invalid_export_iptc_text"}});
    }
    return {};
}

[[nodiscard]] Result<void> validate_iptc_text_field(const std::string_view name,
                                                    const std::optional<std::string> &value,
                                                    const std::size_t maximum,
                                                    const bool allow_line_breaks = false)
{
    return value ? validate_iptc_text_value(name, *value, maximum, allow_line_breaks) :
                   Result<void>{};
}

[[nodiscard]] Result<void> validate_capture_number(const std::string_view name,
                                                   const std::optional<double> &value)
{
    if (!value)
    {
        return {};
    }
    if (name == "iso")
    {
        auto iso = export_photographic_sensitivity(*value);
        if (!iso)
        {
            return iso.error();
        }
        return {};
    }
    auto rational = export_positive_rational(*value);
    if (!rational)
    {
        auto error = rational.error();
        error.context.insert_or_assign("field", std::string(name));
        return error;
    }
    return {};
}

} // namespace

std::size_t xml_escaped_utf8_size(const std::string_view text) noexcept
{
    std::size_t size = 0U;
    for (const char byte : text)
    {
        std::size_t addition = 1U;
        switch (byte)
        {
        case '&':
            addition = 5U;
            break;
        case '<':
        case '>':
            addition = 4U;
            break;
        case '"':
        case '\'':
            addition = 6U;
            break;
        case '\r':
            addition = 5U; // &#xD; preserves CR through XML end-of-line normalization.
            break;
        default:
            break;
        }
        if (size > std::numeric_limits<std::size_t>::max() - addition)
        {
            return std::numeric_limits<std::size_t>::max();
        }
        size += addition;
    }
    return size;
}

bool export_color_space_is_srgb(const ColorProfileState &profile) noexcept
{
    return profile.kind == ColorProfileKind::kBuiltin && profile.identifier == "srgb";
}

bool export_iptc_should_omit(const ExportMetadataSnapshot &metadata) noexcept
{
    return !metadata.writable.title && !metadata.writable.description &&
           !metadata.writable.creator && !metadata.writable.copyright && metadata.tags.empty();
}

std::string export_rational_xmp_text(const ExportUnsignedRational value)
{
    return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

Result<std::uint16_t> export_photographic_sensitivity(const double iso)
{
    if (!std::isfinite(iso) || iso <= 0.0)
    {
        return make_error(ErrorCode::kValidation, "Export ISO must be a positive finite integer",
                          {{"field", "iso"}, {"reason", "invalid_export_capture_number"}});
    }
    if (std::trunc(iso) != iso)
    {
        return make_error(ErrorCode::kValidation, "Export ISO must be exactly an integer",
                          {{"field", "iso"}, {"reason", "invalid_export_iso_fractional"}});
    }
    if (iso > static_cast<double>(std::numeric_limits<std::uint16_t>::max()))
    {
        return make_error(ErrorCode::kValidation, "Export ISO exceeds the Exif SHORT range",
                          {{"field", "iso"},
                           {"maximum", std::to_string(std::numeric_limits<std::uint16_t>::max())},
                           {"reason", "invalid_export_iso_range"}});
    }
    return static_cast<std::uint16_t>(iso);
}

Result<ExportUnsignedRational> export_positive_rational(const double value)
{
    if (!std::isfinite(value) || value <= 0.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Export capture number must be a positive finite value",
                          {{"reason", "invalid_export_capture_number"}});
    }
    if (value > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        return make_error(ErrorCode::kValidation, "Export rational exceeds the 32-bit range",
                          {{"reason", "export_rational_overflow"}});
    }

    const auto fits32 = [](const std::uint64_t numerator, const std::uint64_t denominator) noexcept
    {
        return denominator > 0U && numerator <= std::numeric_limits<std::uint32_t>::max() &&
               denominator <= std::numeric_limits<std::uint32_t>::max();
    };

    // Standard continued-fraction convergents. Intermediate 0/1 is required so
    // values in (0, 1) such as 1/125 can be represented exactly.
    std::uint64_t older_numerator = 0U;
    std::uint64_t older_denominator = 1U;
    std::uint64_t numerator = 1U;
    std::uint64_t denominator = 0U;
    double remaining = value;
    bool have_convergent = false;
    for (int step = 0; step < 64; ++step)
    {
        if (!std::isfinite(remaining) || remaining < 0.0)
        {
            break;
        }
        const auto integer = static_cast<std::uint64_t>(std::floor(remaining));
        if (integer > 0U &&
            (numerator > (std::numeric_limits<std::uint64_t>::max() - older_numerator) / integer ||
             denominator >
                 (std::numeric_limits<std::uint64_t>::max() - older_denominator) / integer))
        {
            break;
        }
        const std::uint64_t next_numerator = integer * numerator + older_numerator;
        const std::uint64_t next_denominator = integer * denominator + older_denominator;
        if (!fits32(next_numerator, next_denominator))
        {
            break;
        }
        older_numerator = numerator;
        older_denominator = denominator;
        numerator = next_numerator;
        denominator = next_denominator;
        have_convergent = numerator > 0U && denominator > 0U;
        const double fraction = remaining - static_cast<double>(integer);
        if (fraction <= 0.0 || fraction < 1.0e-12)
        {
            break;
        }
        remaining = 1.0 / fraction;
    }
    if (!have_convergent)
    {
        return make_error(ErrorCode::kValidation, "Export rational exceeds the 32-bit range",
                          {{"reason", "export_rational_overflow"}});
    }
    return ExportUnsignedRational{static_cast<std::uint32_t>(numerator),
                                  static_cast<std::uint32_t>(denominator)};
}

Result<std::vector<std::string>> canonicalize_export_tags(const std::vector<std::string> &tags,
                                                          const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (tags.size() > kExportTagMaxCount)
    {
        return make_error(ErrorCode::kValidation, "Export tag count exceeds the packet bound",
                          {{"maximum", std::to_string(kExportTagMaxCount)},
                           {"reason", "export_tag_count_too_large"},
                           {"size", std::to_string(tags.size())}});
    }
    try
    {
        std::vector<std::string> canonical;
        canonical.reserve(tags.size());
        for (const auto &tag : tags)
        {
            active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
            auto normalized = normalize_tag_name(tag);
            if (!normalized)
            {
                return normalized.error();
            }
            if (!is_valid_utf8(normalized.value()))
            {
                return make_error(ErrorCode::kValidation, "Export tag is not valid UTF-8",
                                  {{"reason", "invalid_export_tag_utf8"}});
            }
            if (!is_valid_utf8(normalized.value(), true))
            {
                return make_error(ErrorCode::kValidation,
                                  "Export tag contains a character unsupported by XML 1.0",
                                  {{"reason", "invalid_export_tag_xml_character"}});
            }
            canonical.push_back(std::move(normalized).value());
        }
        std::sort(canonical.begin(), canonical.end());
        for (std::size_t index = 1U; index < canonical.size(); ++index)
        {
            if (canonical[index] == canonical[index - 1U])
            {
                return make_error(ErrorCode::kValidation, "Export tags must be unique",
                                  {{"reason", "duplicate_export_tag"}, {"tag", canonical[index]}});
            }
        }
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        return canonical;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate canonical export tags",
                          {{"reason", "export_metadata_allocation_failed"}});
    }
}

Result<ExportMetadataPacketSizes>
estimate_export_metadata_packets(const ExportMetadataSnapshot &metadata)
{
    ExportMetadataPacketSizes sizes;
    const auto bounded_add =
        [](std::size_t &total, const std::size_t addition, const std::size_t maximum) noexcept
    {
        if (total > maximum || addition > maximum - total)
        {
            total = maximum + 1U;
            return;
        }
        total += addition;
    };

    // Exact TIFF-profile upper bound. Every out-of-line value is padded to the
    // required word boundary; padding is not part of the IFD entry count.
    sizes.exif_tiff_profile_bytes = 80U;
    const auto add_exif_ascii = [&](const std::optional<std::string> &value)
    {
        if (!value)
        {
            return;
        }
        bounded_add(sizes.exif_tiff_profile_bytes, 12U, kExportExifTiffProfileMaxBytes);
        if (value->size() >= 4U)
        {
            bounded_add(sizes.exif_tiff_profile_bytes, value->size(),
                        kExportExifTiffProfileMaxBytes);
            bounded_add(sizes.exif_tiff_profile_bytes, 1U, kExportExifTiffProfileMaxBytes);
            if ((value->size() % 2U) == 0U)
            {
                bounded_add(sizes.exif_tiff_profile_bytes, 1U, kExportExifTiffProfileMaxBytes);
            }
        }
    };
    add_exif_ascii(metadata.writable.description);
    add_exif_ascii(metadata.writable.creator);
    add_exif_ascii(metadata.writable.copyright);
    add_exif_ascii(metadata.capture.camera_make);
    add_exif_ascii(metadata.capture.camera_model);
    if (metadata.capture.iso)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 12U, kExportExifTiffProfileMaxBytes);
    }
    if (metadata.capture.aperture)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 20U, kExportExifTiffProfileMaxBytes);
    }
    if (metadata.capture.focal_length_mm)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 20U, kExportExifTiffProfileMaxBytes);
    }
    if (metadata.capture.shutter_s)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 20U, kExportExifTiffProfileMaxBytes);
    }
    if (metadata.capture.captured_datetime)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 32U, kExportExifTiffProfileMaxBytes);
        if (metadata.capture.captured_datetime->subsecond_digits)
        {
            bounded_add(sizes.exif_tiff_profile_bytes, 24U, kExportExifTiffProfileMaxBytes);
        }
        if (metadata.capture.captured_datetime->utc_offset_minutes)
        {
            bounded_add(sizes.exif_tiff_profile_bytes, 20U, kExportExifTiffProfileMaxBytes);
        }
    }
    if (metadata.capture.location)
    {
        bounded_add(sizes.exif_tiff_profile_bytes, 220U, kExportExifTiffProfileMaxBytes);
        if (metadata.capture.location->altitude)
        {
            bounded_add(sizes.exif_tiff_profile_bytes, 32U, kExportExifTiffProfileMaxBytes);
        }
    }

    // Conservative XMP upper bound. The fixed allowance covers the packet
    // wrapper, namespaces, CreatorTool, every optional capture number, and
    // worst-case uint32 dimensions. Variable text uses the serializer's exact
    // escaping expansion and bounded wrapper allowances.
    sizes.xmp_packet_bytes = 1536U;
    const auto add_xmp_text = [&](const std::optional<std::string> &value, const std::size_t wrap)
    {
        if (value)
        {
            bounded_add(sizes.xmp_packet_bytes, wrap, kExportXmpPacketMaxBytes);
            bounded_add(sizes.xmp_packet_bytes, xml_escaped_utf8_size(*value),
                        kExportXmpPacketMaxBytes);
        }
    };
    add_xmp_text(metadata.writable.title, 128U);
    add_xmp_text(metadata.writable.description, 144U);
    add_xmp_text(metadata.writable.creator, 112U);
    add_xmp_text(metadata.writable.copyright, 128U);
    add_xmp_text(metadata.capture.camera_make, 64U);
    add_xmp_text(metadata.capture.camera_model, 64U);
    if (metadata.capture.captured_datetime)
    {
        bounded_add(sizes.xmp_packet_bytes, 96U, kExportXmpPacketMaxBytes);
        if (metadata.capture.captured_datetime->subsecond_digits)
        {
            bounded_add(sizes.xmp_packet_bytes,
                        metadata.capture.captured_datetime->subsecond_digits->size(),
                        kExportXmpPacketMaxBytes);
        }
    }
    if (metadata.capture.location)
    {
        bounded_add(sizes.xmp_packet_bytes, 160U, kExportXmpPacketMaxBytes);
        if (metadata.capture.location->altitude)
        {
            bounded_add(sizes.xmp_packet_bytes, 96U, kExportXmpPacketMaxBytes);
        }
    }
    if (!metadata.tags.empty())
    {
        bounded_add(sizes.xmp_packet_bytes, 64U, kExportXmpPacketMaxBytes);
        for (const auto &tag : metadata.tags)
        {
            bounded_add(sizes.xmp_packet_bytes, 24U, kExportXmpPacketMaxBytes);
            bounded_add(sizes.xmp_packet_bytes, xml_escaped_utf8_size(tag),
                        kExportXmpPacketMaxBytes);
        }
    }

    if (!export_iptc_should_omit(metadata))
    {
        // 1:90 UTF-8 coded-character-set marker plus mandatory 2:00 version 4.
        sizes.iptc_iim_bytes = 15U;
        const auto add_iptc = [&](const std::optional<std::string> &value)
        {
            if (value)
            {
                bounded_add(sizes.iptc_iim_bytes, 5U, kExportIptcIimMaxBytes);
                bounded_add(sizes.iptc_iim_bytes, value->size(), kExportIptcIimMaxBytes);
            }
        };
        add_iptc(metadata.writable.title);
        add_iptc(metadata.writable.creator);
        add_iptc(metadata.writable.copyright);
        add_iptc(metadata.writable.description);
        for (const auto &tag : metadata.tags)
        {
            bounded_add(sizes.iptc_iim_bytes, 5U, kExportIptcIimMaxBytes);
            bounded_add(sizes.iptc_iim_bytes, tag.size(), kExportIptcIimMaxBytes);
        }
    }

    if (sizes.exif_tiff_profile_bytes > kExportExifTiffProfileMaxBytes)
    {
        return make_error(ErrorCode::kValidation, "Export Exif packet exceeds the JPEG APP1 bound",
                          {{"maximum_bytes", std::to_string(kExportExifTiffProfileMaxBytes)},
                           {"reason", "export_exif_packet_too_large"},
                           {"size_bytes", std::to_string(sizes.exif_tiff_profile_bytes)}});
    }
    if (sizes.xmp_packet_bytes > kExportXmpPacketMaxBytes)
    {
        return make_error(ErrorCode::kValidation, "Export XMP packet exceeds the JPEG APP1 bound",
                          {{"maximum_bytes", std::to_string(kExportXmpPacketMaxBytes)},
                           {"reason", "export_xmp_packet_too_large"},
                           {"size_bytes", std::to_string(sizes.xmp_packet_bytes)}});
    }
    if (sizes.iptc_iim_bytes > kExportIptcIimMaxBytes)
    {
        return make_error(ErrorCode::kValidation, "Export IPTC packet exceeds the JPEG APP13 bound",
                          {{"maximum_bytes", std::to_string(kExportIptcIimMaxBytes)},
                           {"reason", "export_iptc_packet_too_large"},
                           {"size_bytes", std::to_string(sizes.iptc_iim_bytes)}});
    }
    return sizes;
}

Result<void> validate_export_metadata(const ExportMetadataSnapshot &metadata,
                                      const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (!metadata.embed_metadata)
    {
        const bool has_payload = !metadata.destination_document_name.empty() ||
                                 metadata.writable != WritableMetadata{} ||
                                 metadata.capture != CaptureMetadata{} || !metadata.tags.empty();
        if (has_payload)
        {
            return make_error(ErrorCode::kValidation,
                              "Disabled export metadata snapshot must not retain payload fields",
                              {{"reason", "disabled_export_metadata_has_payload"}});
        }
        return {};
    }

    const auto writable_reason = "invalid_export_metadata";
    for (const auto &[name, value] :
         std::array<std::pair<std::string_view, const std::optional<std::string> *>, 4U>{{
             {"title", &metadata.writable.title},
             {"description", &metadata.writable.description},
             {"creator", &metadata.writable.creator},
             {"copyright", &metadata.writable.copyright},
         }})
    {
        if (value->has_value())
        {
            auto bounded = validate_metadata_field(name, **value);
            if (!bounded)
            {
                return bounded.error();
            }
        }
        auto valid =
            validate_optional_utf8_field(name, *value, writable_reason, kMetadataFieldMaxLength);
        if (!valid)
        {
            return valid.error();
        }
    }
    for (const auto &[name, value, maximum, allow_line_breaks] : std::array<
             std::tuple<std::string_view, const std::optional<std::string> *, std::size_t, bool>,
             4U>{{
             {"title", &metadata.writable.title, kExportIptcTitleMaxBytes, false},
             {"description", &metadata.writable.description, kExportIptcDescriptionMaxBytes, true},
             {"creator", &metadata.writable.creator, kExportIptcCreatorMaxBytes, false},
             {"copyright", &metadata.writable.copyright, kExportIptcCopyrightMaxBytes, false},
         }})
    {
        auto valid = validate_iptc_text_field(name, *value, maximum, allow_line_breaks);
        if (!valid)
        {
            return valid.error();
        }
    }

    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    auto make =
        validate_optional_utf8_field("camera_make", metadata.capture.camera_make,
                                     "invalid_export_capture_text", kExportCaptureFieldMaxLength);
    if (!make)
    {
        return make.error();
    }
    auto model =
        validate_optional_utf8_field("camera_model", metadata.capture.camera_model,
                                     "invalid_export_capture_text", kExportCaptureFieldMaxLength);
    if (!model)
    {
        return model.error();
    }
    for (const auto &[name, value] :
         std::array<std::pair<std::string_view, const std::optional<double> *>, 4U>{{
             {"iso", &metadata.capture.iso},
             {"aperture", &metadata.capture.aperture},
             {"focal_length_mm", &metadata.capture.focal_length_mm},
             {"shutter_s", &metadata.capture.shutter_s},
         }})
    {
        auto valid = validate_capture_number(name, *value);
        if (!valid)
        {
            return valid.error();
        }
    }
    auto capture_state = validate_capture_metadata(metadata.capture);
    if (!capture_state)
    {
        return capture_state.error();
    }

    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    auto tags = canonicalize_export_tags(metadata.tags, cancellation);
    if (!tags)
    {
        return tags.error();
    }
    if (tags.value() != metadata.tags)
    {
        return make_error(ErrorCode::kValidation,
                          "Export tags must be unique, normalized, and sorted",
                          {{"reason", "invalid_export_tag_order"}});
    }
    for (const auto &tag : metadata.tags)
    {
        auto valid = validate_iptc_text_value("tag", tag, kExportIptcKeywordMaxBytes, false);
        if (!valid)
        {
            return valid.error();
        }
    }

    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto packets = estimate_export_metadata_packets(metadata);
    if (!packets)
    {
        return packets.error();
    }
    return {};
}

Result<void> validate_tiff_export_metadata(const ExportMetadataSnapshot &metadata,
                                           const CancellationToken &cancellation)
{
    auto document = validate_tiff_export_document_name(metadata.destination_document_name);
    if (!document)
    {
        return document.error();
    }
    return validate_export_metadata(metadata, cancellation);
}

std::string asset_display_name(const AssetRecord &asset)
{
    const auto slash = asset.normalized_uri.find_last_of('/');
    if (slash == std::string::npos || slash + 1U >= asset.normalized_uri.size())
    {
        return asset.id;
    }
    return asset.normalized_uri.substr(slash + 1U);
}

namespace
{

[[nodiscard]] std::string ascii_fold(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch)
                   {
                       return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') :
                                                       static_cast<char>(ch);
                   });
    return value;
}

[[nodiscard]] bool folded_contains(const std::string_view value, const std::string &folded_needle)
{
    return ascii_fold(std::string(value)).find(folded_needle) != std::string::npos;
}

[[nodiscard]] bool numeric_range_matches(const std::optional<double> value,
                                         const LibraryNumericRange &range) noexcept
{
    if (!range.minimum && !range.maximum)
        return true;
    if (!value || !std::isfinite(*value))
        return false;
    return (!range.minimum || *value >= *range.minimum) &&
           (!range.maximum || *value <= *range.maximum);
}

[[nodiscard]] Result<void> validate_library_range(const LibraryNumericRange &range,
                                                  const std::string_view name, const bool positive)
{
    const auto invalid = [&](const std::optional<double> value)
    { return value && (!std::isfinite(*value) || (positive && *value < 0.0)); };
    if (invalid(range.minimum) || invalid(range.maximum) ||
        (range.minimum && range.maximum && *range.minimum > *range.maximum))
    {
        return make_error(
            ErrorCode::kValidation, "Library numeric filter range is invalid",
            {{"field", std::string(name)}, {"reason", "invalid_library_filter_range"}});
    }
    return {};
}

[[nodiscard]] bool asset_text_matches(const AssetRecord &asset, const std::string &folded_needle)
{
    if (folded_needle.empty())
        return true;
    const auto optional_matches = [&](const std::optional<std::string> &value)
    { return value && folded_contains(*value, folded_needle); };
    if (folded_contains(asset_display_name(asset), folded_needle) ||
        folded_contains(asset.normalized_uri, folded_needle) ||
        folded_contains(asset.media_type, folded_needle) ||
        optional_matches(asset.metadata.title) || optional_matches(asset.metadata.description) ||
        optional_matches(asset.metadata.creator) || optional_matches(asset.metadata.copyright) ||
        optional_matches(asset.capture.camera_make) || optional_matches(asset.capture.camera_model))
        return true;
    return std::any_of(asset.tags.begin(), asset.tags.end(),
                       [&](const std::string &tag) { return folded_contains(tag, folded_needle); });
}

} // namespace

Result<void> validate_library_query(const LibraryQuery &query)
{
    if (query.rating_value < 0 || query.rating_value > 5)
    {
        return make_error(ErrorCode::kValidation, "Library rating filter is outside 0 to 5",
                          {{"field", "rating"}, {"reason", "invalid_library_rating_filter"}});
    }
    std::set<ColorLabel> labels;
    for (const auto label : query.color_labels)
    {
        if (label == ColorLabel::kNone || !labels.insert(label).second)
        {
            return make_error(
                ErrorCode::kValidation, "Library color filters must be unique non-empty labels",
                {{"field", "color_labels"}, {"reason", "invalid_library_color_filter"}});
        }
    }
    constexpr std::size_t kMaxFilterText = 512U;
    const auto validate_text = [&](const std::string &value,
                                   const std::string_view field) -> Result<void>
    {
        if (value.size() > kMaxFilterText || value.find('\0') != std::string::npos ||
            value.find('\n') != std::string::npos || value.find('\r') != std::string::npos)
        {
            return make_error(
                ErrorCode::kValidation, "Library text filter is invalid",
                {{"field", std::string(field)}, {"reason", "invalid_library_text_filter"}});
        }
        return {};
    };
    for (const auto &[value, field] :
         std::array<std::pair<const std::string *, std::string_view>, 4>{
             std::pair{&query.folder_uri, std::string_view{"folder_uri"}},
             std::pair{&query.tag, std::string_view{"tag"}},
             std::pair{&query.text, std::string_view{"text"}},
             std::pair{&query.camera, std::string_view{"camera"}}})
    {
        auto valid = validate_text(*value, field);
        if (!valid)
            return valid.error();
    }
    if (!query.tag.empty())
    {
        auto normalized = normalize_tag_name(query.tag);
        if (!normalized || normalized.value() != query.tag)
        {
            return make_error(ErrorCode::kValidation, "Library tag filter is not canonical",
                              {{"field", "tag"}, {"reason", "invalid_library_tag_filter"}});
        }
    }
    if (query.media_types.size() > 32U)
    {
        return make_error(ErrorCode::kValidation, "Library media filter is too large",
                          {{"field", "media_types"}, {"reason", "invalid_library_media_filter"}});
    }
    std::set<std::string, std::less<>> media;
    for (const auto &type : query.media_types)
    {
        auto valid = validate_text(type, "media_types");
        if (!valid)
            return valid.error();
        if (type.empty() || !media.insert(type).second)
        {
            return make_error(
                ErrorCode::kValidation, "Library media filters must be unique and non-empty",
                {{"field", "media_types"}, {"reason", "invalid_library_media_filter"}});
        }
    }
    for (const auto &[range, name] :
         std::array<std::pair<const LibraryNumericRange *, std::string_view>, 5>{
             std::pair{&query.iso, std::string_view{"iso"}},
             std::pair{&query.aperture, std::string_view{"aperture"}},
             std::pair{&query.focal_length_mm, std::string_view{"focal_length_mm"}},
             std::pair{&query.shutter_s, std::string_view{"shutter_s"}},
             std::pair{&query.aspect_ratio, std::string_view{"aspect_ratio"}}})
    {
        auto valid = validate_library_range(*range, name, true);
        if (!valid)
            return valid.error();
    }
    const auto invalid_time = [](const auto &after, const auto &before)
    { return after && before && *after > *before; };
    if (invalid_time(query.imported_after_unix_ms, query.imported_before_unix_ms) ||
        invalid_time(query.captured_after_unix_s, query.captured_before_unix_s))
    {
        return make_error(ErrorCode::kValidation, "Library time filter range is invalid",
                          {{"reason", "invalid_library_time_filter"}});
    }
    return {};
}

bool asset_matches_query(const AssetRecord &asset, const LibraryQuery &query)
{
    switch (query.rating_mode)
    {
    case RatingFilterMode::kAny:
        break;
    case RatingFilterMode::kMinimum:
        if (asset.review.rating < query.rating_value)
        {
            return false;
        }
        break;
    case RatingFilterMode::kExact:
        if (asset.review.rating != query.rating_value)
        {
            return false;
        }
        break;
    }

    if (!query.color_labels.empty())
    {
        const auto found = std::find(query.color_labels.begin(), query.color_labels.end(),
                                     asset.review.color_label);
        if (found == query.color_labels.end())
        {
            return false;
        }
    }

    switch (query.reject_filter)
    {
    case RejectFilter::kInclude:
        break;
    case RejectFilter::kExclude:
        if (asset.review.rejected)
        {
            return false;
        }
        break;
    case RejectFilter::kOnly:
        if (!asset.review.rejected)
        {
            return false;
        }
        break;
    }
    if (!asset_in_folder(asset, query.folder_uri))
    {
        return false;
    }
    if (!query.tag.empty())
    {
        const auto found = std::find(asset.tags.begin(), asset.tags.end(), query.tag);
        if (found == asset.tags.end())
        {
            return false;
        }
    }
    if (!query.text.empty() && !asset_text_matches(asset, ascii_fold(query.text)))
        return false;
    if (!query.media_types.empty() && std::find(query.media_types.begin(), query.media_types.end(),
                                                asset.media_type) == query.media_types.end())
        return false;
    if ((query.edit_filter == EditFilter::kEdited && !asset.has_edits) ||
        (query.edit_filter == EditFilter::kUnedited && asset.has_edits))
        return false;
    if (!query.camera.empty())
    {
        const std::string needle = ascii_fold(query.camera);
        const bool make =
            asset.capture.camera_make && folded_contains(*asset.capture.camera_make, needle);
        const bool model =
            asset.capture.camera_model && folded_contains(*asset.capture.camera_model, needle);
        if (!make && !model)
            return false;
    }
    if (!numeric_range_matches(asset.capture.iso, query.iso) ||
        !numeric_range_matches(asset.capture.aperture, query.aperture) ||
        !numeric_range_matches(asset.capture.focal_length_mm, query.focal_length_mm) ||
        !numeric_range_matches(asset.capture.shutter_s, query.shutter_s))
        return false;
    std::optional<double> ratio;
    if (asset.width && asset.height && *asset.height != 0U)
        ratio = static_cast<double>(*asset.width) / static_cast<double>(*asset.height);
    if (!numeric_range_matches(ratio, query.aspect_ratio))
        return false;
    if ((query.imported_after_unix_ms && asset.created_unix_ms < *query.imported_after_unix_ms) ||
        (query.imported_before_unix_ms && asset.created_unix_ms > *query.imported_before_unix_ms))
        return false;
    if (query.captured_after_unix_s || query.captured_before_unix_s)
    {
        if (!asset.capture.captured_unix_s ||
            (query.captured_after_unix_s &&
             *asset.capture.captured_unix_s < *query.captured_after_unix_s) ||
            (query.captured_before_unix_s &&
             *asset.capture.captured_unix_s > *query.captured_before_unix_s))
            return false;
    }
    return true;
}

Result<std::string> normalize_tag_name(const std::string_view name)
{
    std::string trimmed;
    trimmed.reserve(name.size());
    std::size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t' ||
                                   name[start] == '\n' || name[start] == '\r'))
    {
        ++start;
    }
    std::size_t end = name.size();
    while (end > start && (name[end - 1U] == ' ' || name[end - 1U] == '\t' ||
                           name[end - 1U] == '\n' || name[end - 1U] == '\r'))
    {
        --end;
    }
    for (std::size_t index = start; index < end; ++index)
    {
        const auto byte = static_cast<unsigned char>(name[index]);
        if (byte == '\0' || byte == '\n' || byte == '\r')
        {
            return make_error(ErrorCode::kValidation,
                              "Tag name contains an invalid control character");
        }
        trimmed.push_back(name[index]);
    }
    if (trimmed.empty())
    {
        return make_error(ErrorCode::kValidation, "Tag name must not be empty");
    }
    if (trimmed.size() > kTagMaxLength)
    {
        return make_error(ErrorCode::kValidation, "Tag name exceeds the maximum length",
                          {{"max_length", std::to_string(kTagMaxLength)}});
    }
    return trimmed;
}

Result<std::vector<std::string>> parse_tag_list(const std::string_view text)
{
    std::vector<std::string> tags;
    std::string current;
    auto flush = [&]() -> Result<void>
    {
        if (current.empty())
        {
            return {};
        }
        auto normalized = normalize_tag_name(current);
        current.clear();
        if (!normalized)
        {
            return normalized.error();
        }
        if (std::find(tags.begin(), tags.end(), normalized.value()) == tags.end())
        {
            tags.push_back(std::move(normalized).value());
        }
        return {};
    };
    for (const char ch : text)
    {
        if (ch == ',' || ch == ';')
        {
            auto flushed = flush();
            if (!flushed)
            {
                return flushed.error();
            }
            continue;
        }
        current.push_back(ch);
    }
    auto flushed = flush();
    if (!flushed)
    {
        return flushed.error();
    }
    return tags;
}

Result<void> validate_metadata_field(const std::string_view name, const std::string_view value)
{
    if (name != "title" && name != "description" && name != "creator" && name != "copyright")
    {
        return make_error(ErrorCode::kInvalidArgument, "Writable metadata field is unknown",
                          {{"field", std::string(name)}});
    }
    if (value.size() > kMetadataFieldMaxLength)
    {
        return make_error(ErrorCode::kValidation,
                          "Writable metadata field exceeds the maximum length",
                          {{"field", std::string(name)},
                           {"max_length", std::to_string(kMetadataFieldMaxLength)}});
    }
    if (value.find('\0') != std::string_view::npos)
    {
        return make_error(ErrorCode::kValidation, "Writable metadata cannot contain NUL",
                          {{"field", std::string(name)}});
    }
    return {};
}

bool asset_in_folder(const AssetRecord &asset, const std::string_view folder_uri) noexcept
{
    if (folder_uri.empty())
    {
        return true;
    }
    const auto prefix = std::string(folder_uri) + "/";
    return asset.normalized_uri.starts_with(prefix);
}

std::vector<FolderRecord> library_folders(const std::vector<AssetRecord> &assets)
{
    std::vector<FolderRecord> folders;
    FolderRecord all;
    all.display_name = "All Photographs";
    all.asset_count = static_cast<int>(assets.size());
    folders.push_back(std::move(all));
    if (assets.empty())
    {
        return folders;
    }

    std::set<std::string> folder_uris;
    for (const auto &asset : assets)
    {
        auto parent = uri_parent(asset.normalized_uri);
        while (!parent.empty())
        {
            folder_uris.insert(parent);
            parent = uri_parent(parent);
        }
    }
    if (folder_uris.empty())
    {
        return folders;
    }

    std::string common = *folder_uris.begin();
    for (const auto &uri : folder_uris)
    {
        while (!common.empty() && uri != common && !uri.starts_with(common + "/"))
        {
            common = uri_parent(common);
        }
    }

    std::map<std::string, int> counts;
    for (const auto &uri : folder_uris)
    {
        if (!common.empty() && uri != common && !uri.starts_with(common + "/"))
        {
            continue;
        }
        int count = 0;
        for (const auto &asset : assets)
        {
            if (asset_in_folder(asset, uri))
            {
                ++count;
            }
        }
        counts[uri] = count;
    }

    auto slash_count = [](const std::string &text)
    { return static_cast<int>(std::count(text.begin(), text.end(), '/')); };
    const auto common_slashes = common.empty() ? 0 : slash_count(common);
    for (const auto &[uri, count] : counts)
    {
        FolderRecord folder;
        folder.uri = uri;
        folder.display_name = uri_display_name(uri);
        folder.depth = std::max(0, slash_count(uri) - common_slashes);
        folder.asset_count = count;
        folders.push_back(std::move(folder));
    }
    return folders;
}

std::vector<AssetRecord> filter_and_sort_assets(std::vector<AssetRecord> assets,
                                                const LibraryQuery &query)
{
    assets.erase(std::remove_if(assets.begin(), assets.end(), [&query](const AssetRecord &asset)
                                { return !asset_matches_query(asset, query); }),
                 assets.end());
    std::sort(assets.begin(), assets.end(),
              [&query](const AssetRecord &left, const AssetRecord &right)
              {
                  if (query.sort_field == AssetSortField::kCaptureTime &&
                      left.capture.captured_unix_s.has_value() !=
                          right.capture.captured_unix_s.has_value())
                  {
                      return left.capture.captured_unix_s.has_value();
                  }
                  int comparison = 0;
                  switch (query.sort_field)
                  {
                  case AssetSortField::kImportTime:
                      if (left.created_unix_ms < right.created_unix_ms)
                      {
                          comparison = -1;
                      }
                      else if (left.created_unix_ms > right.created_unix_ms)
                      {
                          comparison = 1;
                      }
                      break;
                  case AssetSortField::kCaptureTime:
                      if (left.capture.captured_unix_s && right.capture.captured_unix_s)
                      {
                          if (*left.capture.captured_unix_s < *right.capture.captured_unix_s)
                              comparison = -1;
                          else if (*left.capture.captured_unix_s > *right.capture.captured_unix_s)
                              comparison = 1;
                      }
                      break;
                  case AssetSortField::kDisplayName:
                      comparison = asset_display_name(left).compare(asset_display_name(right));
                      break;
                  case AssetSortField::kRating:
                      comparison = left.review.rating - right.review.rating;
                      break;
                  case AssetSortField::kFileSize:
                      if (left.size_bytes < right.size_bytes)
                          comparison = -1;
                      else if (left.size_bytes > right.size_bytes)
                          comparison = 1;
                      break;
                  }
                  if (comparison == 0)
                  {
                      comparison = left.id.compare(right.id);
                  }
                  return query.sort_direction == SortDirection::kAscending ? comparison < 0 :
                                                                             comparison > 0;
              });
    return assets;
}

} // namespace ravo
