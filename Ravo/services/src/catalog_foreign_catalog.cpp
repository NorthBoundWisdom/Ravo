#include "ravo/services/conversion_service.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/recovery_store.h"
#include "ravo/engine/engine.h"
#include "ravo/services/develop_service.h"
#include "ravo/services/import_service.h"
#include "ravo/services/library_service.h"
#include "ravo/services/metadata_service.h"

#include "catalog_internal.h"
#include "catalog_service_internal.h"

#include <cctype>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/adapters/lightroom_catalog.h"
#include "ravo/adapters/lightroom_develop.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/parse_number.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/foreign_catalog.h"

namespace ravo
{

ConversionService::ConversionService(const std::unique_ptr<CatalogRepository> &repository,
                                     DevelopService &develop_service, ImportService &import_service,
                                     LibraryService &library_service,
                                     MetadataService &metadata_service) noexcept
    : repository_(repository)
    , develop_service_(develop_service)
    , import_service_(import_service)
    , library_service_(library_service)
    , metadata_service_(metadata_service)
{
}

namespace
{

using ForeignCatalogFixtureItem = ForeignCatalogPhoto;

struct ForeignCatalogFixture
{
    bool native_lightroom = false;
    ForeignCatalogSourceKind source_kind = ForeignCatalogSourceKind::kLightroomClassic;
    std::optional<std::string> source_product_version;
    std::string source_path;
    std::string source_root;
    std::vector<ForeignCatalogFixtureItem> items;
    std::vector<ForeignCatalogCollection> collections;
    std::string source_sha256;
    std::vector<std::string> archived_only_tables;
};

[[nodiscard]] std::string path_text(const std::filesystem::path &path)
{
    return catalog_service_internal::utf8_string(path.generic_u8string());
}

[[nodiscard]] bool has_extension_lower(const std::filesystem::path &path,
                                       const std::string_view expected) noexcept
{
    return extension_lower(path) == expected;
}

[[nodiscard]] bool looks_like_sqlite(const std::string_view path)
{
    auto bytes = read_utf8_text_file(path, 16U);
    if (!bytes)
        return false;
    return bytes.value().rfind("SQLite format 3", 0) == 0U;
}

[[nodiscard]] Result<std::string> required_json_string(const JsonValue &object,
                                                       const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr || value->string_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field is missing",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_missing"}});
    }
    return *value->string_if();
}

[[nodiscard]] Result<std::int64_t> required_json_int(const JsonValue &object,
                                                     const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr || value->number_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field is missing",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_missing"}});
    }
    try
    {
        return static_cast<std::int64_t>(std::stoll(value->number_if()->text));
    }
    catch (const std::exception &)
    {
        return make_error(ErrorCode::kValidation, "Foreign catalog fixture number is invalid",
                          {{"field", std::string(key)},
                           {"value", value->number_if()->text},
                           {"reason", "foreign_catalog_fixture_number_invalid"}});
    }
}

[[nodiscard]] Result<std::optional<std::string>> optional_json_string(const JsonValue &object,
                                                                      const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr)
        return std::optional<std::string>{};
    if (value->is_null())
        return std::optional<std::string>{};
    if (value->string_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field must be a string",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_type"}});
    }
    return std::optional<std::string>{*value->string_if()};
}

[[nodiscard]] Result<std::optional<int>> optional_json_int(const JsonValue &object,
                                                           const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr || value->is_null())
        return std::optional<int>{};
    if (value->number_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field must be a number",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_type"}});
    }
    try
    {
        return std::optional<int>{std::stoi(value->number_if()->text)};
    }
    catch (const std::exception &)
    {
        return make_error(ErrorCode::kValidation, "Foreign catalog fixture number is invalid",
                          {{"field", std::string(key)},
                           {"value", value->number_if()->text},
                           {"reason", "foreign_catalog_fixture_number_invalid"}});
    }
}

[[nodiscard]] Result<std::optional<bool>> optional_json_bool(const JsonValue &object,
                                                             const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr || value->is_null())
        return std::optional<bool>{};
    if (value->boolean_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field must be a boolean",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_type"}});
    }
    return std::optional<bool>{*value->boolean_if()};
}

[[nodiscard]] Result<std::vector<std::string>>
optional_json_string_array(const JsonValue &object, const std::string_view key)
{
    const auto *value = object.find(key);
    if (value == nullptr || value->is_null())
        return std::vector<std::string>{};
    const auto *array = value->array_if();
    if (array == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture field must be an array",
            {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_type"}});
    }
    std::vector<std::string> items;
    items.reserve(array->size());
    for (const auto &entry : *array)
    {
        if (entry.string_if() == nullptr)
        {
            return make_error(
                ErrorCode::kValidation, "Foreign catalog fixture array entry must be a string",
                {{"field", std::string(key)}, {"reason", "foreign_catalog_fixture_field_type"}});
        }
        items.push_back(*entry.string_if());
    }
    return items;
}

[[nodiscard]] Result<std::string> resolve_fixture_path(const std::string_view source_root,
                                                       const std::string_view relative)
{
    if (relative.empty())
    {
        return make_error(ErrorCode::kValidation, "Foreign catalog item path is empty",
                          {{"reason", "foreign_catalog_item_path_empty"}});
    }
    // Fixture item paths are portable and relative to the source document's own
    // directory. Only already-absolute paths (or file:// URIs) bypass the join,
    // so conversion never resolves a source original against the process CWD.
    const auto as_written = utf8_path(relative);
    if (as_written.is_absolute() || relative.rfind("file://", 0) == 0U)
    {
        auto normalized = normalize_local_input(relative);
        if (!normalized)
            return normalized.error();
        return normalized.value().path;
    }
    const auto joined = utf8_path(source_root) / as_written;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(joined, error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to resolve foreign catalog path",
                          {{"path", path_text(joined)},
                           {"reason", "foreign_catalog_path_resolve_failed"},
                           {"detail", error.message()}});
    }
    auto normalized = normalize_local_input(path_text(canonical));
    if (!normalized)
        return normalized.error();
    return normalized.value().path;
}

[[nodiscard]] Result<ForeignCatalogFileFingerprint>
fingerprint_original(const std::string_view path, const CancellationToken &cancellation)
{
    auto before = read_file_identity(path);
    if (!before)
        return before.error();
    auto digest = sha256_file_hex(path, cancellation);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
    if (before.value().size_bytes != identity.value().size_bytes ||
        before.value().mtime_unix_ms != identity.value().mtime_unix_ms)
        return make_error(
            ErrorCode::kConflict, "Source changed during fingerprint",
            {{"reason", "source_changed_during_fingerprint"}, {"path", std::string(path)}});
    ForeignCatalogFileFingerprint fingerprint;
    fingerprint.path = std::string(path);
    fingerprint.sha256 = std::move(digest).value();
    fingerprint.size_bytes = identity.value().size_bytes;
    fingerprint.mtime_unix_ms = identity.value().mtime_unix_ms;
    return fingerprint;
}

[[nodiscard]] bool fingerprints_equal(const ForeignCatalogFileFingerprint &left,
                                      const ForeignCatalogFileFingerprint &right) noexcept
{
    return left.path == right.path && left.sha256 == right.sha256 &&
           left.size_bytes == right.size_bytes && left.mtime_unix_ms == right.mtime_unix_ms;
}

