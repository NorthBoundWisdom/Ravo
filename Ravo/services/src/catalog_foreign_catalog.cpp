#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"
#include "catalog_service_internal.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/foreign_catalog.h"

namespace ravo
{
namespace
{

struct ForeignCatalogFixtureItem
{
    std::string foreign_id;
    std::string original_path;
    std::optional<int> rating;
    std::optional<ColorLabel> color_label;
    std::optional<bool> rejected;
    WritableMetadata metadata;
    std::vector<std::string> keywords;
    std::optional<std::string> crs_xmp_path;
    std::vector<std::string> unsupported_adjusts;
};

struct ForeignCatalogFixture
{
    ForeignCatalogSourceKind source_kind = ForeignCatalogSourceKind::kLightroomClassic;
    std::optional<std::string> source_product_version;
    std::string source_path;
    std::string source_root;
    std::vector<ForeignCatalogFixtureItem> items;
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
fingerprint_original(const std::string_view path)
{
    auto digest = sha256_file_hex(path);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
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
                             const std::optional<ForeignCatalogSourceKind> expected_kind)
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
        return make_error(ErrorCode::kUnsupported,
                          "Vendor Lightroom catalogs are not a packaged conversion source",
                          {{"path", location.value().path},
                           {"reason", "unsupported_source_schema"},
                           {"detail", "lightroom_lrcat_reader_not_packaged"}});
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
           metadata.country || metadata.province_state || metadata.city || metadata.sublocation;
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
CatalogService::convert_foreign_catalog(const ForeignCatalogConversionRequest &request)
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

    auto fixture = load_foreign_catalog_fixture(request.source_path, request.source_kind);
    if (!fixture)
        return fixture.error();

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    auto existing = list_assets();
    if (!existing)
        return existing.error();
    if (!existing.value().empty())
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
    report.source_kind = fixture.value().source_kind;
    report.source_product_version = fixture.value().source_product_version;
    report.source_path = fixture.value().source_path;
    report.destination_catalog = snapshot.value().database_path;

    for (const auto &item : fixture.value().items)
    {
        auto path = resolve_fixture_path(fixture.value().source_root, item.original_path);
        if (!path)
            continue;
        std::error_code exists_error;
        if (!std::filesystem::is_regular_file(utf8_path(path.value()), exists_error) ||
            exists_error)
            continue;
        auto fingerprint = fingerprint_original(path.value());
        if (!fingerprint)
            return fingerprint.error();
        report.source_originals.push_back(std::move(fingerprint).value());
    }

