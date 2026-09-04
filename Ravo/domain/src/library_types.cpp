#include "ravo/domain/types.h"

#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <tuple>

namespace ravo
{
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
        optional_matches(asset.metadata.country) ||
        optional_matches(asset.metadata.province_state) || optional_matches(asset.metadata.city) ||
        optional_matches(asset.metadata.sublocation) ||
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
    for (const auto &[value, field] :
         std::array<std::pair<const std::optional<std::string> *, std::string_view>, 9>{
             std::pair{&query.camera_make_equals, std::string_view{"camera_make_equals"}},
             std::pair{&query.camera_model_equals, std::string_view{"camera_model_equals"}},
             std::pair{&query.lens_make_equals, std::string_view{"lens_make_equals"}},
             std::pair{&query.lens_model_equals, std::string_view{"lens_model_equals"}},
             std::pair{&query.captured_local_date, std::string_view{"captured_local_date"}},
             std::pair{&query.country_equals, std::string_view{"country_equals"}},
             std::pair{&query.province_state_equals, std::string_view{"province_state_equals"}},
             std::pair{&query.city_equals, std::string_view{"city_equals"}},
             std::pair{&query.sublocation_equals, std::string_view{"sublocation_equals"}}})
    {
        if (!*value)
            continue;
        auto valid = validate_text(**value, field);
        if (!valid)
            return valid.error();
    }
    if (query.captured_local_date)
    {
        const auto &day = *query.captured_local_date;
        const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        if (day.size() != 10U || !is_digit(day[0]) || !is_digit(day[1]) || !is_digit(day[2]) ||
            !is_digit(day[3]) || day[4] != ':' || !is_digit(day[5]) || !is_digit(day[6]) ||
            day[7] != ':' || !is_digit(day[8]) || !is_digit(day[9]))
        {
            return make_error(ErrorCode::kValidation, "Library capture-date facet is invalid",
                              {{"field", "captured_local_date"},
                               {"reason", "invalid_library_capture_date_facet"}});
        }
    }
    if ((query.camera_make_equals.has_value() != query.camera_model_equals.has_value()))
    {
        return make_error(ErrorCode::kValidation,
                          "Library camera facet requires both make and model equality selectors",
                          {{"reason", "invalid_library_camera_facet"}});
    }
    if ((query.lens_make_equals.has_value() != query.lens_model_equals.has_value()))
    {
        return make_error(ErrorCode::kValidation,
                          "Library lens-name facet requires both make and model equality selectors",
                          {{"reason", "invalid_library_lens_name_facet"}});
    }
    if (query.focal_length_mm_equals)
    {
        if (!std::isfinite(*query.focal_length_mm_equals) || *query.focal_length_mm_equals <= 0.0)
        {
            return make_error(
                ErrorCode::kValidation, "Library lens facet focal length is invalid",
                {{"field", "focal_length_mm_equals"}, {"reason", "invalid_library_lens_facet"}});
        }
        if (query.focal_length_mm.minimum &&
            *query.focal_length_mm.minimum > *query.focal_length_mm_equals + 1.0e-9)
        {
            return make_error(
                ErrorCode::kValidation, "Library lens facet contradicts focal length range",
                {{"field", "focal_length_mm_equals"}, {"reason", "invalid_library_lens_facet"}});
        }
        if (query.focal_length_mm.maximum &&
            *query.focal_length_mm.maximum < *query.focal_length_mm_equals - 1.0e-9)
        {
            return make_error(
                ErrorCode::kValidation, "Library lens facet contradicts focal length range",
                {{"field", "focal_length_mm_equals"}, {"reason", "invalid_library_lens_facet"}});
        }
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
    if (!query.collection_id.empty())
    {
        constexpr std::string_view kPrefix = "set_";
        if (query.collection_id.size() != kPrefix.size() + 32U ||
            !query.collection_id.starts_with(kPrefix))
        {
            return make_error(ErrorCode::kValidation, "Library set id is invalid",
                              {{"field", "collection_id"}, {"reason", "invalid_library_set_id"}});
        }
        for (std::size_t index = kPrefix.size(); index < query.collection_id.size(); ++index)
        {
            const auto byte = static_cast<unsigned char>(query.collection_id[index]);
            const bool hex = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
            if (!hex)
            {
                return make_error(
                    ErrorCode::kValidation, "Library set id is invalid",
                    {{"field", "collection_id"}, {"reason", "invalid_library_set_id"}});
            }
        }
    }
    return {};
}

Result<void> validate_library_page_request(const LibraryPageRequest &request)
{
    auto valid = validate_library_query(request.query);
    if (!valid)
        return valid.error();
    if (request.additional_query)
    {
        if (!request.additional_query->collection_id.empty())
        {
            return make_error(ErrorCode::kValidation,
                              "Additional library query cannot select a named set",
                              {{"field", "additional_query.collection_id"},
                               {"reason", "invalid_library_additional_query"}});
        }
        auto extra = validate_library_query(*request.additional_query);
        if (!extra)
            return extra.error();
    }
    if (request.limit == 0U || request.limit > kLibraryPageMaximumSize ||
        request.offset > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
        (request.after_asset_id &&
         (request.after_asset_id->empty() || request.after_asset_id->size() > 180U ||
          request.offset == 0U)) ||
        (request.known_total &&
         (*request.known_total < request.offset ||
          *request.known_total >
              static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))))
        return make_error(ErrorCode::kValidation, "Library page request is outside its bounds",
                          {{"limit", std::to_string(request.limit)},
                           {"maximum_limit", std::to_string(kLibraryPageMaximumSize)},
                           {"offset", std::to_string(request.offset)},
                           {"reason", "invalid_library_page"}});
    return {};
}