[[nodiscard]] Result<ForeignCatalogFixtureItem> parse_fixture_item(const JsonValue &object)
{
    if (object.object_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Foreign catalog item must be an object",
                          {{"reason", "foreign_catalog_fixture_item_type"}});
    }
    ForeignCatalogFixtureItem item;
    auto foreign_id = required_json_string(object, "foreign_id");
    if (!foreign_id)
        return foreign_id.error();
    item.foreign_id = std::move(foreign_id).value();
    auto original = required_json_string(object, "original_path");
    if (!original)
        return original.error();
    item.original_path = std::move(original).value();

    auto rating = optional_json_int(object, "rating");
    if (!rating)
        return rating.error();
    item.rating = rating.value();
    if (item.rating && (*item.rating < 0 || *item.rating > 5))
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog rating is out of range",
            {{"foreign_id", item.foreign_id}, {"reason", "foreign_catalog_rating_invalid"}});
    }

    auto color = optional_json_string(object, "color_label");
    if (!color)
        return color.error();
    if (color.value())
    {
        auto parsed = parse_color_label(*color.value());
        if (!parsed)
            return parsed.error();
        item.color_label = parsed.value();
    }

    auto rejected = optional_json_bool(object, "rejected");
    if (!rejected)
        return rejected.error();
    item.rejected = rejected.value();

    auto title = optional_json_string(object, "title");
    if (!title)
        return title.error();
    item.metadata.title = title.value();
    auto description = optional_json_string(object, "description");
    if (!description)
        return description.error();
    item.metadata.description = description.value();
    auto creator = optional_json_string(object, "creator");
    if (!creator)
        return creator.error();
    item.metadata.creator = creator.value();
    auto copyright = optional_json_string(object, "copyright");
    if (!copyright)
        return copyright.error();
    item.metadata.copyright = copyright.value();
    auto country = optional_json_string(object, "country");
    if (!country)
        return country.error();
    item.metadata.country = country.value();
    auto province = optional_json_string(object, "province_state");
    if (!province)
        return province.error();
    item.metadata.province_state = province.value();
    auto city = optional_json_string(object, "city");
    if (!city)
        return city.error();
    item.metadata.city = city.value();
    auto sublocation = optional_json_string(object, "sublocation");
    if (!sublocation)
        return sublocation.error();
    item.metadata.sublocation = sublocation.value();

    auto keywords = optional_json_string_array(object, "keywords");
    if (!keywords)
        return keywords.error();
    item.keywords = std::move(keywords).value();
    auto crs = optional_json_string(object, "crs_xmp_path");
    if (!crs)
        return crs.error();
    item.crs_xmp_path = crs.value();
    auto unsupported = optional_json_string_array(object, "unsupported_adjusts");
    if (!unsupported)
        return unsupported.error();
    item.unsupported_adjusts = std::move(unsupported).value();
    return item;
}

[[nodiscard]] Result<ForeignCatalogFixture>
load_foreign_catalog_fixture(const std::string_view source_path,
                             const std::optional<ForeignCatalogSourceKind> expected_kind,
                             const CancellationToken &cancellation)
{
    auto location = normalize_local_input(source_path);
    if (!location)
        return location.error();
    const auto path = utf8_path(location.value().path);
    std::error_code error;
    if (std::filesystem::is_directory(path, error) && !error)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Capture One session directories are not a packaged conversion source",
                          {{"path", location.value().path},
                           {"reason", "unsupported_source_schema"},
                           {"detail", "capture_one_session_reader_not_packaged"}});
    }
    if (error && error != std::errc::no_such_file_or_directory)
    {
        return make_error(ErrorCode::kIo, "Unable to inspect foreign catalog source",
                          {{"path", location.value().path},
                           {"reason", "foreign_catalog_source_inspect_failed"},
                           {"detail", error.message()}});
    }
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return make_error(
            ErrorCode::kNotFound, "Foreign catalog source was not found",
            {{"path", location.value().path}, {"reason", "foreign_catalog_source_missing"}});
    }
    if (has_extension_lower(path, ".lrcat") || looks_like_sqlite(location.value().path))
    {
        if (expected_kind && *expected_kind != ForeignCatalogSourceKind::kLightroomClassic)
            return make_error(ErrorCode::kInvalidArgument, "Source kind does not match Lightroom",
                              {{"reason", "foreign_catalog_source_kind_mismatch"}});
        auto photos = read_lightroom_catalog(location.value().path, cancellation);
        if (!photos)
            return photos.error();
        ForeignCatalogFixture catalog;
        catalog.native_lightroom = true;
        catalog.source_path = location.value().path;
        catalog.source_root = path_text(path.parent_path());
        catalog.collections = std::move(photos.value().collections);
        catalog.source_sha256 = photos.value().source_sha256;
        catalog.archived_only_tables = std::move(photos.value().archived_only_tables);
        catalog.items = std::move(photos.value().photos);
        std::stable_sort(catalog.items.begin(), catalog.items.end(),
                         [](const auto &a, const auto &b)
                         { return a.master_id.has_value() < b.master_id.has_value(); });
        return catalog;
    }

    auto text = read_utf8_text_file(location.value().path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
    {
        auto failure = parsed.error();
        failure.context.insert_or_assign("path", location.value().path);
        failure.context.insert_or_assign("reason", "unsupported_source_schema");
        return failure;
    }
    if (parsed.value().object_if() == nullptr)
    {
        return make_error(
            ErrorCode::kUnsupported, "Foreign catalog source is not a fixture object",
            {{"path", location.value().path}, {"reason", "unsupported_source_schema"}});
    }
    auto schema = required_json_string(parsed.value(), "schema");
    if (!schema)
        return schema.error();
    if (schema.value() != kForeignCatalogFixtureContractVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Foreign catalog fixture schema is unsupported",
                          {{"path", location.value().path},
                           {"schema", schema.value()},
                           {"reason", "unsupported_source_schema"}});
    }
    auto version = required_json_int(parsed.value(), "schema_version");
    if (!version)
        return version.error();
    if (version.value() != kForeignCatalogFixtureSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Foreign catalog fixture schema version is unsupported",
                          {{"path", location.value().path},
                           {"schema_version", std::to_string(version.value())},
                           {"reason", "unsupported_source_version"}});
    }
    auto kind_text = required_json_string(parsed.value(), "source_kind");
    if (!kind_text)
        return kind_text.error();
    auto kind = parse_foreign_catalog_source_kind(kind_text.value());
    if (!kind)
        return kind.error();
    if (expected_kind && *expected_kind != kind.value())
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog source kind does not match --source-kind",
            {{"path", location.value().path},
             {"source_kind", std::string(foreign_catalog_source_kind_name(kind.value()))},
             {"requested_kind", std::string(foreign_catalog_source_kind_name(*expected_kind))},
             {"reason", "foreign_catalog_source_kind_mismatch"}});
    }

    ForeignCatalogFixture fixture;
    fixture.source_kind = kind.value();
    fixture.source_path = location.value().path;
    fixture.source_root = path_text(path.parent_path());
    auto product = optional_json_string(parsed.value(), "source_product_version");
    if (!product)
        return product.error();
    fixture.source_product_version = product.value();
    fixture.source_sha256 = sha256_utf8_hex(text.value());

    const auto *items = parsed.value().find("items");
    if (items == nullptr || items->array_if() == nullptr)
    {
        return make_error(
            ErrorCode::kValidation, "Foreign catalog fixture items must be an array",
            {{"path", location.value().path}, {"reason", "foreign_catalog_fixture_items_missing"}});
    }
    for (const auto &entry : *items->array_if())
    {
        auto item = parse_fixture_item(entry);
        if (!item)
            return item.error();
        fixture.items.push_back(std::move(item).value());
    }
    return fixture;
}

