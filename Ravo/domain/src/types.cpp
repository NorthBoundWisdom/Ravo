#include "ravo/domain/types.h"

#include "ravo/domain/uri.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <random>
#include <set>
#include <sstream>

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
    return {};
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
                  case AssetSortField::kDisplayName:
                      comparison = asset_display_name(left).compare(asset_display_name(right));
                      break;
                  case AssetSortField::kRating:
                      comparison = left.review.rating - right.review.rating;
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