bool asset_matches_query(const AssetRecord &asset, const LibraryQuery &query)
{
    if (!query.collection_id.empty())
        return false;
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
    if (query.camera_make_equals || query.camera_model_equals)
    {
        const auto equals_absent =
            [](const std::optional<std::string> &field, const std::string &expected)
        { return field.value_or(std::string{}) == expected; };
        if (query.camera_make_equals &&
            !equals_absent(asset.capture.camera_make, *query.camera_make_equals))
            return false;
        if (query.camera_model_equals &&
            !equals_absent(asset.capture.camera_model, *query.camera_model_equals))
            return false;
    }
    if (query.lens_make_equals || query.lens_model_equals)
    {
        const auto equals_absent =
            [](const std::optional<std::string> &field, const std::string &expected)
        { return field.value_or(std::string{}) == expected; };
        if (query.lens_make_equals &&
            !equals_absent(asset.capture.lens_make, *query.lens_make_equals))
            return false;
        if (query.lens_model_equals &&
            !equals_absent(asset.capture.lens_model, *query.lens_model_equals))
            return false;
    }
    if (query.focal_length_mm_equals)
    {
        if (!asset.capture.focal_length_mm ||
            std::abs(*asset.capture.focal_length_mm - *query.focal_length_mm_equals) > 1.0e-9)
            return false;
    }
    if (query.captured_local_date)
    {
        if (!asset.capture.captured_datetime ||
            asset.capture.captured_datetime->local_exif.size() < 10U ||
            asset.capture.captured_datetime->local_exif.compare(0, 10,
                                                                *query.captured_local_date) != 0)
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
    const auto location_equals =
        [](const std::optional<std::string> &field, const std::optional<std::string> &expected)
    {
        if (!expected)
            return true;
        return field.value_or(std::string{}) == *expected;
    };
    if (!location_equals(asset.metadata.country, query.country_equals) ||
        !location_equals(asset.metadata.province_state, query.province_state_equals) ||
        !location_equals(asset.metadata.city, query.city_equals) ||
        !location_equals(asset.metadata.sublocation, query.sublocation_equals))
        return false;
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

Result<std::string> normalize_keyword_name(const std::string_view name)
{
    auto normalized = normalize_tag_name(name);
    if (!normalized)
    {
        return normalized.error();
    }
    if (normalized.value().find(kKeywordPathSeparator) != std::string::npos)
    {
        return make_error(ErrorCode::kValidation,
                          "Keyword name must not contain the path separator",
                          {{"reason", "invalid_keyword_name_separator"},
                           {"separator", std::string(1, kKeywordPathSeparator)}});
    }
    return normalized;
}

Result<std::vector<std::string>> parse_keyword_path(const std::string_view path)
{
    if (path.empty())
    {
        return make_error(ErrorCode::kValidation, "Keyword path must not be empty",
                          {{"reason", "empty_keyword_path"}});
    }
    if (path.size() > kKeywordPathMaxLength)
    {
        return make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum length",
                          {{"reason", "keyword_path_too_long"},
                           {"max_length", std::to_string(kKeywordPathMaxLength)}});
    }
    std::vector<std::string> segments;
    std::string current;
    auto flush = [&]() -> Result<void>
    {
        if (current.empty())
        {
            return make_error(ErrorCode::kValidation, "Keyword path contains an empty segment",
                              {{"reason", "empty_keyword_path_segment"}});
        }
        auto normalized = normalize_keyword_name(current);
        current.clear();
        if (!normalized)
        {
            return normalized.error();
        }
        segments.push_back(std::move(normalized).value());
        return {};
    };
    for (const char ch : path)
    {
        if (ch == kKeywordPathSeparator)
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
    if (segments.size() > kKeywordMaximumDepth)
    {
        return make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum depth",
                          {{"reason", "keyword_path_too_deep"},
                           {"maximum", std::to_string(kKeywordMaximumDepth)},
                           {"depth", std::to_string(segments.size())}});
    }
    return segments;
}

Result<std::string> join_keyword_path(const std::vector<std::string> &segments)
{
    if (segments.empty())
    {
        return make_error(ErrorCode::kValidation, "Keyword path must not be empty",
                          {{"reason", "empty_keyword_path"}});
    }
    std::string path;
    for (std::size_t index = 0; index < segments.size(); ++index)
    {
        auto normalized = normalize_keyword_name(segments[index]);
        if (!normalized)
        {
            return normalized.error();
        }
        if (index != 0)
        {
            path.push_back(kKeywordPathSeparator);
        }
        path += normalized.value();
        if (path.size() > kKeywordPathMaxLength)
        {
            return make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum length",
                              {{"reason", "keyword_path_too_long"},
                               {"max_length", std::to_string(kKeywordPathMaxLength)}});
        }
    }
    if (segments.size() > kKeywordMaximumDepth)
    {
        return make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum depth",
                          {{"reason", "keyword_path_too_deep"},
                           {"maximum", std::to_string(kKeywordMaximumDepth)},
                           {"depth", std::to_string(segments.size())}});
    }
    return path;
}

