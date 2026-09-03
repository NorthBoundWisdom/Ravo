#include "application_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace ravo::cli_internal
{

Result<LibraryQuery> build_library_query(const CatalogCliArguments &flags)
{
    LibraryQuery query;
    if (!flags.query_json.empty())
    {
        auto parsed = parse_library_query_document(flags.query_json);
        if (!parsed)
            return parsed.error();
        query = parsed.value();
    }
    if (!flags.tag.empty())
    {
        auto tag = normalize_tag_name(flags.tag);
        if (!tag)
            return tag.error();
        query.tag = tag.value();
    }
    if (!flags.set_id.empty())
        query.collection_id = std::string(flags.set_id);
    if (!flags.camera.empty())
        query.camera = std::string(flags.camera);
    if (!flags.camera_make.empty() || !flags.camera_model.empty())
    {
        query.camera_make_equals = std::string(flags.camera_make);
        query.camera_model_equals = std::string(flags.camera_model);
    }
    if (!flags.focal_length_mm.empty())
    {
        auto parsed = parse_double_flag(flags.focal_length_mm, "--focal-length-mm");
        if (!parsed)
            return parsed.error();
        query.focal_length_mm_equals = parsed.value();
    }
    if (!flags.captured_local_date.empty())
        query.captured_local_date = std::string(flags.captured_local_date);
    if (!flags.country.empty())
        query.country_equals = std::string(flags.country);
    if (!flags.province_state.empty())
        query.province_state_equals = std::string(flags.province_state);
    if (!flags.city.empty())
        query.city_equals = std::string(flags.city);
    if (!flags.sublocation.empty())
        query.sublocation_equals = std::string(flags.sublocation);
    if (!flags.captured_after_unix_s.empty())
    {
        auto parsed = parse_int_flag(flags.captured_after_unix_s, "--captured-after");
        if (!parsed)
            return parsed.error();
        query.captured_after_unix_s = parsed.value();
    }
    if (!flags.captured_before_unix_s.empty())
    {
        auto parsed = parse_int_flag(flags.captured_before_unix_s, "--captured-before");
        if (!parsed)
            return parsed.error();
        query.captured_before_unix_s = parsed.value();
    }
    return query;
}

namespace
{

[[nodiscard]] JsonValue facet_entry_json(const LibraryFacetEntry &entry)
{
    JsonValue::Object row{{"key", entry.key},
                          {"label", entry.label},
                          {"count", JsonValue::number(std::to_string(entry.count))}};
    if (entry.camera_make)
        row.emplace("camera_make", *entry.camera_make);
    if (entry.camera_model)
        row.emplace("camera_model", *entry.camera_model);
    if (entry.focal_length_mm)
        row.emplace("focal_length_mm", JsonValue::number(std::to_string(*entry.focal_length_mm)));
    if (entry.captured_local_date)
        row.emplace("captured_local_date", *entry.captured_local_date);
    return JsonValue{std::move(row)};
}

[[nodiscard]] JsonValue facet_entries_json(const std::vector<LibraryFacetEntry> &entries)
{
    JsonValue::Array rows;
    rows.reserve(entries.size());
    for (const auto &entry : entries)
        rows.push_back(facet_entry_json(entry));
    return JsonValue{std::move(rows)};
}

// Echo back only the filters that actually narrowed the counts so callers can
// tell a scoped listing from a whole-catalog one without re-parsing flags.
[[nodiscard]] JsonValue scope_json(const LibraryQuery &scope)
{
    JsonValue::Object row;
    if (!scope.tag.empty())
        row.emplace("tag", scope.tag);
    if (!scope.collection_id.empty())
        row.emplace("set_id", scope.collection_id);
    if (!scope.camera.empty())
        row.emplace("camera", scope.camera);
    if (scope.camera_make_equals)
        row.emplace("camera_make", *scope.camera_make_equals);
    if (scope.camera_model_equals)
        row.emplace("camera_model", *scope.camera_model_equals);
    if (scope.focal_length_mm_equals)
        row.emplace("focal_length_mm",
                    JsonValue::number(std::to_string(*scope.focal_length_mm_equals)));
    if (scope.captured_local_date)
        row.emplace("captured_local_date", *scope.captured_local_date);
    if (scope.captured_after_unix_s)
        row.emplace("captured_after",
                    JsonValue::number(std::to_string(*scope.captured_after_unix_s)));
    if (scope.captured_before_unix_s)
        row.emplace("captured_before",
                    JsonValue::number(std::to_string(*scope.captured_before_unix_s)));
    if (scope.country_equals)
        row.emplace("country", *scope.country_equals);
    if (scope.province_state_equals)
        row.emplace("province_state", *scope.province_state_equals);
    if (scope.city_equals)
        row.emplace("city", *scope.city_equals);
    if (scope.sublocation_equals)
        row.emplace("sublocation", *scope.sublocation_equals);
    return JsonValue{std::move(row)};
}

} // namespace

Result<JsonValue> run_catalog_facets_command(CatalogService &service,
                                             const CatalogCliArguments &flags)
{
    auto scope = build_library_query(flags);
    if (!scope)
        return scope.error();
    auto captures = service.list_capture_facets(scope.value());
    if (!captures)
        return captures.error();
    auto locations = service.list_location_facets(scope.value());
    if (!locations)
        return locations.error();
    const bool scoped = captures.value().scoped || locations.value().scoped;
    return JsonValue{JsonValue::Object{
        {"cameras", facet_entries_json(captures.value().cameras)},
        {"lenses", facet_entries_json(captures.value().lenses)},
        {"capture_dates", facet_entries_json(captures.value().capture_dates)},
        {"countries", facet_entries_json(locations.value().countries)},
        {"province_states", facet_entries_json(locations.value().province_states)},
        {"cities", facet_entries_json(locations.value().cities)},
        {"sublocations", facet_entries_json(locations.value().sublocations)},
        {"scoped", scoped},
        {"scope", scope_json(scope.value())},
        {"truncated", captures.value().truncated || locations.value().truncated},
    }};
}

} // namespace ravo::cli_internal