void count_item(ForeignCatalogConversionReport &report, const ForeignCatalogItemReport &item)
{
    switch (item.status)
    {
    case ForeignCatalogItemStatus::kImported:
        ++report.imported;
        break;
    case ForeignCatalogItemStatus::kSkipped:
        ++report.skipped;
        break;
    case ForeignCatalogItemStatus::kUnsupported:
        ++report.unsupported;
        break;
    case ForeignCatalogItemStatus::kFailed:
        ++report.failed;
        break;
    }
    report.unsupported_fields += item.unsupported_fields.size();
}

void add_mapped(ForeignCatalogItemReport &item, const std::string_view field)
{
    item.mapped_fields.emplace_back(field);
}

[[nodiscard]] bool has_writable_metadata(const WritableMetadata &metadata) noexcept
{
    return metadata.title || metadata.description || metadata.creator || metadata.copyright ||
           metadata.country || metadata.province_state || metadata.city || metadata.sublocation ||
           metadata.headline || metadata.credit || metadata.source || metadata.instructions ||
           metadata.usage_terms || metadata.job_id;
}

} // namespace

Result<ForeignCatalogSourceKind> parse_foreign_catalog_source_kind(const std::string_view text)
{
    if (text == "lightroom-classic" || text == "lightroom")
        return ForeignCatalogSourceKind::kLightroomClassic;
    if (text == "capture-one" || text == "capture_one")
        return ForeignCatalogSourceKind::kCaptureOne;
    return make_error(ErrorCode::kUnsupported, "Foreign catalog source kind is unsupported",
                      {{"source_kind", std::string(text)}, {"reason", "unsupported_source_kind"}});
}

