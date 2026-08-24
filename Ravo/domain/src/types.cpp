#include "ravo/domain/types.h"

#include "ravo/domain/uri.h"

#include <algorithm>
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
    return true;
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