    for (const auto &item : fixture.value().items)
    {
        auto still = request.cancellation.check();
        ForeignCatalogItemReport row;
        row.foreign_id = item.foreign_id;
        if (!still)
        {
            row.status = ForeignCatalogItemStatus::kSkipped;
            row.reasons.emplace_back("cancelled");
            report.cancelled = true;
            count_item(report, row);
            report.items.push_back(std::move(row));
            continue;
        }

        auto original = resolve_fixture_path(fixture.value().source_root, item.original_path);
        if (!original)
        {
            row.status = ForeignCatalogItemStatus::kFailed;
            row.reasons.emplace_back(original.error().context.count("reason") != 0U ?
                                         original.error().context.at("reason") :
                                         "foreign_catalog_item_path_invalid");
            count_item(report, row);
            report.items.push_back(std::move(row));
            continue;
        }
        row.original_path = original.value();

        std::error_code exists_error;
        if (!std::filesystem::is_regular_file(utf8_path(original.value()), exists_error) ||
            exists_error)
        {
            row.status = ForeignCatalogItemStatus::kSkipped;
            row.reasons.emplace_back("missing_original");
            count_item(report, row);
            report.items.push_back(std::move(row));
            continue;
        }

        auto imported = import_one(original.value(), request.cancellation, request.preview,
                                   request.defer_previews);
        if (!imported)
        {
            row.status = imported.error().code == ErrorCode::kCancelled ?
                             ForeignCatalogItemStatus::kSkipped :
                             ForeignCatalogItemStatus::kFailed;
            if (imported.error().code == ErrorCode::kCancelled)
                report.cancelled = true;
            row.reasons.emplace_back(imported.error().context.count("reason") != 0U ?
                                         imported.error().context.at("reason") :
                                         "import_failed");
            count_item(report, row);
            report.items.push_back(std::move(row));
            continue;
        }
        if (imported.value().status != ImportItemStatus::kImported || !imported.value().asset)
        {
            row.status = imported.value().status == ImportItemStatus::kUnsupported ?
                             ForeignCatalogItemStatus::kUnsupported :
                             (imported.value().status == ImportItemStatus::kDuplicate ?
                                  ForeignCatalogItemStatus::kSkipped :
                                  ForeignCatalogItemStatus::kFailed);
            row.reasons.emplace_back(imported.value().status == ImportItemStatus::kDuplicate ?
                                         "duplicate_original" :
                                         "import_not_published");
            count_item(report, row);
            report.items.push_back(std::move(row));
            continue;
        }

        const auto asset_id = imported.value().asset->id;
        row.asset_id = asset_id;
        add_mapped(row, "original");

        if (item.rating)
        {
            auto rated = set_rating(asset_id, *item.rating);
            if (!rated)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("rating_failed");
                count_item(report, row);
                report.items.push_back(std::move(row));
                continue;
            }
            add_mapped(row, "rating");
        }
        if (item.color_label)
        {
            auto labeled = set_color_label(asset_id, *item.color_label);
            if (!labeled)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("color_label_failed");
                count_item(report, row);
                report.items.push_back(std::move(row));
                continue;
            }
            add_mapped(row, "color_label");
        }
        if (item.rejected)
        {
            auto flagged = set_rejected(asset_id, *item.rejected);
            if (!flagged)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("rejected_failed");
                count_item(report, row);
                report.items.push_back(std::move(row));
                continue;
            }
            add_mapped(row, "rejected");
        }
        if (has_writable_metadata(item.metadata))
        {
            auto written = set_writable_metadata(asset_id, item.metadata);
            if (!written)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("metadata_failed");
                count_item(report, row);
                report.items.push_back(std::move(row));
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
        }
        if (!item.keywords.empty())
        {
            auto tagged = set_tags(asset_id, item.keywords);
            if (!tagged)
            {
                row.status = ForeignCatalogItemStatus::kFailed;
                row.reasons.emplace_back("keywords_failed");
                count_item(report, row);
                report.items.push_back(std::move(row));
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
                    auto loaded = load_recipe(asset_id);
                    if (!loaded)
                    {
                        row.status = ForeignCatalogItemStatus::kFailed;
                        row.reasons.emplace_back("recipe_load_failed");
                        count_item(report, row);
                        report.items.push_back(std::move(row));
                        continue;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        row.status = ForeignCatalogItemStatus::kFailed;
                        row.reasons.emplace_back("recipe_develop_failed");
                        count_item(report, row);
                        report.items.push_back(std::move(row));
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
                        auto saved = save_develop(asset_id, params.value());
                        if (!saved)
                        {
                            row.status = ForeignCatalogItemStatus::kFailed;
                            row.reasons.emplace_back("crs_apply_failed");
                            count_item(report, row);
                            report.items.push_back(std::move(row));
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
        count_item(report, row);
        report.items.push_back(std::move(row));
    }

    for (auto &fingerprint : report.source_originals)
    {
        auto after = fingerprint_original(fingerprint.path);
        if (!after)
            return after.error();
        if (!fingerprints_equal(fingerprint, after.value()))
        {
            report.originals_unchanged = false;
            return make_error(
                ErrorCode::kConflict, "Foreign catalog conversion mutated a source original",
                {{"path", fingerprint.path}, {"reason", "foreign_catalog_original_mutated"}});
        }
    }
    return report;
}

} // namespace ravo