Result<ForeignCatalogConversionReport>
ConversionService::convert_foreign_catalog(const ForeignCatalogConversionRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (request.source_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Foreign catalog conversion requires a source",
                          {{"reason", "foreign_catalog_source_missing"}});
    }
    if (request.mode == ImportTransferMode::kMove)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Foreign catalog conversion must not Move originals",
                          {{"reason", "foreign_catalog_move_rejected"}});
    }
    if (request.mode != ImportTransferMode::kAdd)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Foreign catalog conversion Copy is not in this fixture tranche",
                          {{"reason", "foreign_catalog_copy_not_in_tranche"}});
    }

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto fixture = load_foreign_catalog_fixture(request.source_path, request.source_kind,
                                                request.cancellation);
    if (!fixture)
        return fixture.error();
    if (!request.foreign_ids.empty() && request.expected_source_sha256.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Explicit foreign IDs require the observed source hash",
                          {{"reason", "foreign_selection_requires_source_hash"}});
    if (!request.expected_source_sha256.empty())
    {
        if (request.expected_source_sha256.size() != 64 ||
            !std::all_of(request.expected_source_sha256.begin(),
                         request.expected_source_sha256.end(), [](const char c)
                         { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return make_error(ErrorCode::kInvalidArgument,
                              "Expected source hash must be canonical SHA-256");
        if (request.expected_source_sha256 != fixture.value().source_sha256)
            return make_error(ErrorCode::kConflict, "Foreign source changed since inspection",
                              {{"reason", "foreign_source_revision_conflict"}});
    }

    const auto source_photo_count = fixture.value().items.size();
    std::set<std::string> selected_ids;
    if (request.foreign_ids.size() > 4096)
        return make_error(ErrorCode::kInvalidArgument, "Foreign selection exceeds 4096 IDs");
    for (const auto &id : request.foreign_ids)
        if (id.empty() || !selected_ids.insert(id).second)
            return make_error(ErrorCode::kInvalidArgument, "Invalid or duplicate foreign photo ID");
    if (!selected_ids.empty())
    {
        std::set<std::string> found;
        for (const auto &photo : fixture.value().items)
        {
            if (!selected_ids.contains(photo.foreign_id))
                continue;
            found.insert(photo.foreign_id);
            if (photo.master_id && !selected_ids.contains(*photo.master_id))
                return make_error(ErrorCode::kInvalidArgument,
                                  "Select the master together with its virtual copy",
                                  {{"reason", "foreign_selection_requires_master"},
                                   {"master_id", *photo.master_id}});
        }
        if (found != selected_ids)
            return make_error(ErrorCode::kNotFound, "Foreign selection contains unknown photo IDs",
                              {{"reason", "foreign_photo_not_found"}});
        auto &items = fixture.value().items;
        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &photo)
                                   { return !selected_ids.contains(photo.foreign_id); }),
                    items.end());
    }

    std::map<std::string, std::string> mappings;
    for (const auto &[foreign, local] : request.path_mappings)
    {
        std::string prefix = foreign;
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        while (prefix.size() > 1 && prefix.back() == '/')
            prefix.pop_back();
        auto destination = normalize_local_input(local);
        if (!destination)
            return destination.error();
        std::error_code error;
        if (prefix.empty() ||
            !std::filesystem::is_directory(utf8_path(destination.value().path), error) || error)
            return make_error(ErrorCode::kInvalidArgument,
                              "Foreign path mapping requires a local directory",
                              {{"reason", "invalid_foreign_path_mapping"}});
        if (!mappings.emplace(prefix, destination.value().path).second)
            return make_error(ErrorCode::kConflict, "Duplicate foreign path mapping",
                              {{"reason", "ambiguous_foreign_path_mapping"}});
    }
    for (auto &item : fixture.value().items)
    {
        std::string path = item.original_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::pair<const std::string, std::string> *best = nullptr;
        for (const auto &mapping : mappings)
            if (path.starts_with(mapping.first) &&
                (path.size() == mapping.first.size() || mapping.first == "/" ||
                 path[mapping.first.size()] == '/') &&
                (!best || mapping.first.size() > best->first.size()))
                best = &mapping;
        if (!best)
            continue;
        auto suffix = path.substr(best->first.size());
        while (!suffix.empty() && suffix.front() == '/')
            suffix.erase(0, 1);
        for (const auto &segment : utf8_path(suffix))
            if (segment == "..")
                return make_error(ErrorCode::kValidation, "Foreign mapping escapes its root",
                                  {{"reason", "invalid_foreign_path_mapping"}});
        item.original_path = path_text(utf8_path(best->second) / utf8_path(suffix));
        if (item.skip_reason == "foreign_volume_unavailable")
            item.skip_reason.reset();
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto existing = library_service_.list_assets();
    if (!existing)
        return existing.error();
    if (request.resume &&
        (!fixture.value().native_lightroom || request.expected_source_sha256.empty()))
        return make_error(ErrorCode::kInvalidArgument,
                          "Resume requires native Lightroom and the observed source hash");
    if (!existing.value().empty() && !request.resume)
    {
        return make_error(ErrorCode::kConflict,
                          "Foreign catalog conversion requires an empty destination catalog",
                          {{"catalog", snapshot.value().database_path},
                           {"asset_count", std::to_string(existing.value().size())},
                           {"reason", "destination_catalog_not_empty"}});
    }

    auto dest_location = normalize_local_input(snapshot.value().database_path);
    if (!dest_location)
        return dest_location.error();
    if (dest_location.value().path == fixture.value().source_path)
    {
        return make_error(ErrorCode::kConflict,
                          "Foreign catalog conversion cannot use the source as the live catalog",
                          {{"path", fixture.value().source_path},
                           {"reason", "foreign_catalog_in_place_rejected"}});
    }

    ForeignCatalogConversionReport report;
    if (fixture.value().native_lightroom)
        report.schema = "ravo.lightroom-catalog-conversion/v1";
    report.source_kind = fixture.value().source_kind;
    report.source_product_version = fixture.value().source_product_version;
    report.source_path = fixture.value().source_path;
    report.destination_catalog = snapshot.value().database_path;
    report.source_photo_count = source_photo_count;
    report.selected_photo_count = fixture.value().items.size();
    report.archived_only_tables = fixture.value().archived_only_tables;
    JsonValue::Array selection, mapping;
    for (const auto &item : fixture.value().items)
        selection.emplace_back(item.foreign_id);
    for (const auto &[foreign, local] : mappings)
        mapping.emplace_back(JsonValue::Array{foreign, local});
    if (fixture.value().native_lightroom)
        report.conversion_id = sha256_utf8_hex(serialize_json(JsonValue::Object{
            {"schema", "ravo.foreign-conversion/v1"},
            {"source", fixture.value().source_sha256},
            {"selection", std::move(selection)},
            {"mappings", std::move(mapping)},
            {"preview", JsonValue::number(std::to_string(static_cast<int>(request.preview)))},
            {"defer_previews", request.defer_previews}}));
    std::map<std::string, ForeignConversionCheckpoint> checkpoints;
    std::int64_t confirmed_revision = snapshot.value().revision;
    bool journal_failed = false;
    if (request.resume)
    {
        auto journal = repository_->load_foreign_conversion(report.conversion_id);
        if (!journal)
            return journal.error();
        if (!journal.value())
            return make_error(ErrorCode::kNotFound, "No matching conversion checkpoint",
                              {{"reason", "foreign_conversion_not_found"}});
        if (journal.value()->source_sha256 != fixture.value().source_sha256 ||
            journal.value()->catalog_revision != snapshot.value().revision)
            return make_error(ErrorCode::kConflict, "Catalog changed since conversion checkpoint",
                              {{"reason", "foreign_conversion_revision_conflict"}});
        std::map<std::string, std::string> committed_targets;
        for (const auto &proof : journal.value()->commits)
            if (proof.target_id)
            {
                auto [position, inserted] =
                    committed_targets.emplace(proof.foreign_id, *proof.target_id);
                if (!inserted && position->second != *proof.target_id)
                    return make_error(ErrorCode::kValidation,
                                      "Conflicting conversion target proofs");
            }
        for (auto &record : journal.value()->records)
        {
            // Commit proofs survive an exit between business commit and receipt
            // publication. They identify the affected item without replaying it.
            if (const auto proof = committed_targets.find(record.foreign_id);
                proof != committed_targets.end())
            {
                if (record.asset_id && record.asset_id != proof->second)
                    return make_error(ErrorCode::kValidation, "Conversion target proof mismatch");
                record.asset_id = proof->second;
            }
            if (record.foreign_id.starts_with("collection/"))
            {
                auto receipt = foreign_catalog_collection_from_json(record.receipt_json);
                if (!receipt || "collection/" + receipt.value().foreign_id != record.foreign_id ||
                    (receipt.value().set_id && receipt.value().set_id != record.asset_id))
                    return make_error(ErrorCode::kValidation, "Corrupt collection checkpoint");
                receipt.value().set_id = record.asset_id;
                record.receipt_json =
                    serialize_json(foreign_catalog_collection_to_json(receipt.value()));
                checkpoints.emplace(record.foreign_id, std::move(record));
                continue;
            }
            auto receipt = foreign_catalog_item_from_json(record.receipt_json);
            if (!receipt || receipt.value().foreign_id != record.foreign_id ||
                (receipt.value().asset_id && receipt.value().asset_id != record.asset_id))
                return make_error(ErrorCode::kValidation, "Corrupt conversion checkpoint");
            receipt.value().asset_id = record.asset_id;
            record.receipt_json = serialize_json(foreign_catalog_item_to_json(receipt.value()));
            checkpoints.emplace(record.foreign_id, std::move(record));
        }
    }
    const auto checkpoint = [&](const ForeignCatalogItemReport &row,
                                const bool complete) -> Result<void>
    {
        if (report.conversion_id.empty())
            return {};
        ForeignConversionCheckpoint record{row.foreign_id, row.phase, row.asset_id, complete,
                                           serialize_json(foreign_catalog_item_to_json(row))};
        auto saved = repository_->save_foreign_conversion_checkpoint(report.conversion_id, record,
                                                                     confirmed_revision);
        if (!saved)
        {
            journal_failed = true;
            report.issues.push_back(saved.error());
        }
        else
            ++confirmed_revision;
        return saved;
    };
    const auto publish_item = [&](ForeignCatalogItemReport row)
    {
        if (!row.phase.empty() && !row.resumed && !journal_failed)
        {
            if (row.status == ForeignCatalogItemStatus::kImported)
                row.phase = "complete";
            auto saved = checkpoint(row, row.status == ForeignCatalogItemStatus::kImported);
            if (!saved)
                row.reasons.push_back("checkpoint_write_failed");
        }
        count_item(report, row);
        report.items.push_back(std::move(row));
    };
    std::map<std::string, std::string> foreign_assets;
    std::set<std::string> fingerprinted_paths;
    std::map<std::string, std::size_t> fingerprint_index;
    for (const auto &[id, record] : checkpoints)
    {
        if (id.starts_with("collection/"))
            continue;
        auto previous = foreign_catalog_item_from_json(record.receipt_json).value();
        if (previous.source_fingerprint &&
            fingerprinted_paths.insert(previous.source_fingerprint->path).second)
        {
            fingerprint_index.emplace(previous.source_fingerprint->path,
                                      report.source_originals.size());
            report.source_originals.push_back(*previous.source_fingerprint);
        }
    }
    fingerprinted_paths.clear();
    bool preflight_failed = false;
    for (const auto &item : fixture.value().items)
    {
        auto checked = request.cancellation.check();
        if (!checked)
        {
            report.cancelled = true;
            break;
        }
        if (item.skip_reason)
            continue;
        auto path = resolve_fixture_path(fixture.value().source_root, item.original_path);
        if (!path)
            continue;
        std::error_code exists_error;
        if (!std::filesystem::is_regular_file(utf8_path(path.value()), exists_error) ||
            exists_error)
            continue;
        if (!fingerprinted_paths.insert(path.value()).second)
            continue;
        auto fingerprint = fingerprint_original(path.value(), request.cancellation);
        if (!fingerprint)
        {
            if (!request.resume)
                return fingerprint.error();
            report.issues.push_back(fingerprint.error());
            report.cancelled |= fingerprint.error().code == ErrorCode::kCancelled;
            preflight_failed = true;
            break;
        }
        const auto previous = fingerprint_index.find(path.value());
        if (previous == fingerprint_index.end())
        {
            fingerprint_index.emplace(path.value(), report.source_originals.size());
            report.source_originals.push_back(std::move(fingerprint).value());
        }
        else if (!fingerprints_equal(report.source_originals[previous->second],
                                     fingerprint.value()))
        {
            report.issues.push_back(make_error(
                ErrorCode::kConflict, "Source changed since conversion checkpoint",
                {{"reason", "foreign_conversion_source_conflict"}, {"path", path.value()}}));
            preflight_failed = true;
        }
    }

    if (request.progress)
        request.progress("preflight", report.source_originals.size(),
                         report.source_originals.size());
    if (auto active = request.cancellation.check(); !active)
    {
        if (!request.resume)
            return active.error();
        report.cancelled = true;
        preflight_failed = true;
    }
    if (fixture.value().native_lightroom && !request.resume)
    {
        auto archived = repository_->archive_foreign_catalog(
            fixture.value().source_path, fixture.value().source_sha256, request.cancellation);
        if (!archived)
            return archived.error();
        report.source_archive = std::move(archived).value();
        ++confirmed_revision; // Archive owner commits exactly one revision.
        auto begun = repository_->begin_foreign_conversion(
            report.conversion_id, fixture.value().source_sha256, confirmed_revision);
        if (!begun)
        {
            report.issues.push_back(begun.error());
            return report;
        }
        ++confirmed_revision;
    }

    for (const auto &item : fixture.value().items)
    {
        if (request.progress)
            request.progress("conversion", report.items.size(), fixture.value().items.size());
        auto still = request.cancellation.check();
        ForeignCatalogItemReport row;
        row.foreign_id = item.foreign_id;
        if (const auto previous = checkpoints.find(item.foreign_id); previous != checkpoints.end())
        {
            row = foreign_catalog_item_from_json(previous->second.receipt_json).value();
            row.resumed = true;
            if (!previous->second.complete)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.push_back("incomplete_conversion_requires_resolution");
            }
            if (row.asset_id)
                foreign_assets.emplace(item.foreign_id, *row.asset_id);
            publish_item(std::move(row));
            continue;
        }
        if (journal_failed || preflight_failed)
        {
            row.reasons.push_back(preflight_failed ? "source_preflight_failed" :
                                                     "conversion_checkpoint_unavailable");
            publish_item(std::move(row));
            continue;
        }
        const auto mutate = [&](const std::string &phase, auto &&action) -> decltype(action())
        {
            auto active = request.cancellation.check();
            if (!active)
                return active.error();
            row.phase = phase;
            auto saved = checkpoint(row, false);
            if (!saved)
                return saved.error();
            if (request.progress)
                request.progress(phase, report.items.size(), fixture.value().items.size());
            active = request.cancellation.check();
            if (!active)
                return active.error();
            using Outcome = decltype(action());
            Outcome outcome = make_error(ErrorCode::kIo, "Conversion stage was not started");
            if (report.conversion_id.empty())
                return action();
            bool ran = false;
            auto committed = repository_->run_foreign_conversion_stage(
                report.conversion_id, {row.foreign_id, row.phase, row.asset_id, false, ""},
                confirmed_revision,
                [&]
                {
                    ran = true;
                    outcome = action();
                });
            if (!committed)
            {
                journal_failed = true;
                report.issues.push_back(committed.error());
                return ran ? std::move(outcome) : Outcome(committed.error());
            }
            confirmed_revision = committed.value().revision;
            if (committed.value().target_id)
                row.asset_id = committed.value().target_id;
            if (request.progress && outcome)
                request.progress("committed:" + phase, report.items.size(),
                                 fixture.value().items.size());
            return outcome;
        };
        if (!still)
        {
            row.status = ForeignCatalogItemStatus::kSkipped;
            row.reasons.emplace_back("cancelled");
            report.cancelled = true;
            publish_item(std::move(row));
            continue;
        }

        if (item.skip_reason)
        {
            row.status = ForeignCatalogItemStatus::kUnsupported;
            row.original_path = item.original_path;
            row.reasons.push_back(*item.skip_reason);
            publish_item(std::move(row));
            continue;
        }
        auto original = resolve_fixture_path(fixture.value().source_root, item.original_path);
        if (!original)
        {
            row.status = ForeignCatalogItemStatus::kFailed;
            row.reasons.emplace_back(original.error().context.count("reason") != 0U ?
                                         original.error().context.at("reason") :
                                         "foreign_catalog_item_path_invalid");
            publish_item(std::move(row));
            continue;
        }
        row.original_path = original.value();
        const auto fingerprint = fingerprint_index.find(original.value());
        if (fingerprint != fingerprint_index.end())
            row.source_fingerprint = report.source_originals[fingerprint->second];

        std::error_code exists_error;
        if (!std::filesystem::is_regular_file(utf8_path(original.value()), exists_error) ||
            exists_error)
        {
            row.status = ForeignCatalogItemStatus::kSkipped;
            row.reasons.emplace_back("missing_original");
            publish_item(std::move(row));
            continue;
        }

        auto imported = mutate(
            "import",
            [&]() -> Result<ImportItemResult>
            {
                if (!item.master_id)
                    return import_service_.import_one(original.value(), request.cancellation,
                                                      request.preview, request.defer_previews);
                const auto master = foreign_assets.find(*item.master_id);
                if (master == foreign_assets.end())
                    return make_error(ErrorCode::kNotFound, "Virtual copy master was not imported",
                                      {{"reason", "lightroom_master_not_imported"}});
                auto version = library_service_.create_asset_version(master->second);
                if (!version)
                    return version.error();
                ImportItemResult result;
                result.status = ImportItemStatus::kImported;
                result.asset = version.value().version;
                return result;
            });
        if (!imported)
        {
            const auto committed = imported.error().context.find("committed_asset_id");
            if (committed != imported.error().context.end())
                row.asset_id = committed->second;
            row.status = imported.error().code == ErrorCode::kCancelled ?
                             ForeignCatalogItemStatus::kSkipped :
                             (imported.error().code == ErrorCode::kUnsupported ?
                                  ForeignCatalogItemStatus::kUnsupported :
                                  ForeignCatalogItemStatus::kFailed);
            if (imported.error().code == ErrorCode::kCancelled)
                report.cancelled = true;
            row.reasons.emplace_back(imported.error().context.count("reason") != 0U ?
                                         imported.error().context.at("reason") :
                                         "import_failed");
            publish_item(std::move(row));
            continue;
        }
        if (imported.value().status != ImportItemStatus::kImported || !imported.value().asset)
        {
            if (imported.value().asset && imported.value().status == ImportItemStatus::kFailed)
                row.asset_id = imported.value().asset->id;
            row.status = imported.value().status == ImportItemStatus::kUnsupported ?
                             ForeignCatalogItemStatus::kUnsupported :
                             (imported.value().status == ImportItemStatus::kDuplicate ?
                                  ForeignCatalogItemStatus::kSkipped :
                                  ForeignCatalogItemStatus::kFailed);
            row.reasons.emplace_back(imported.value().status == ImportItemStatus::kDuplicate ?
                                         "duplicate_original" :
                                         "import_not_published");
            publish_item(std::move(row));
            continue;
        }

        const auto asset_id = imported.value().asset->id;
        row.asset_id = asset_id;
        foreign_assets.emplace(item.foreign_id, asset_id);
        if (item.master_id)
        {
            // Version creation is its own committed stage. Independent-copy
            // clearing has separate provenance, without nested stage bindings.
            auto reset =
                mutate("copy_reset", [&] { return develop_service_.reset_recipe(asset_id); });
            auto tags = reset ? mutate("copy_keywords_clear",
                                       [&] { return metadata_service_.set_tags(asset_id, {}); }) :
                                Result<AssetRecord>(reset.error());
            auto metadata =
                tags ? mutate("copy_metadata_clear", [&]
                              { return metadata_service_.set_writable_metadata(asset_id, {}); }) :
                       Result<AssetRecord>(tags.error());
            if (!metadata)
            {
                row.reasons.push_back("copy_initialization_failed");
                report.cancelled |= metadata.error().code == ErrorCode::kCancelled;
                publish_item(std::move(row));
                continue;
            }
            imported.value().asset = metadata.value();
        }

        add_mapped(row, "original");
        if (item.master_id)
            add_mapped(row, "virtual_copy");

        if (item.rating)
        {
            auto rated = mutate("rating", [&]
                                { return library_service_.set_rating(asset_id, *item.rating); });
            if (!rated)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("rating_failed");
                publish_item(std::move(row));
                continue;
            }
            add_mapped(row, "rating");
        }
        if (item.color_label)
        {
            auto labeled =
                mutate("color_label", [&]
                       { return library_service_.set_color_label(asset_id, *item.color_label); });
            if (!labeled)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("color_label_failed");
                publish_item(std::move(row));
                continue;
            }
            add_mapped(row, "color_label");
        }
        if (item.rejected)
        {
            auto flagged =
                mutate("rejected",
                       [&] { return library_service_.set_rejected(asset_id, *item.rejected); });
            if (!flagged)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("rejected_failed");
                publish_item(std::move(row));
                continue;
            }
            add_mapped(row, "rejected");
        }
        if (item.picked)
        {
            auto picked = mutate("picked", [&]
                                 { return library_service_.set_picked(asset_id, *item.picked); });
            if (!picked)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("picked_failed");
                publish_item(std::move(row));
                continue;
            }
            add_mapped(row, "picked");
        }
        if (has_writable_metadata(item.metadata))
        {
            auto written = mutate(
                "metadata",
                [&] { return metadata_service_.set_writable_metadata(asset_id, item.metadata); });
            if (!written)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("metadata_failed");
                publish_item(std::move(row));
                continue;
            }
            if (item.metadata.title)
                add_mapped(row, "title");
            if (item.metadata.description)
                add_mapped(row, "description");
            if (item.metadata.creator)
                add_mapped(row, "creator");
            if (item.metadata.copyright)
                add_mapped(row, "copyright");
            if (item.metadata.country)
                add_mapped(row, "country");
            if (item.metadata.province_state)
                add_mapped(row, "province_state");
            if (item.metadata.city)
                add_mapped(row, "city");
            if (item.metadata.sublocation)
                add_mapped(row, "sublocation");
            if (item.metadata.headline)
                add_mapped(row, "headline");
            if (item.metadata.credit)
                add_mapped(row, "credit");
            if (item.metadata.source)
                add_mapped(row, "source");
            if (item.metadata.instructions)
                add_mapped(row, "instructions");
            if (item.metadata.usage_terms)
                add_mapped(row, "usage_terms");
            if (item.metadata.job_id)
                add_mapped(row, "job_id");
        }
        if (!item.keywords.empty())
        {
            auto tagged = mutate("keywords", [&]
                                 { return metadata_service_.set_tags(asset_id, item.keywords); });
            if (!tagged)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("keywords_failed");
                publish_item(std::move(row));
                continue;
            }
            add_mapped(row, "keywords");
        }

        for (const auto &adjust : item.unsupported_adjusts)
        {
            CrsOmission omission;
            omission.key = adjust;
            omission.reason = "unsupported_foreign_adjust";
            row.unsupported_fields.push_back(std::move(omission));
        }

        const auto import_develop_states = [&]() -> Result<void>
        {
            if (!item.develop_settings && item.develop_states.empty())
                return {};
            auto baseline = develop_service_.load_baseline_recipe(asset_id);
            if (!baseline)
                return baseline.error();
            const auto apply_settings = [&](const std::string &settings,
                                            const std::string &context) -> Result<Recipe>
            {
                auto params = develop_from_recipe(baseline.value());
                if (!params)
                    return params.error();
                const auto &asset = *imported.value().asset;
                auto converted = import_lightroom_develop(
                    settings, {asset.id, asset.normalized_uri, asset.content_fingerprint});
                if (!converted)
                    return converted.error();
                for (auto omission : converted.value().omitted)
                {
                    omission.key = context + ":" + omission.key;
                    row.unsupported_fields.push_back(std::move(omission));
                }
                if (converted.value().compatible_groups == 0)
                    return make_error(ErrorCode::kUnsupported,
                                      "No compatible Lightroom adjustments",
                                      {{"reason", "lightroom_no_compatible_adjustments"}});
                apply_lightroom_develop(params.value(), converted.value());
                return recipe_from_develop(baseline.value().asset, params.value());
            };
            for (const auto &state : item.develop_states)
            {
                auto checked = request.cancellation.check();
                if (!checked)
                    return checked.error();
                auto recipe =
                    apply_settings(state.settings, state.snapshot ? "snapshot" : "history");
                if (!recipe)
                {
                    row.unsupported_fields.push_back({state.name, "",
                                                      recipe.error().context.contains("reason") ?
                                                          recipe.error().context.at("reason") :
                                                          "invalid_lightroom_develop_state"});
                    continue;
                }
                const auto label =
                    "Lightroom: " + (state.name.empty() ? "Imported state" : state.name);
                auto recorded =
                    mutate("history:" + std::to_string(&state - item.develop_states.data()),
                           [&]
                           {
                               return develop_service_.create_recipe_snapshot(
                                   asset_id, recipe.value(), label);
                           });
                if (!recorded)
                    return recorded.error();
                add_mapped(row, state.snapshot ? "snapshot" : "history");
            }
            if (item.develop_settings)
            {
                auto checked = request.cancellation.check();
                if (!checked)
                    return checked.error();
                auto recipe = apply_settings(*item.develop_settings, "develop");
                if (!recipe)
                    row.unsupported_fields.push_back({"lightroom.develop", "",
                                                      recipe.error().context.contains("reason") ?
                                                          recipe.error().context.at("reason") :
                                                          "invalid_lightroom_develop"});
                else
                {
                    auto saved =
                        mutate("develop", [&]
                               { return develop_service_.save_recipe(asset_id, recipe.value()); });
                    if (!saved)
                        return saved.error();
                    add_mapped(row, "develop");
                }
            }
            return {};
        };
        auto developed = import_develop_states();
        if (!developed)
        {
            const bool was_cancelled = developed.error().code == ErrorCode::kCancelled;
            report.cancelled |= was_cancelled;
            row.status = was_cancelled ? ForeignCatalogItemStatus::kSkipped :
                                         ForeignCatalogItemStatus::kFailed;
            row.reasons.emplace_back(was_cancelled ? "cancelled" : "develop_import_failed");
            publish_item(std::move(row));
            continue;
        }

        if (item.crs_xmp_path)
        {
            auto crs_path = resolve_fixture_path(fixture.value().source_root, *item.crs_xmp_path);
            if (!crs_path)
            {
                CrsOmission omission;
                omission.key = *item.crs_xmp_path;
                omission.reason = "crs_xmp_path_invalid";
                row.unsupported_fields.push_back(std::move(omission));
            }
            else
            {
                auto text = read_utf8_text_file(crs_path.value());
                if (!text)
                {
                    CrsOmission omission;
                    omission.key = crs_path.value();
                    omission.reason = "crs_xmp_unreadable";
                    row.unsupported_fields.push_back(std::move(omission));
                }
                else if (!is_crs_xmp_document(text.value()))
                {
                    CrsOmission omission;
                    omission.key = "crs";
                    omission.reason = "unsupported_xmp_dialect";
                    row.unsupported_fields.push_back(std::move(omission));
                }
                else
                {
                    auto loaded = develop_service_.load_recipe(asset_id);
                    if (!loaded)
                    {
                        row.status = ForeignCatalogItemStatus::kFailed;
                        row.reasons.emplace_back("recipe_load_failed");
                        publish_item(std::move(row));
                        continue;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        row.status = ForeignCatalogItemStatus::kFailed;
                        row.reasons.emplace_back("recipe_develop_failed");
                        publish_item(std::move(row));
                        continue;
                    }
                    const auto asset = imported.value().asset;
                    AssetDescriptor descriptor{asset->id, asset->normalized_uri,
                                               asset->content_fingerprint};
                    auto crs = import_crs_xmp({text.value(), descriptor});
                    if (!crs)
                    {
                        CrsOmission omission;
                        omission.key = "crs";
                        const auto reason = crs.error().context.find("reason");
                        omission.reason = reason != crs.error().context.end() ? reason->second :
                                                                                "unsupported_crs";
                        row.unsupported_fields.push_back(std::move(omission));
                    }
                    else
                    {
                        apply_crs_look(params.value(), crs.value().look, crs.value().mask);
                        auto saved = mutate(
                            "crs", [&]
                            { return develop_service_.save_develop(asset_id, params.value()); });
                        if (!saved)
                        {
                            row.status = ForeignCatalogItemStatus::kFailed;
                            row.reasons.emplace_back("crs_apply_failed");
                            publish_item(std::move(row));
                            continue;
                        }
                        add_mapped(row, "crs");
                        for (const auto &omitted : crs.value().omitted)
                            row.unsupported_fields.push_back(omitted);
                    }
                }
            }
        }

        row.status = ForeignCatalogItemStatus::kImported;
        publish_item(std::move(row));
    }

    std::map<std::string, const ForeignCatalogCollection *> collection_index;
    for (const auto &collection : fixture.value().collections)
        collection_index.emplace(collection.foreign_id, &collection);
    for (const auto &collection : fixture.value().collections)
    {
        ForeignCatalogCollectionReport row;
        row.foreign_id = collection.foreign_id;
        row.name = collection.name;
        const auto record_id = "collection/" + collection.foreign_id;
        if (const auto previous = checkpoints.find(record_id); previous != checkpoints.end())
        {
            row = foreign_catalog_collection_from_json(previous->second.receipt_json).value();
            const bool pending =
                !row.pending_photo_ids.empty() ||
                std::any_of(row.reasons.begin(), row.reasons.end(), [](const auto &reason)
                            { return reason.starts_with("member_not_imported:"); });
            if (previous->second.complete && !pending)
            {
                report.collections.push_back(std::move(row));
                continue;
            }
            row.reasons.clear();
            row.pending_photo_ids.clear();
            row.name = collection.name;
        }
        auto parent = collection.parent_id;
        if (parent && *parent != "0")
            row.reasons.push_back("collection_hierarchy_flattened");
        while (parent && *parent != "0")
        {
            const auto *ancestor = collection_index.at(*parent);
            row.name = ancestor->name + " / " + row.name;
            parent = ancestor->parent_id;
        }
        auto still = request.cancellation.check();
        if (!still || journal_failed || preflight_failed)
        {
            row.reasons.push_back(!still ? "cancelled" :
                                           (journal_failed ? "conversion_checkpoint_unavailable" :
                                                             "source_preflight_failed"));
            report.cancelled |= !still;
        }
        else
        {
            std::vector<std::string> members;
            bool partial_selection = false;
            for (const auto &id : collection.photo_ids)
            {
                if (!selected_ids.empty() && !selected_ids.contains(id))
                {
                    partial_selection = true;
                    continue;
                }
                const auto found = foreign_assets.find(id);
                if (found == foreign_assets.end())
                {
                    row.reasons.push_back("member_not_imported:" + id);
                    row.pending_photo_ids.push_back(id);
                }
                else
                    members.push_back(found->second);
            }
            if (partial_selection)
                row.reasons.push_back("partial_selection_membership");
            const bool smart = collection.creation_id.find("smart") != std::string::npos;
            if (smart)
            {
                row.reasons.push_back("smart_rules_not_evaluated");
                row.name += " [Lightroom smart snapshot]";
            }
            const auto save_collection = [&](const bool complete) -> Result<void>
            {
                if (report.conversion_id.empty())
                    return {};
                auto saved = repository_->save_foreign_conversion_checkpoint(
                    report.conversion_id,
                    {record_id, complete ? "complete" : "collection", row.set_id, complete,
                     serialize_json(foreign_catalog_collection_to_json(row))},
                    confirmed_revision);
                if (saved)
                    ++confirmed_revision;
                return saved;
            };
            auto started = save_collection(false);
            if (!started)
            {
                journal_failed = true;
                report.issues.push_back(started.error());
                row.reasons.push_back("checkpoint_write_failed");
                report.collections.push_back(std::move(row));
                continue;
            }
            Result<LibrarySetMutation> created =
                make_error(ErrorCode::kIo, "Collection stage was not started");
            const auto action = [&]
            {
                created = row.set_id ?
                              library_service_.add_library_set_members(*row.set_id, members,
                                                                       confirmed_revision) :
                              library_service_.create_library_set(LibrarySetKind::kManual, row.name,
                                                                  std::nullopt, members,
                                                                  confirmed_revision);
            };
            if (report.conversion_id.empty())
                action();
            else
            {
                auto committed = repository_->run_foreign_conversion_stage(
                    report.conversion_id, {record_id, "collection", row.set_id, false, ""},
                    confirmed_revision, action);
                if (!committed)
                {
                    journal_failed = true;
                    report.issues.push_back(committed.error());
                }
                else
                {
                    confirmed_revision = committed.value().revision;
                    if (committed.value().target_id)
                        row.set_id = committed.value().target_id;
                    if (request.progress && created)
                        request.progress("committed:collection", report.collections.size(),
                                         fixture.value().collections.size());
                }
            }
            if (!created)
                row.reasons.push_back("collection_create_failed:" + created.error().message);
            else
            {
                row.set_id = created.value().set.id;
                row.imported_members = static_cast<std::size_t>(created.value().set.asset_count);
            }
            auto saved =
                journal_failed ?
                    Result<void>(make_error(ErrorCode::kConflict,
                                            "Conversion journal is no longer writable")) :
                    save_collection(static_cast<bool>(created) && row.pending_photo_ids.empty());
            if (!saved)
            {
                journal_failed = true;
                report.issues.push_back(saved.error());
                row.reasons.push_back("checkpoint_write_failed");
            }
        }
        report.collections.push_back(std::move(row));
    }

    for (auto &fingerprint : report.source_originals)
    {
        if (request.progress)
            request.progress("source_audit", report.source_audits.size(),
                             report.source_originals.size());
        ForeignCatalogSourceAudit audit;
        audit.before = fingerprint;
        auto after = fingerprint_original(fingerprint.path, request.cancellation);
        if (!after)
        {
            audit.error = after.error();
            audit.status = after.error().code == ErrorCode::kCancelled ? "cancelled" : "failed";
            report.cancelled |= after.error().code == ErrorCode::kCancelled;
        }
        else
        {
            audit.after = std::move(after).value();
            audit.status = fingerprints_equal(fingerprint, *audit.after) ? "verified" : "changed";
            if (audit.status == "changed")
                audit.error = make_error(
                    ErrorCode::kConflict, "Source changed during conversion",
                    {{"path", fingerprint.path}, {"reason", "source_changed_during_conversion"}});
        }
        report.source_audits.push_back(std::move(audit));
    }
    report.source_audit_complete =
        !request.cancellation.is_cancellation_requested() &&
        std::all_of(report.source_audits.begin(), report.source_audits.end(), [](const auto &audit)
                    { return audit.status == "verified" || audit.status == "changed"; });
    report.originals_unchanged =
        report.source_audit_complete &&
        std::all_of(report.source_audits.begin(), report.source_audits.end(),
                    [](const auto &audit) { return audit.status == "verified"; });
    return report;
}