Result<void> validate_metadata_field(const std::string_view name, const std::string_view value)
{
    if (name != "title" && name != "description" && name != "creator" && name != "copyright" &&
        name != "country" && name != "province_state" && name != "city" && name != "sublocation" &&
        name != "headline" && name != "credit" && name != "source" && name != "instructions" &&
        name != "usage_terms" && name != "job_id")
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

Result<WritableMetadataPatch>
writable_metadata_patch_for_field(const std::string_view name,
                                  const std::optional<std::string> &value)
{
    if (value)
    {
        auto valid = validate_metadata_field(name, *value);
        if (!valid)
            return valid.error();
    }
    else if (name != "title" && name != "description" && name != "creator" && name != "copyright" &&
             name != "country" && name != "province_state" && name != "city" &&
             name != "sublocation" && name != "headline" && name != "credit" && name != "source" &&
             name != "instructions" && name != "usage_terms" && name != "job_id")
    {
        return make_error(ErrorCode::kInvalidArgument, "Writable metadata field is unknown",
                          {{"field", std::string(name)}});
    }
    WritableMetadataPatch patch;
    if (name == "title")
    {
        patch.update_title = true;
        patch.title = value;
    }
    else if (name == "description")
    {
        patch.update_description = true;
        patch.description = value;
    }
    else if (name == "creator")
    {
        patch.update_creator = true;
        patch.creator = value;
    }
    else if (name == "copyright")
    {
        patch.update_copyright = true;
        patch.copyright = value;
    }
    else if (name == "country")
    {
        patch.update_country = true;
        patch.country = value;
    }
    else if (name == "province_state")
    {
        patch.update_province_state = true;
        patch.province_state = value;
    }
    else if (name == "city")
    {
        patch.update_city = true;
        patch.city = value;
    }
    else if (name == "sublocation")
    {
        patch.update_sublocation = true;
        patch.sublocation = value;
    }
    else if (name == "headline")
    {
        patch.update_headline = true;
        patch.headline = value;
    }
    else if (name == "credit")
    {
        patch.update_credit = true;
        patch.credit = value;
    }
    else if (name == "source")
    {
        patch.update_source = true;
        patch.source = value;
    }
    else if (name == "instructions")
    {
        patch.update_instructions = true;
        patch.instructions = value;
    }
    else if (name == "usage_terms")
    {
        patch.update_usage_terms = true;
        patch.usage_terms = value;
    }
    else if (name == "job_id")
    {
        patch.update_job_id = true;
        patch.job_id = value;
    }
    return patch;
}

WritableMetadataPatch writable_metadata_patch_all(const WritableMetadata &metadata)
{
    WritableMetadataPatch patch;
    patch.update_title = true;
    patch.title = metadata.title;
    patch.update_description = true;
    patch.description = metadata.description;
    patch.update_creator = true;
    patch.creator = metadata.creator;
    patch.update_copyright = true;
    patch.copyright = metadata.copyright;
    patch.update_country = true;
    patch.country = metadata.country;
    patch.update_province_state = true;
    patch.province_state = metadata.province_state;
    patch.update_city = true;
    patch.city = metadata.city;
    patch.update_sublocation = true;
    patch.sublocation = metadata.sublocation;
    patch.update_headline = true;
    patch.headline = metadata.headline;
    patch.update_credit = true;
    patch.credit = metadata.credit;
    patch.update_source = true;
    patch.source = metadata.source;
    patch.update_instructions = true;
    patch.instructions = metadata.instructions;
    patch.update_usage_terms = true;
    patch.usage_terms = metadata.usage_terms;
    patch.update_job_id = true;
    patch.job_id = metadata.job_id;
    return patch;
}

void apply_writable_metadata_patch(WritableMetadata &metadata,
                                   const WritableMetadataPatch &patch) noexcept
{
    if (patch.update_title)
        metadata.title = patch.title;
    if (patch.update_description)
        metadata.description = patch.description;
    if (patch.update_creator)
        metadata.creator = patch.creator;
    if (patch.update_copyright)
        metadata.copyright = patch.copyright;
    if (patch.update_country)
        metadata.country = patch.country;
    if (patch.update_province_state)
        metadata.province_state = patch.province_state;
    if (patch.update_city)
        metadata.city = patch.city;
    if (patch.update_sublocation)
        metadata.sublocation = patch.sublocation;
    if (patch.update_headline)
        metadata.headline = patch.headline;
    if (patch.update_credit)
        metadata.credit = patch.credit;
    if (patch.update_source)
        metadata.source = patch.source;
    if (patch.update_instructions)
        metadata.instructions = patch.instructions;
    if (patch.update_usage_terms)
        metadata.usage_terms = patch.usage_terms;
    if (patch.update_job_id)
        metadata.job_id = patch.job_id;
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
    std::map<std::string, int, std::less<>> direct;
    for (const auto &asset : assets)
        ++direct[uri_parent(asset.normalized_uri)];
    std::vector<FolderAssetCount> counts;
    counts.reserve(direct.size());
    for (auto &[uri, count] : direct)
        counts.push_back({{}, std::move(uri), count, false});
    return library_folders_from_counts(counts, static_cast<int>(assets.size()));
}

std::vector<FolderRecord>
library_folders_from_counts(const std::vector<FolderAssetCount> &direct_counts,
                            const int total_asset_count)
{
    std::vector<FolderRecord> folders;
    FolderRecord all;
    all.display_name = "All Photographs";
    all.asset_count = std::max(0, total_asset_count);
    folders.push_back(std::move(all));
    if (direct_counts.empty())
        return folders;

    std::map<std::string, int, std::less<>> counts;
    std::map<std::string, const FolderAssetCount *, std::less<>> direct_by_uri;
    for (const auto &direct : direct_counts)
    {
        if (direct.uri.empty() || direct.direct_asset_count <= 0)
            continue;
        direct_by_uri.insert_or_assign(direct.uri, &direct);
        auto current = direct.uri;
        while (!current.empty())
        {
            counts[current] += direct.direct_asset_count;
            current = uri_parent(current);
        }
    }
    if (counts.empty())
        return folders;

    std::string common = counts.begin()->first;
    for (const auto &[uri, count] : counts)
    {
        static_cast<void>(count);
        while (!common.empty() && uri != common && !uri.starts_with(common + "/"))
            common = uri_parent(common);
    }

    auto slash_count = [](const std::string &text)
    { return static_cast<int>(std::count(text.begin(), text.end(), '/')); };
    const auto common_slashes = common.empty() ? 0 : slash_count(common);
    for (const auto &[uri, count] : counts)
    {
        if (!common.empty() && uri != common && !uri.starts_with(common + "/"))
            continue;
        FolderRecord folder;
        folder.uri = uri;
        folder.display_name = uri_display_name(uri);
        folder.depth = std::max(0, slash_count(uri) - common_slashes);
        folder.asset_count = count;
        if (const auto direct = direct_by_uri.find(uri); direct != direct_by_uri.end())
        {
            folder.id = direct->second->id;
            folder.missing = direct->second->missing;
        }
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

Result<std::string> normalize_library_set_name(const std::string_view name)
{
    std::size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t' ||
                                   name[start] == '\n' || name[start] == '\r'))
        ++start;
    std::size_t end = name.size();
    while (end > start && (name[end - 1U] == ' ' || name[end - 1U] == '\t' ||
                           name[end - 1U] == '\n' || name[end - 1U] == '\r'))
        --end;
    std::string trimmed;
    trimmed.reserve(end - start);
    for (std::size_t index = start; index < end; ++index)
    {
        const auto byte = static_cast<unsigned char>(name[index]);
        if (byte < 0x20U || byte == 0x7FU)
        {
            return make_error(ErrorCode::kValidation,
                              "Library set name contains an invalid character",
                              {{"reason", "invalid_library_set_name"}});
        }
        trimmed.push_back(name[index]);
    }
    if (trimmed.empty())
        return make_error(ErrorCode::kValidation, "Library set name must not be empty",
                          {{"reason", "invalid_library_set_name"}});
    if (trimmed.size() > kLibrarySetNameMaxLength)
    {
        return make_error(ErrorCode::kValidation, "Library set name exceeds the maximum length",
                          {{"max_length", std::to_string(kLibrarySetNameMaxLength)},
                           {"reason", "invalid_library_set_name"}});
    }
    return trimmed;
}

std::string_view library_set_kind_name(const LibrarySetKind kind) noexcept
{
    switch (kind)
    {
    case LibrarySetKind::kManual:
        return kLibrarySetKindManual;
    case LibrarySetKind::kSmart:
        return kLibrarySetKindSmart;
    }
    return kLibrarySetKindManual;
}

Result<LibrarySetKind> parse_library_set_kind(const std::string_view name)
{
    if (name == kLibrarySetKindManual)
        return LibrarySetKind::kManual;
    if (name == kLibrarySetKindSmart)
        return LibrarySetKind::kSmart;
    return make_error(ErrorCode::kValidation, "Library set kind is invalid",
                      {{"kind", std::string(name)}, {"reason", "invalid_library_set_kind"}});
}

namespace
{

[[nodiscard]] const char *rating_mode_name(const RatingFilterMode mode) noexcept
{
    switch (mode)
    {
    case RatingFilterMode::kAny:
        return "any";
    case RatingFilterMode::kMinimum:
        return "minimum";
    case RatingFilterMode::kExact:
        return "exact";
    }
    return "any";
}

[[nodiscard]] Result<RatingFilterMode> parse_rating_mode(const std::string_view name)
{
    if (name == "any")
        return RatingFilterMode::kAny;
    if (name == "minimum")
        return RatingFilterMode::kMinimum;
    if (name == "exact")
        return RatingFilterMode::kExact;
    return make_error(ErrorCode::kValidation, "Library rating mode is invalid",
                      {{"rating_mode", std::string(name)}, {"reason", "invalid_library_query"}});
}

[[nodiscard]] const char *reject_filter_name(const RejectFilter mode) noexcept
{
    switch (mode)
    {
    case RejectFilter::kInclude:
        return "include";
    case RejectFilter::kExclude:
        return "exclude";
    case RejectFilter::kOnly:
        return "only";
    }
    return "include";
}

[[nodiscard]] Result<RejectFilter> parse_reject_filter_name(const std::string_view name)
{
    if (name == "include")
        return RejectFilter::kInclude;
    if (name == "exclude")
        return RejectFilter::kExclude;
    if (name == "only")
        return RejectFilter::kOnly;
    return make_error(ErrorCode::kValidation, "Library reject filter is invalid",
                      {{"reject_filter", std::string(name)}, {"reason", "invalid_library_query"}});
}

[[nodiscard]] const char *edit_filter_name(const EditFilter mode) noexcept
{
    switch (mode)
    {
    case EditFilter::kAny:
        return "any";
    case EditFilter::kEdited:
        return "edited";
    case EditFilter::kUnedited:
        return "unedited";
    }
    return "any";
}

[[nodiscard]] Result<EditFilter> parse_edit_filter_name(const std::string_view name)
{
    if (name == "any")
        return EditFilter::kAny;
    if (name == "edited")
        return EditFilter::kEdited;
    if (name == "unedited")
        return EditFilter::kUnedited;
    return make_error(ErrorCode::kValidation, "Library edit filter is invalid",
                      {{"edit_filter", std::string(name)}, {"reason", "invalid_library_query"}});
}

[[nodiscard]] const char *sort_field_name(const AssetSortField field) noexcept
{
    switch (field)
    {
    case AssetSortField::kImportTime:
        return "import_time";
    case AssetSortField::kCaptureTime:
        return "capture_time";
    case AssetSortField::kDisplayName:
        return "display_name";
    case AssetSortField::kRating:
        return "rating";
    case AssetSortField::kFileSize:
        return "file_size";
    }
    return "import_time";
}

[[nodiscard]] Result<AssetSortField> parse_sort_field_name(const std::string_view name)
{
    if (name == "import_time")
        return AssetSortField::kImportTime;
    if (name == "capture_time")
        return AssetSortField::kCaptureTime;
    if (name == "display_name")
        return AssetSortField::kDisplayName;
    if (name == "rating")
        return AssetSortField::kRating;
    if (name == "file_size")
        return AssetSortField::kFileSize;
    return make_error(ErrorCode::kValidation, "Library sort field is invalid",
                      {{"sort_field", std::string(name)}, {"reason", "invalid_library_query"}});
}

[[nodiscard]] JsonValue range_to_json(const LibraryNumericRange &range)
{
    JsonValue::Object object;
    if (range.minimum)
        object.emplace("minimum", JsonValue::number(std::to_string(*range.minimum)));
    else
        object.emplace("minimum", JsonValue{nullptr});
    if (range.maximum)
        object.emplace("maximum", JsonValue::number(std::to_string(*range.maximum)));
    else
        object.emplace("maximum", JsonValue{nullptr});
    return JsonValue{std::move(object)};
}

[[nodiscard]] bool parse_library_query_double(const std::string_view text, double &result)
{
    if (text.empty())
        return false;
    // Apple libc++ does not yet provide floating-point std::from_chars. JSON
    // always uses '.', so require classic-locale, finite, complete consumption.
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double parsed = 0.0;
    if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(parsed))
        return false;
    result = parsed;
    return true;
}

[[nodiscard]] Result<std::optional<double>> json_optional_double(const JsonValue &value,
                                                                 const std::string_view field)
{
    if (value.is_null())
        return std::optional<double>{};
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Library query number is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    double parsed = 0.0;
    if (!parse_library_query_double(number->text, parsed))
    {
        return make_error(ErrorCode::kValidation, "Library query number is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    return std::optional<double>{parsed};
}

[[nodiscard]] Result<std::optional<std::int64_t>> json_optional_int64(const JsonValue &value,
                                                                      const std::string_view field)
{
    if (value.is_null())
        return std::optional<std::int64_t>{};
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Library query integer is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    std::int64_t parsed = 0;
    const auto *begin = number->text.data();
    const auto *end = begin + number->text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return make_error(ErrorCode::kValidation, "Library query integer is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    return std::optional<std::int64_t>{parsed};
}

[[nodiscard]] Result<LibraryNumericRange> json_range(const JsonValue &value,
                                                     const std::string_view field)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Library query range is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    const auto *minimum = value.find("minimum");
    const auto *maximum = value.find("maximum");
    if (minimum == nullptr || maximum == nullptr || object->size() != 2U)
    {
        return make_error(ErrorCode::kValidation, "Library query range is invalid",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    auto parsed_min = json_optional_double(*minimum, field);
    if (!parsed_min)
        return parsed_min.error();
    auto parsed_max = json_optional_double(*maximum, field);
    if (!parsed_max)
        return parsed_max.error();
    return LibraryNumericRange{parsed_min.value(), parsed_max.value()};
}

[[nodiscard]] Result<std::string> required_string(const JsonValue &root,
                                                  const std::string_view field)
{
    const auto *value = root.find(field);
    if (value == nullptr || value->string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Library query field is missing",
                          {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
    }
    return *value->string_if();
}

} // namespace

Result<std::string> serialize_library_query_document(const LibraryQuery &query)
{
    if (!query.collection_id.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "A saved library query cannot select another named set",
                          {{"reason", "invalid_library_query_collection"}});
    }
    auto valid = validate_library_query(query);
    if (!valid)
        return valid.error();
    JsonValue::Array colors;
    colors.reserve(query.color_labels.size());
    for (const auto label : query.color_labels)
        colors.emplace_back(std::string(color_label_name(label)));
    JsonValue::Array media;
    media.reserve(query.media_types.size());
    for (const auto &type : query.media_types)
        media.emplace_back(type);
    const auto optional_time = [](const std::optional<std::int64_t> &value) -> JsonValue
    {
        if (!value)
            return JsonValue{nullptr};
        return JsonValue::number(std::to_string(*value));
    };
    JsonValue document{JsonValue::Object{
        {"schema_version", JsonValue::number(std::to_string(kLibraryQueryDocumentSchemaVersion))},
        {"rating_mode", rating_mode_name(query.rating_mode)},
        {"rating_value", JsonValue::number(std::to_string(query.rating_value))},
        {"color_labels", std::move(colors)},
        {"reject_filter", reject_filter_name(query.reject_filter)},
        {"sort_field", sort_field_name(query.sort_field)},
        {"sort_direction", query.sort_direction == SortDirection::kAscending ? "asc" : "desc"},
        {"folder_uri", query.folder_uri},
        {"tag", query.tag},
        {"text", query.text},
        {"media_types", std::move(media)},
        {"edit_filter", edit_filter_name(query.edit_filter)},
        {"camera", query.camera},
        {"iso", range_to_json(query.iso)},
        {"aperture", range_to_json(query.aperture)},
        {"focal_length_mm", range_to_json(query.focal_length_mm)},
        {"shutter_s", range_to_json(query.shutter_s)},
        {"aspect_ratio", range_to_json(query.aspect_ratio)},
        {"imported_after_unix_ms", optional_time(query.imported_after_unix_ms)},
        {"imported_before_unix_ms", optional_time(query.imported_before_unix_ms)},
        {"captured_after_unix_s", optional_time(query.captured_after_unix_s)},
        {"captured_before_unix_s", optional_time(query.captured_before_unix_s)},
        {"camera_make_equals",
         query.camera_make_equals ? JsonValue{*query.camera_make_equals} : JsonValue{nullptr}},
        {"camera_model_equals",
         query.camera_model_equals ? JsonValue{*query.camera_model_equals} : JsonValue{nullptr}},
        {"lens_make_equals",
         query.lens_make_equals ? JsonValue{*query.lens_make_equals} : JsonValue{nullptr}},
        {"lens_model_equals",
         query.lens_model_equals ? JsonValue{*query.lens_model_equals} : JsonValue{nullptr}},
        {"focal_length_mm_equals",
         query.focal_length_mm_equals ?
             JsonValue::number(std::to_string(*query.focal_length_mm_equals)) :
             JsonValue{nullptr}},
        {"captured_local_date",
         query.captured_local_date ? JsonValue{*query.captured_local_date} : JsonValue{nullptr}},
        {"country_equals",
         query.country_equals ? JsonValue{*query.country_equals} : JsonValue{nullptr}},
        {"province_state_equals", query.province_state_equals ?
                                      JsonValue{*query.province_state_equals} :
                                      JsonValue{nullptr}},
        {"city_equals", query.city_equals ? JsonValue{*query.city_equals} : JsonValue{nullptr}},
        {"sublocation_equals",
         query.sublocation_equals ? JsonValue{*query.sublocation_equals} : JsonValue{nullptr}},
    }};
    return serialize_json(document);
}

Result<LibraryQuery> parse_library_query_document(const std::string_view json)
{
    auto parsed = parse_json(json);
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Library query document must be an object",
                          {{"reason", "invalid_library_query"}});
    }
    static constexpr std::array<std::string_view, 32> kKeys{"schema_version",
                                                            "rating_mode",
                                                            "rating_value",
                                                            "color_labels",
                                                            "reject_filter",
                                                            "sort_field",
                                                            "sort_direction",
                                                            "folder_uri",
                                                            "tag",
                                                            "text",
                                                            "media_types",
                                                            "edit_filter",
                                                            "camera",
                                                            "iso",
                                                            "aperture",
                                                            "focal_length_mm",
                                                            "shutter_s",
                                                            "aspect_ratio",
                                                            "imported_after_unix_ms",
                                                            "imported_before_unix_ms",
                                                            "captured_after_unix_s",
                                                            "captured_before_unix_s",
                                                            "camera_make_equals",
                                                            "camera_model_equals",
                                                            "lens_make_equals",
                                                            "lens_model_equals",
                                                            "focal_length_mm_equals",
                                                            "captured_local_date",
                                                            "country_equals",
                                                            "province_state_equals",
                                                            "city_equals",
                                                            "sublocation_equals"};
    for (const auto &[key, value] : *object)
    {
        static_cast<void>(value);
        if (key == "collection_id")
        {
            return make_error(ErrorCode::kValidation,
                              "A saved library query cannot select another named set",
                              {{"reason", "invalid_library_query_collection"}});
        }
        if (std::find(kKeys.begin(), kKeys.end(), std::string_view{key}) == kKeys.end())
        {
            return make_error(ErrorCode::kValidation, "Library query document has an unknown field",
                              {{"field", key}, {"reason", "invalid_library_query"}});
        }
    }
    const auto *schema = parsed.value().find("schema_version");
    auto schema_version = schema == nullptr ?
                              Result<std::optional<std::int64_t>>{std::optional<std::int64_t>{}} :
                              json_optional_int64(*schema, "schema_version");
    if (!schema_version)
        return schema_version.error();
    if (!schema_version.value() ||
        *schema_version.value() < kLibraryQueryDocumentSchemaVersionMin ||
        *schema_version.value() > kLibraryQueryDocumentSchemaVersion)
    {
        return make_error(ErrorCode::kValidation, "Library query schema version is unsupported",
                          {{"reason", "unsupported_library_query_schema"}});
    }
    const bool schema_v2 = *schema_version.value() >= 2;
    const bool schema_v3 = *schema_version.value() >= 3;
    const bool schema_v4 = *schema_version.value() >= 4;
    LibraryQuery query;
    auto rating_mode = required_string(parsed.value(), "rating_mode");
    if (!rating_mode)
        return rating_mode.error();
    auto parsed_mode = parse_rating_mode(rating_mode.value());
    if (!parsed_mode)
        return parsed_mode.error();
    query.rating_mode = parsed_mode.value();
    const auto *rating_value = parsed.value().find("rating_value");
    if (rating_value == nullptr)
        return make_error(ErrorCode::kValidation, "Library query field is missing",
                          {{"field", "rating_value"}, {"reason", "invalid_library_query"}});
    auto parsed_rating = json_optional_int64(*rating_value, "rating_value");
    if (!parsed_rating || !parsed_rating.value())
    {
        return parsed_rating ?
                   make_error(ErrorCode::kValidation, "Library query field is missing",
                              {{"field", "rating_value"}, {"reason", "invalid_library_query"}}) :
                   parsed_rating.error();
    }
    if (*parsed_rating.value() < 0 || *parsed_rating.value() > 5)
        return make_error(ErrorCode::kValidation, "Library rating filter is outside 0 to 5",
                          {{"field", "rating"}, {"reason", "invalid_library_rating_filter"}});
    query.rating_value = static_cast<int>(*parsed_rating.value());
    const auto *colors = parsed.value().find("color_labels");
    if (colors == nullptr || colors->array_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Library query field is missing",
                          {{"field", "color_labels"}, {"reason", "invalid_library_query"}});
    for (const auto &entry : *colors->array_if())
    {
        if (entry.string_if() == nullptr)
            return make_error(
                ErrorCode::kValidation, "Library color filters must be unique non-empty labels",
                {{"field", "color_labels"}, {"reason", "invalid_library_color_filter"}});
        auto label = parse_color_label(*entry.string_if());
        if (!label || label.value() == ColorLabel::kNone)
            return make_error(
                ErrorCode::kValidation, "Library color filters must be unique non-empty labels",
                {{"field", "color_labels"}, {"reason", "invalid_library_color_filter"}});
        query.color_labels.push_back(label.value());
    }
    auto reject = required_string(parsed.value(), "reject_filter");
    if (!reject)
        return reject.error();
    auto parsed_reject = parse_reject_filter_name(reject.value());
    if (!parsed_reject)
        return parsed_reject.error();
    query.reject_filter = parsed_reject.value();
    auto sort_field = required_string(parsed.value(), "sort_field");
    if (!sort_field)
        return sort_field.error();
    auto parsed_sort = parse_sort_field_name(sort_field.value());
    if (!parsed_sort)
        return parsed_sort.error();
    query.sort_field = parsed_sort.value();
    auto sort_direction = required_string(parsed.value(), "sort_direction");
    if (!sort_direction)
        return sort_direction.error();
    if (sort_direction.value() == "asc")
        query.sort_direction = SortDirection::kAscending;
    else if (sort_direction.value() == "desc")
        query.sort_direction = SortDirection::kDescending;
    else
        return make_error(
            ErrorCode::kValidation, "Library sort direction is invalid",
            {{"sort_direction", sort_direction.value()}, {"reason", "invalid_library_query"}});
    auto folder_uri = required_string(parsed.value(), "folder_uri");
    if (!folder_uri)
        return folder_uri.error();
    query.folder_uri = std::move(folder_uri).value();
    auto tag = required_string(parsed.value(), "tag");
    if (!tag)
        return tag.error();
    query.tag = std::move(tag).value();
    auto text = required_string(parsed.value(), "text");
    if (!text)
        return text.error();
    query.text = std::move(text).value();
    const auto *media = parsed.value().find("media_types");
    if (media == nullptr || media->array_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Library query field is missing",
                          {{"field", "media_types"}, {"reason", "invalid_library_query"}});
    for (const auto &entry : *media->array_if())
    {
        if (entry.string_if() == nullptr)
            return make_error(
                ErrorCode::kValidation, "Library media filters must be unique and non-empty",
                {{"field", "media_types"}, {"reason", "invalid_library_media_filter"}});
        query.media_types.push_back(*entry.string_if());
    }
    auto edit = required_string(parsed.value(), "edit_filter");
    if (!edit)
        return edit.error();
    auto parsed_edit = parse_edit_filter_name(edit.value());
    if (!parsed_edit)
        return parsed_edit.error();
    query.edit_filter = parsed_edit.value();
    auto camera = required_string(parsed.value(), "camera");
    if (!camera)
        return camera.error();
    query.camera = std::move(camera).value();
    const auto parse_range_field = [&](const std::string_view field) -> Result<LibraryNumericRange>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr)
            return make_error(ErrorCode::kValidation, "Library query field is missing",
                              {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
        return json_range(*value, field);
    };
    auto iso = parse_range_field("iso");
    if (!iso)
        return iso.error();
    query.iso = iso.value();
    auto aperture = parse_range_field("aperture");
    if (!aperture)
        return aperture.error();
    query.aperture = aperture.value();
    auto focal = parse_range_field("focal_length_mm");
    if (!focal)
        return focal.error();
    query.focal_length_mm = focal.value();
    auto shutter = parse_range_field("shutter_s");
    if (!shutter)
        return shutter.error();
    query.shutter_s = shutter.value();
    auto aspect = parse_range_field("aspect_ratio");
    if (!aspect)
        return aspect.error();
    query.aspect_ratio = aspect.value();
    const auto parse_time = [&](const std::string_view field) -> Result<std::optional<std::int64_t>>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr)
            return make_error(ErrorCode::kValidation, "Library query field is missing",
                              {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
        return json_optional_int64(*value, field);
    };
    auto imported_after = parse_time("imported_after_unix_ms");
    if (!imported_after)
        return imported_after.error();
    query.imported_after_unix_ms = imported_after.value();
    auto imported_before = parse_time("imported_before_unix_ms");
    if (!imported_before)
        return imported_before.error();
    query.imported_before_unix_ms = imported_before.value();
    auto captured_after = parse_time("captured_after_unix_s");
    if (!captured_after)
        return captured_after.error();
    query.captured_after_unix_s = captured_after.value();
    auto captured_before = parse_time("captured_before_unix_s");
    if (!captured_before)
        return captured_before.error();
    query.captured_before_unix_s = captured_before.value();
    const auto parse_optional_string =
        [&](const std::string_view field,
            const bool required_when_missing) -> Result<std::optional<std::string>>
    {
        const auto *value = parsed.value().find(field);
        if (value == nullptr)
        {
            if (!required_when_missing)
                return std::optional<std::string>{};
            return make_error(ErrorCode::kValidation, "Library query field is missing",
                              {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
        }
        if (value->is_null())
            return std::optional<std::string>{};
        if (value->string_if() == nullptr)
            return make_error(ErrorCode::kValidation,
                              "Library query field must be a string or null",
                              {{"field", std::string(field)}, {"reason", "invalid_library_query"}});
        return std::optional<std::string>{*value->string_if()};
    };
    auto camera_make_equals = parse_optional_string("camera_make_equals", schema_v2);
    if (!camera_make_equals)
        return camera_make_equals.error();
    query.camera_make_equals = camera_make_equals.value();
    auto camera_model_equals = parse_optional_string("camera_model_equals", schema_v2);
    if (!camera_model_equals)
        return camera_model_equals.error();
    query.camera_model_equals = camera_model_equals.value();
    auto lens_make_equals = parse_optional_string("lens_make_equals", schema_v4);
    if (!lens_make_equals)
        return lens_make_equals.error();
    query.lens_make_equals = lens_make_equals.value();
    auto lens_model_equals = parse_optional_string("lens_model_equals", schema_v4);
    if (!lens_model_equals)
        return lens_model_equals.error();
    query.lens_model_equals = lens_model_equals.value();
    const auto *focal_equals = parsed.value().find("focal_length_mm_equals");
    if (focal_equals == nullptr)
    {
        if (schema_v2)
            return make_error(
                ErrorCode::kValidation, "Library query field is missing",
                {{"field", "focal_length_mm_equals"}, {"reason", "invalid_library_query"}});
    }
    else if (!focal_equals->is_null())
    {
        auto number = json_optional_double(*focal_equals, "focal_length_mm_equals");
        if (!number)
            return number.error();
        query.focal_length_mm_equals = number.value();
    }
    auto captured_local_date = parse_optional_string("captured_local_date", schema_v2);
    if (!captured_local_date)
        return captured_local_date.error();
    query.captured_local_date = captured_local_date.value();
    auto country_equals = parse_optional_string("country_equals", schema_v3);
    if (!country_equals)
        return country_equals.error();
    query.country_equals = country_equals.value();
    auto province_state_equals = parse_optional_string("province_state_equals", schema_v3);
    if (!province_state_equals)
        return province_state_equals.error();
    query.province_state_equals = province_state_equals.value();
    auto city_equals = parse_optional_string("city_equals", schema_v3);
    if (!city_equals)
        return city_equals.error();
    query.city_equals = city_equals.value();
    auto sublocation_equals = parse_optional_string("sublocation_equals", schema_v3);
    if (!sublocation_equals)
        return sublocation_equals.error();
    query.sublocation_equals = sublocation_equals.value();
    auto valid = validate_library_query(query);
    if (!valid)
        return valid.error();
    return query;
}

Result<void> validate_library_set_record(const LibrarySetRecord &set)
{
    if (set.id.size() != std::string_view{"set_"}.size() + 32U || !set.id.starts_with("set_"))
        return make_error(ErrorCode::kValidation, "Library set id is invalid",
                          {{"reason", "invalid_library_set_id"}});
    auto name = normalize_library_set_name(set.name);
    if (!name || name.value() != set.name)
        return make_error(ErrorCode::kValidation, "Library set name is not canonical",
                          {{"reason", "invalid_library_set_name"}});
    if (set.kind == LibrarySetKind::kManual)
    {
        if (set.query)
            return make_error(ErrorCode::kValidation, "A manual library set cannot store a query",
                              {{"reason", "invalid_library_set_query"}});
        return {};
    }
    if (!set.query)
        return make_error(ErrorCode::kValidation, "A smart library set requires a query",
                          {{"reason", "invalid_library_set_query"}});
    if (!set.query->collection_id.empty())
        return make_error(ErrorCode::kValidation,
                          "A saved library query cannot select another named set",
                          {{"reason", "invalid_library_query_collection"}});
    return validate_library_query(*set.query);
}

} // namespace ravo