Result<std::vector<ForeignCatalogArchive>> ConversionService::foreign_catalog_archives() const
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_foreign_catalog_archives();
}

Result<std::optional<ForeignConversionJournal>>
ConversionService::foreign_conversion_status(const std::string_view id) const
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->load_foreign_conversion(id);
}

Result<std::vector<std::string>> ConversionService::foreign_conversion_ids() const
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_foreign_conversion_ids();
}

Result<ForeignCatalogInspection>
ConversionService::inspect_lightroom_catalog(const std::string_view source_path,
                                             const CancellationToken &cancellation)
{
    auto source = normalize_local_input(source_path);
    if (!source)
        return source.error();
    auto snapshot = read_lightroom_catalog(source.value().path, cancellation);
    if (!snapshot)
        return snapshot.error();
    ForeignCatalogInspection result;
    result.source_path = source.value().path;
    result.source_sha256 = snapshot.value().source_sha256;
    result.photos = snapshot.value().photos.size();
    result.collections = snapshot.value().collections.size();
    result.archived_only_tables = std::move(snapshot.value().archived_only_tables);
    std::set<std::string> sampled_extensions;
    for (const auto &photo : snapshot.value().photos)
    {
        auto checked = cancellation.check();
        if (!checked)
            return checked.error();
        result.virtual_copies += photo.master_id.has_value();
        result.metadata_photos += has_writable_metadata(photo.metadata);
        std::error_code error;
        if (!photo.skip_reason &&
            std::filesystem::is_regular_file(utf8_path(photo.original_path), error) && !error)
            ++result.available_originals;
        if (result.sample_photos.size() < 10 && !photo.master_id &&
            sampled_extensions.insert(extension_lower(utf8_path(photo.original_path))).second)
            result.sample_photos.push_back(
                {photo.foreign_id, photo.original_path, photo.master_id});
        for (const auto &state : photo.develop_states)
            state.snapshot ? ++result.snapshots : ++result.history_steps;
        if (!photo.develop_settings)
            continue;
        ++result.current_edits;
        auto fields = parse_lightroom_develop_fields(*photo.develop_settings);
        if (!fields)
        {
            ++result.malformed_edits;
            auto prefix_size = std::min<std::size_t>(128, photo.develop_settings->size());
            if (prefix_size < photo.develop_settings->size())
                while (prefix_size > 0 &&
                       (static_cast<unsigned char>((*photo.develop_settings)[prefix_size]) &
                        0xc0U) == 0x80U)
                    --prefix_size;
            if (result.malformed_samples.size() < 10)
                result.malformed_samples.push_back(
                    {{"foreign_id", photo.foreign_id},
                     {"detail", fields.error().context.at("detail")},
                     {"settings_prefix", photo.develop_settings->substr(0, prefix_size)},
                     {"settings_bytes", std::to_string(photo.develop_settings->size())}});
            continue;
        }
        const auto number = [&](const char *key, const double absent)
        {
            const auto found = fields.value().find(key);
            if (found == fields.value().end())
                return absent;
            double value = absent;
            return !found->second.starts_with('+') && parse_ascii_double(found->second, value) ?
                       value :
                       absent;
        };
        const auto table_has_data = [&](const char *key)
        {
            const auto found = fields.value().find(key);
            return found != fields.value().end() &&
                   found->second.find_first_not_of(" \t\r\n{}") != std::string::npos;
        };
        const auto sample = [&](const char *reason, const bool matches)
        {
            if (matches && !photo.master_id && !result.editing_samples.contains(reason))
                result.editing_samples.emplace(
                    reason, ForeignCatalogInspectionPhoto{photo.foreign_id, photo.original_path,
                                                          photo.master_id});
        };
        sample("nonzero_exposure", number("Exposure2012", 0) != 0);
        sample("cropped", number("CropLeft", 0) != 0 || number("CropTop", 0) != 0 ||
                              number("CropRight", 1) != 1 || number("CropBottom", 1) != 1);
        const auto wb = fields.value().find("WhiteBalance");
        sample("custom_white_balance", wb != fields.value().end() && wb->second == "\"Custom\"");
        sample("mask_data", table_has_data("MaskGroupBasedCorrections") ||
                                table_has_data("PaintBasedCorrections") ||
                                table_has_data("GradientBasedCorrections") ||
                                table_has_data("CircularGradientBasedCorrections"));
        sample("retouch_data", table_has_data("RetouchAreas") || table_has_data("RetouchInfo"));
        sample("upright_enabled", number("PerspectiveUpright", 0) != 0);
        for (const auto &[key, value] : fields.value())
        {
            if (result.develop_fields.size() >= 8192 && !result.develop_fields.contains(key))
                return make_error(ErrorCode::kUnsupported, "Develop inventory exceeds key bound");
            ++result.develop_fields[key];
            if (key == "CameraProfile")
            {
                if (result.camera_profiles.size() >= 4096 &&
                    !result.camera_profiles.contains(value))
                    return make_error(ErrorCode::kUnsupported,
                                      "Profile inventory exceeds value bound");
                ++result.camera_profiles[value];
            }
        }
    }
    auto companion = utf8_path(source.value().path);
    companion.replace_extension(".lrcat-data");
    result.companion_path = path_text(companion);
    std::error_code error;
    result.companion_present = std::filesystem::exists(companion, error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot inspect Lightroom companion",
                          {{"detail", error.message()}});
    if (result.companion_present)
    {
        result.companion_is_directory = std::filesystem::is_directory(companion, error);
        if (error)
            return make_error(ErrorCode::kIo, "Cannot inspect Lightroom companion type",
                              {{"detail", error.message()}});
    }
    return result;
}

Result<void>
ConversionService::export_foreign_catalog_archive(const std::string_view source_id,
                                                  const std::string_view output_path,
                                                  const CancellationToken &cancellation) const
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->export_foreign_catalog_archive(source_id, output_path, cancellation);
}

} // namespace ravo
