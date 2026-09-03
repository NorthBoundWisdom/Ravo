#include "ravo/adapters/sqlite_catalog.h"

#include "catalog_repository_test_control.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <QtCore/QFileInfo>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QMetaType>
#include <QtCore/QSaveFile>
#include <QtCore/QVariant>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"

#include "catalog_sql_internal.h"

namespace ravo
{
namespace sqlite_internal
{

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] std::string utf8_from_qstring(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] QString contains_like_pattern(const std::string_view text)
{
    QString escaped = qstring_from_utf8(text);
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("%"), QStringLiteral("\\%"));
    escaped.replace(QStringLiteral("_"), QStringLiteral("\\_"));
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

[[nodiscard]] QString prefix_like_pattern(const std::string_view text)
{
    QString escaped = qstring_from_utf8(text);
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("%"), QStringLiteral("\\%"));
    escaped.replace(QStringLiteral("_"), QStringLiteral("\\_"));
    return escaped + QStringLiteral("/%");
}

void append_library_query_predicates(const LibraryQuery &query, QStringList &predicates,
                                     QVariantList &bindings)
{
    const auto add =
        [&predicates, &bindings](QString predicate, std::initializer_list<QVariant> values)
    {
        predicates.push_back(std::move(predicate));
        for (const auto &value : values)
            bindings.push_back(value);
    };
    switch (query.rating_mode)
    {
    case RatingFilterMode::kAny:
        break;
    case RatingFilterMode::kMinimum:
        add(QStringLiteral("a.rating >= ?"), {query.rating_value});
        break;
    case RatingFilterMode::kExact:
        add(QStringLiteral("a.rating = ?"), {query.rating_value});
        break;
    }
    if (!query.color_labels.empty())
    {
        QStringList placeholders;
        for (const auto label : query.color_labels)
        {
            placeholders.push_back(QStringLiteral("?"));
            bindings.push_back(qstring_from_utf8(color_label_name(label)));
        }
        predicates.push_back(QStringLiteral("a.color_label IN (") +
                             placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    }
    switch (query.reject_filter)
    {
    case RejectFilter::kInclude:
        break;
    case RejectFilter::kExclude:
        predicates.push_back(QStringLiteral("a.rejected = 0"));
        break;
    case RejectFilter::kOnly:
        predicates.push_back(QStringLiteral("a.rejected != 0"));
        break;
    }
    if (!query.folder_uri.empty())
        add(QStringLiteral("(a.folder_uri = ? OR a.folder_uri LIKE ? ESCAPE '\\')"),
            {qstring_from_utf8(query.folder_uri), prefix_like_pattern(query.folder_uri)});
    if (!query.tag.empty())
        add(QStringLiteral("a.id IN (SELECT t.asset_id FROM asset_tag t WHERE t.name = ?)"),
            {qstring_from_utf8(query.tag)});
    if (!query.text.empty())
    {
        const auto pattern = contains_like_pattern(query.text);
        add(QStringLiteral("(a.display_name LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "a.normalized_uri LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "a.media_type LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.title LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.description LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.creator LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.copyright LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.camera_make LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.camera_model LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "EXISTS(SELECT 1 FROM asset_tag tx WHERE tx.asset_id = a.id "
                           "AND tx.name LIKE ? ESCAPE '\\' COLLATE NOCASE))"),
            {pattern, pattern, pattern, pattern, pattern, pattern, pattern, pattern, pattern,
             pattern});
    }
    if (!query.media_types.empty())
    {
        QStringList placeholders;
        for (const auto &media_type : query.media_types)
        {
            placeholders.push_back(QStringLiteral("?"));
            bindings.push_back(qstring_from_utf8(media_type));
        }
        predicates.push_back(QStringLiteral("a.media_type IN (") +
                             placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    }
    switch (query.edit_filter)
    {
    case EditFilter::kAny:
        break;
    case EditFilter::kEdited:
        predicates.push_back(
            QStringLiteral("EXISTS(SELECT 1 FROM asset_recipe er WHERE er.asset_id = a.id)"));
        break;
    case EditFilter::kUnedited:
        predicates.push_back(
            QStringLiteral("NOT EXISTS(SELECT 1 FROM asset_recipe er WHERE er.asset_id = a.id)"));
        break;
    }
    if (!query.camera.empty())
    {
        const auto pattern = contains_like_pattern(query.camera);
        add(QStringLiteral("(m.camera_make LIKE ? ESCAPE '\\' COLLATE NOCASE OR "
                           "m.camera_model LIKE ? ESCAPE '\\' COLLATE NOCASE)"),
            {pattern, pattern});
    }
    const auto add_range =
        [&predicates, &bindings](const QString &column, const LibraryNumericRange &range)
    {
        if (range.minimum)
        {
            predicates.push_back(column + QStringLiteral(" >= ?"));
            bindings.push_back(*range.minimum);
        }
        if (range.maximum)
        {
            predicates.push_back(column + QStringLiteral(" <= ?"));
            bindings.push_back(*range.maximum);
        }
    };
    add_range(QStringLiteral("m.iso"), query.iso);
    add_range(QStringLiteral("m.aperture"), query.aperture);
    add_range(QStringLiteral("m.focal_length_mm"), query.focal_length_mm);
    add_range(QStringLiteral("m.shutter_s"), query.shutter_s);
    add_range(QStringLiteral("(CAST(a.width AS REAL) / NULLIF(a.height, 0))"), query.aspect_ratio);
    if (query.imported_after_unix_ms)
        add(QStringLiteral("a.created_unix_ms >= ?"),
            {static_cast<qlonglong>(*query.imported_after_unix_ms)});
    if (query.imported_before_unix_ms)
        add(QStringLiteral("a.created_unix_ms <= ?"),
            {static_cast<qlonglong>(*query.imported_before_unix_ms)});
    if (query.captured_after_unix_s)
        add(QStringLiteral("m.captured_unix_s >= ?"),
            {static_cast<qlonglong>(*query.captured_after_unix_s)});
    if (query.captured_before_unix_s)
        add(QStringLiteral("m.captured_unix_s <= ?"),
            {static_cast<qlonglong>(*query.captured_before_unix_s)});
    if (!query.collection_id.empty())
        add(QStringLiteral(
                "a.id IN (SELECT member.asset_id FROM library_set_member member WHERE member.set_id = ?)"),
            {qstring_from_utf8(query.collection_id)});
}

[[nodiscard]] TaskError map_sql_error(const QSqlQuery &query, const std::string_view action)
{
    return make_error(ErrorCode::kIo, "Catalog SQL statement failed",
                      {{"action", std::string(action)},
                       {"qt_error", utf8_from_qstring(query.lastError().text())}});
}

[[nodiscard]] Result<std::size_t> count_library_query(QSqlDatabase &database,
                                                      const LibraryQuery &query)
{
    QStringList predicates;
    QVariantList bindings;
    append_library_query_predicates(query, predicates, bindings);
    const auto filter_where =
        predicates.empty() ? QString{} :
                             QStringLiteral(" WHERE ") + predicates.join(QStringLiteral(" AND "));
    QSqlQuery count(database);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM asset a LEFT JOIN asset_metadata m "
                                 "ON m.asset_id = a.id") +
                  filter_where);
    for (const auto &binding : bindings)
        count.addBindValue(binding);
    if (!count.exec() || !count.next())
        return map_sql_error(count, "count_library_query");
    const auto total_value = count.value(0).toLongLong();
    if (total_value < 0)
        return make_error(ErrorCode::kValidation, "Library page count is invalid",
                          {{"reason", "invalid_library_page_count"}});
    return static_cast<std::size_t>(total_value);
}

[[nodiscard]] Result<LibrarySetRecord> read_library_set(QSqlDatabase &database, QSqlQuery &query)
{
    LibrarySetRecord record;
    record.id = utf8_from_qstring(query.value(0).toString());
    auto kind = parse_library_set_kind(utf8_from_qstring(query.value(1).toString()));
    if (!kind)
        return kind.error();
    record.kind = kind.value();
    record.name = utf8_from_qstring(query.value(2).toString());
    if (!query.value(3).isNull())
    {
        auto parsed = parse_library_query_document(utf8_from_qstring(query.value(3).toString()));
        if (!parsed)
            return parsed.error();
        record.query = std::move(parsed).value();
    }
    record.created_unix_ms = query.value(4).toLongLong();
    record.updated_unix_ms = query.value(5).toLongLong();
    if (record.kind == LibrarySetKind::kManual)
    {
        QSqlQuery count(database);
        count.prepare(QStringLiteral("SELECT COUNT(*) FROM library_set_member WHERE set_id = ?"));
        count.addBindValue(qstring_from_utf8(record.id));
        if (!count.exec() || !count.next())
            return map_sql_error(count, "count_library_set_members");
        record.asset_count = static_cast<std::size_t>(count.value(0).toLongLong());
    }
    else
    {
        LibraryQuery query_value = record.query.value_or(LibraryQuery{});
        auto counted = count_library_query(database, query_value);
        if (!counted)
            return counted.error();
        record.asset_count = counted.value();
    }
    auto valid = validate_library_set_record(record);
    if (!valid)
        return valid.error();
    return record;
}

[[nodiscard]] Result<std::set<std::string, std::less<>>>
asset_metadata_columns(QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(asset_metadata)")))
    {
        return map_sql_error(query, "read_asset_metadata_columns");
    }
    std::set<std::string, std::less<>> columns;
    while (query.next())
    {
        columns.insert(utf8_from_qstring(query.value(1).toString()));
    }
    return columns;
}

[[nodiscard]] Result<std::set<std::string, std::less<>>> asset_columns(QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(asset)")))
        return map_sql_error(query, "read_asset_columns");
    std::set<std::string, std::less<>> columns;
    while (query.next())
        columns.insert(utf8_from_qstring(query.value(1).toString()));
    return columns;
}

constexpr const char *kV5CaptureColumns[][2] = {
    {"captured_local_exif", "TEXT"},
    {"captured_subsecond_digits", "TEXT"},
    {"captured_utc_offset_minutes", "INTEGER"},
    {"gps_latitude_e6", "INTEGER"},
    {"gps_longitude_e6", "INTEGER"},
    {"gps_altitude_magnitude_mm", "INTEGER"},
    {"gps_altitude_ref", "INTEGER"},
};

[[nodiscard]] QVariant optional_string(const std::optional<std::string> &value)
{
    if (!value)
    {
        return QVariant();
    }
    return qstring_from_utf8(*value);
}

[[nodiscard]] QVariant optional_u32(const std::optional<std::uint32_t> &value)
{
    if (!value)
    {
        return QVariant();
    }
    return static_cast<qlonglong>(*value);
}

[[nodiscard]] QVariant optional_i64(const std::optional<std::int64_t> &value)
{
    if (!value)
    {
        return QVariant();
    }
    return static_cast<qlonglong>(*value);
}

[[nodiscard]] QVariant optional_i32(const std::optional<std::int32_t> &value)
{
    if (!value)
    {
        return QVariant();
    }
    return static_cast<qlonglong>(*value);
}

[[nodiscard]] Result<void> require_integer_storage(const QSqlQuery &query, const int index,
                                                   const std::string_view field)
{
    const QVariant value = query.value(index);
    const auto type = value.typeId();
    if (type != QMetaType::LongLong && type != QMetaType::Int && type != QMetaType::ULongLong &&
        type != QMetaType::UInt)
    {
        return make_error(
            ErrorCode::kValidation, "Catalog capture integer has the wrong storage class",
            {{"field", std::string(field)}, {"reason", "invalid_persisted_capture_storage_class"}});
    }
    return {};
}

[[nodiscard]] Result<void> require_text_storage(const QSqlQuery &query, const int index,
                                                const std::string_view field)
{
    const QVariant value = query.value(index);
    if (value.typeId() != QMetaType::QString)
    {
        return make_error(
            ErrorCode::kValidation, "Catalog capture text has the wrong storage class",
            {{"field", std::string(field)}, {"reason", "invalid_persisted_capture_storage_class"}});
    }
    return {};
}

[[nodiscard]] Result<std::optional<std::int32_t>>
i32_column(const QSqlQuery &query, const int index, const std::string_view field)
{
    if (query.isNull(index))
    {
        return std::optional<std::int32_t>{};
    }
    auto storage = require_integer_storage(query, index, field);
    if (!storage)
    {
        return storage.error();
    }
    bool converted = false;
    const qlonglong value = query.value(index).toLongLong(&converted);
    if (!converted || value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
    {
        return make_error(
            ErrorCode::kValidation, "Catalog capture integer is invalid",
            {{"field", std::string(field)}, {"reason", "invalid_persisted_capture_integer"}});
    }
    return std::optional<std::int32_t>{static_cast<std::int32_t>(value)};
}

[[nodiscard]] Result<std::optional<std::uint32_t>>
u32_column_checked(const QSqlQuery &query, const int index, const std::string_view field)
{
    if (query.isNull(index))
    {
        return std::optional<std::uint32_t>{};
    }
    auto storage = require_integer_storage(query, index, field);
    if (!storage)
    {
        return storage.error();
    }
    bool converted = false;
    const qulonglong value = query.value(index).toULongLong(&converted);
    if (!converted || value > std::numeric_limits<std::uint32_t>::max())
    {
        return make_error(
            ErrorCode::kValidation, "Catalog capture integer is invalid",
            {{"field", std::string(field)}, {"reason", "invalid_persisted_capture_integer"}});
    }
    return std::optional<std::uint32_t>{static_cast<std::uint32_t>(value)};
}

[[nodiscard]] Result<std::optional<std::string>>
text_column(const QSqlQuery &query, const int index, const std::string_view field)
{
    if (query.isNull(index))
    {
        return std::optional<std::string>{};
    }
    auto storage = require_text_storage(query, index, field);
    if (!storage)
    {
        return storage.error();
    }
    return std::optional<std::string>{utf8_from_qstring(query.value(index).toString())};
}

[[nodiscard]] std::optional<std::string> string_column(const QSqlQuery &query, const int index)
{
    if (query.isNull(index))
    {
        return std::nullopt;
    }
    return utf8_from_qstring(query.value(index).toString());
}

[[nodiscard]] std::optional<std::uint32_t> u32_column(const QSqlQuery &query, const int index)
{
    if (query.isNull(index))
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(query.value(index).toULongLong());
}

[[nodiscard]] std::optional<std::int64_t> i64_column(const QSqlQuery &query, const int index)
{
    if (query.isNull(index))
    {
        return std::nullopt;
    }
    return query.value(index).toLongLong();
}

[[nodiscard]] std::optional<double> double_column(const QSqlQuery &query, const int index)
{
    if (query.isNull(index))
    {
        return std::nullopt;
    }
    return query.value(index).toDouble();
}

[[nodiscard]] QVariant optional_double(const std::optional<double> &value)
{
    if (!value)
    {
        return QVariant();
    }
    return *value;
}

[[nodiscard]] AssetRecord read_asset(const QSqlQuery &query)
{
    AssetRecord asset;
    asset.id = utf8_from_qstring(query.value(0).toString());
    asset.normalized_uri = utf8_from_qstring(query.value(1).toString());
    asset.media_type = utf8_from_qstring(query.value(2).toString());
    asset.size_bytes = static_cast<std::uint64_t>(query.value(3).toULongLong());
    asset.mtime_unix_ms = query.value(4).toLongLong();
    asset.content_fingerprint = string_column(query, 5);
    asset.width = u32_column(query, 6);
    asset.height = u32_column(query, 7);
    asset.import_state = utf8_from_qstring(query.value(8).toString());
    asset.error_code = string_column(query, 9);
    asset.error_message = string_column(query, 10);
    asset.created_unix_ms = query.value(11).toLongLong();
    asset.review.rating = query.value(12).toInt();
    const auto label = parse_color_label(utf8_from_qstring(query.value(13).toString()));
    asset.review.color_label = label ? label.value() : ColorLabel::kNone;
    asset.review.rejected = query.value(14).toInt() != 0;
    asset.has_edits = query.value(15).toInt() != 0;
    asset.version_ordinal = query.value(16).toInt();
    asset.source_asset_id = string_column(query, 17);
    return asset;
}

[[nodiscard]] Result<void> attach_asset_fields(QSqlDatabase &database,
                                               std::vector<AssetRecord> &assets)
{
    if (assets.empty())
    {
        return {};
    }
    std::map<std::string, AssetRecord *, std::less<>> by_id;
    for (auto &asset : assets)
    {
        by_id.emplace(asset.id, &asset);
    }

    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(assets.size()));
    for (std::size_t index = 0; index < assets.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    const auto in_clause = placeholders.join(QLatin1Char(','));

    QSqlQuery tags(database);
    tags.prepare(QStringLiteral("SELECT asset_id, name FROM asset_tag WHERE asset_id IN (") +
                 in_clause + QStringLiteral(") ORDER BY name ASC"));
    for (const auto &asset : assets)
        tags.addBindValue(qstring_from_utf8(asset.id));
    if (!tags.exec())
    {
        return map_sql_error(tags, "list_asset_tags");
    }
    while (tags.next())
    {
        const auto id = utf8_from_qstring(tags.value(0).toString());
        const auto found = by_id.find(id);
        if (found != by_id.end())
        {
            found->second->tags.push_back(utf8_from_qstring(tags.value(1).toString()));
        }
    }

    QSqlQuery metadata(database);
    metadata.prepare(
        QStringLiteral("SELECT asset_id, title, description, creator, copyright, camera_make, "
                       "camera_model, iso, aperture, focal_length_mm, shutter_s, "
                       "captured_unix_s, captured_local_exif, captured_subsecond_digits, "
                       "captured_utc_offset_minutes, gps_latitude_e6, gps_longitude_e6, "
                       "gps_altitude_magnitude_mm, gps_altitude_ref, country, province_state, "
                       "city, sublocation FROM asset_metadata "
                       "WHERE asset_id IN (") +
        in_clause + QStringLiteral(")"));
    for (const auto &asset : assets)
        metadata.addBindValue(qstring_from_utf8(asset.id));
    if (!metadata.exec())
    {
        return map_sql_error(metadata, "list_asset_metadata");
    }
    while (metadata.next())
    {
        const auto id = utf8_from_qstring(metadata.value(0).toString());
        const auto found = by_id.find(id);
        if (found == by_id.end())
        {
            continue;
        }
        auto &asset = *found->second;
        asset.metadata.title = string_column(metadata, 1);
        asset.metadata.description = string_column(metadata, 2);
        asset.metadata.creator = string_column(metadata, 3);
        asset.metadata.copyright = string_column(metadata, 4);
        asset.metadata.country = string_column(metadata, 19);
        asset.metadata.province_state = string_column(metadata, 20);
        asset.metadata.city = string_column(metadata, 21);
        asset.metadata.sublocation = string_column(metadata, 22);
        asset.capture.camera_make = string_column(metadata, 5);
        asset.capture.camera_model = string_column(metadata, 6);
        asset.capture.iso = double_column(metadata, 7);
        asset.capture.aperture = double_column(metadata, 8);
        asset.capture.focal_length_mm = double_column(metadata, 9);
        asset.capture.shutter_s = double_column(metadata, 10);
        asset.capture.captured_unix_s = i64_column(metadata, 11);
        auto local_exif_value = text_column(metadata, 12, "captured_local_exif");
        if (!local_exif_value)
        {
            return local_exif_value.error();
        }
        auto subsecond_value = text_column(metadata, 13, "captured_subsecond_digits");
        if (!subsecond_value)
        {
            return subsecond_value.error();
        }
        auto offset_value = i32_column(metadata, 14, "captured_utc_offset_minutes");
        if (!offset_value)
        {
            return offset_value.error();
        }
        const auto local_exif = std::move(local_exif_value).value();
        const auto subsecond = std::move(subsecond_value).value();
        const auto offset = std::move(offset_value).value();
        if (local_exif || subsecond || offset)
        {
            if (!local_exif)
            {
                return make_error(ErrorCode::kValidation,
                                  "Catalog capture time components require a local time",
                                  {{"reason", "invalid_persisted_capture_datetime"}});
            }
            CaptureDateTime captured;
            captured.local_exif = *local_exif;
            captured.subsecond_digits = subsecond;
            captured.utc_offset_minutes = offset;
            asset.capture.captured_datetime = std::move(captured);
        }
        auto latitude_value = i32_column(metadata, 15, "gps_latitude_e6");
        if (!latitude_value)
        {
            return latitude_value.error();
        }
        auto longitude_value = i32_column(metadata, 16, "gps_longitude_e6");
        if (!longitude_value)
        {
            return longitude_value.error();
        }
        auto altitude_magnitude_value =
            u32_column_checked(metadata, 17, "gps_altitude_magnitude_mm");
        if (!altitude_magnitude_value)
        {
            return altitude_magnitude_value.error();
        }
        auto altitude_ref_value = i32_column(metadata, 18, "gps_altitude_ref");
        if (!altitude_ref_value)
        {
            return altitude_ref_value.error();
        }
        const auto latitude = std::move(latitude_value).value();
        const auto longitude = std::move(longitude_value).value();
        const auto altitude_magnitude = std::move(altitude_magnitude_value).value();
        const auto altitude_ref = std::move(altitude_ref_value).value();
        if (latitude || longitude || altitude_magnitude || altitude_ref)
        {
            if (!latitude || !longitude)
            {
                return make_error(ErrorCode::kValidation,
                                  "Catalog capture location requires both coordinates",
                                  {{"reason", "invalid_persisted_capture_location"}});
            }
            CaptureLocation location;
            location.latitude_e6 = *latitude;
            location.longitude_e6 = *longitude;
            if (altitude_magnitude || altitude_ref)
            {
                if (!altitude_magnitude || !altitude_ref)
                {
                    return make_error(
                        ErrorCode::kValidation,
                        "Catalog capture altitude requires both magnitude and reference",
                        {{"reason", "invalid_persisted_capture_altitude"}});
                }
                if (*altitude_ref != 0 && *altitude_ref != 1)
                {
                    return make_error(ErrorCode::kValidation,
                                      "Catalog capture altitude reference must be 0 or 1",
                                      {{"reason", "invalid_persisted_capture_altitude_ref"}});
                }
                CaptureAltitude altitude;
                altitude.magnitude_mm = *altitude_magnitude;
                altitude.reference = *altitude_ref == 1 ? CaptureAltitudeReference::kBelowSeaLevel :
                                                          CaptureAltitudeReference::kAboveSeaLevel;
                location.altitude = altitude;
            }
            asset.capture.location = location;
        }
        auto valid_capture = validate_capture_metadata(asset.capture);
        if (!valid_capture)
        {
            return valid_capture.error();
        }
    }
    QSqlQuery stacks(database);
    stacks.prepare(
        QStringLiteral(
            "SELECT m.asset_id, m.stack_id, m.position, s.pick_asset_id, "
            "(SELECT COUNT(*) FROM library_stack_member c WHERE c.stack_id = m.stack_id) "
            "FROM library_stack_member m INNER JOIN library_stack s ON s.id = m.stack_id "
            "WHERE m.asset_id IN (") +
        in_clause + QStringLiteral(")"));
    for (const auto &asset : assets)
        stacks.addBindValue(qstring_from_utf8(asset.id));
    if (!stacks.exec())
        return map_sql_error(stacks, "list_asset_stacks");
    while (stacks.next())
    {
        const auto id = utf8_from_qstring(stacks.value(0).toString());
        const auto found = by_id.find(id);
        if (found == by_id.end())
            continue;
        found->second->stack_id = utf8_from_qstring(stacks.value(1).toString());
        found->second->stack_position = stacks.value(2).toInt();
        found->second->stack_pick = utf8_from_qstring(stacks.value(3).toString()) == id;
        found->second->stack_count = stacks.value(4).toInt();
    }
    return {};
}

[[nodiscard]] Result<void> attach_asset_fields(QSqlDatabase &database, AssetRecord &asset)
{
    std::vector<AssetRecord> assets{asset};
    auto attached = attach_asset_fields(database, assets);
    if (!attached)
    {
        return attached.error();
    }
    asset = std::move(assets.front());
    return {};
}

[[nodiscard]] RecipeHistoryEntry read_history(const QSqlQuery &query)
{
    RecipeHistoryEntry entry;
    entry.id = query.value(0).toLongLong();
    entry.asset_id = utf8_from_qstring(query.value(1).toString());
    entry.seq = query.value(2).toLongLong();
    entry.kind = utf8_from_qstring(query.value(3).toString());
    entry.label = string_column(query, 4);
    entry.recipe_json = utf8_from_qstring(query.value(5).toString());
    entry.created_unix_ms = query.value(6).toLongLong();
    return entry;
}

[[nodiscard]] PreviewRecord read_preview(const QSqlQuery &query)
{
    PreviewRecord preview;
    preview.asset_id = utf8_from_qstring(query.value(0).toString());
    preview.contract_version = query.value(1).toLongLong();
    preview.cache_key = utf8_from_qstring(query.value(2).toString());
    preview.width = u32_column(query, 3);
    preview.height = u32_column(query, 4);
    preview.state = utf8_from_qstring(query.value(5).toString());
    preview.cache_relpath = string_column(query, 6);
    preview.last_success_unix_ms = i64_column(query, 7);
    return preview;
}

[[nodiscard]] AssetRecoveryState read_recovery_state(const QSqlQuery &query, const int first_column)
{
    AssetRecoveryState state;
    state.asset_id = utf8_from_qstring(query.value(first_column).toString());
    state.generation = query.value(first_column + 1).toLongLong();
    state.synchronized_generation = query.value(first_column + 2).toLongLong();
    return state;
}

[[nodiscard]] QString next_connection_name()
{
    static int counter = 0;
    return QString("ravo_catalog_%1").arg(++counter);
}

[[nodiscard]] bool valid_sha256(const std::string_view value)
{
    return value.size() == 64U && std::all_of(value.begin(), value.end(),
                                              [](const char character)
                                              {
                                                  return (character >= '0' && character <= '9') ||
                                                         (character >= 'a' && character <= 'f');
                                              });
}

[[nodiscard]] Result<std::pair<std::string, std::uint64_t>>
hash_database_file(const std::string_view path, const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const auto qt_path = qstring_from_utf8(path);
    const QFileInfo info(qt_path);
    if (!info.exists() || !info.isFile() || info.isSymLink())
    {
        return make_error(ErrorCode::kValidation, "Catalog backup database is not a regular file",
                          {{"path", std::string(path)}, {"reason", "backup_database_not_regular"}});
    }
    QFile file(qt_path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open catalog backup database",
                          {{"path", std::string(path)},
                           {"detail", utf8_from_qstring(file.errorString())},
                           {"reason", "backup_database_open_failed"}});
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::uint64_t total = 0U;
    constexpr qint64 kChunkBytes = 1024 * 1024;
    while (!file.atEnd())
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const auto bytes = file.read(kChunkBytes);
        if (bytes.isEmpty() && !file.atEnd())
        {
            return make_error(ErrorCode::kIo, "Unable to read catalog backup database",
                              {{"path", std::string(path)},
                               {"detail", utf8_from_qstring(file.errorString())},
                               {"reason", "backup_database_read_failed"}});
        }
        hash.addData(bytes);
        total += static_cast<std::uint64_t>(bytes.size());
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return std::pair<std::string, std::uint64_t>{hash.result().toHex().toStdString(), total};
}

[[nodiscard]] Result<CatalogDatabaseArtifact>
inspect_backup_database(const std::string_view path, const std::string_view expected_sha256,
                        const CancellationToken &cancellation)
{
    if (!expected_sha256.empty() && !valid_sha256(expected_sha256))
    {
        return make_error(ErrorCode::kInvalidArgument, "Expected catalog SHA-256 is invalid",
                          {{"reason", "invalid_backup_catalog_sha256"}});
    }
    auto digest = hash_database_file(path, cancellation);
    if (!digest)
    {
        return digest.error();
    }
    if (!expected_sha256.empty() && digest.value().first != expected_sha256)
    {
        return make_error(ErrorCode::kValidation,
                          "Catalog backup database checksum does not match its manifest",
                          {{"path", std::string(path)},
                           {"expected_sha256", std::string(expected_sha256)},
                           {"actual_sha256", digest.value().first},
                           {"reason", "backup_catalog_checksum_mismatch"}});
    }

    const auto connection = next_connection_name();
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(qstring_from_utf8(path));
    if (!database.open())
    {
        const auto detail = utf8_from_qstring(database.lastError().text());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return make_error(ErrorCode::kValidation, "Unable to open catalog backup database",
                          {{"path", std::string(path)},
                           {"detail", detail},
                           {"reason", "backup_database_invalid"}});
    }

    CatalogDatabaseArtifact artifact;
    artifact.path = std::string(path);
    auto digest_value = std::move(digest).value();
    artifact.sha256 = std::move(digest_value.first);
    artifact.bytes = digest_value.second;
    std::optional<TaskError> database_error;
    bool extra_schema_row = false;
    {
        QSqlQuery integrity(database);
        if (!integrity.exec(QStringLiteral("PRAGMA integrity_check")) || !integrity.next() ||
            integrity.value(0).toString() != QStringLiteral("ok") || integrity.next())
        {
            database_error = make_error(
                ErrorCode::kValidation, "Catalog backup database failed its integrity check",
                {{"path", std::string(path)},
                 {"detail", utf8_from_qstring(integrity.lastError().text())},
                 {"reason", "backup_catalog_integrity_failed"}});
        }
        if (!database_error)
        {
            QSqlQuery schema(database);
            if (!schema.exec(QStringLiteral(
                    "SELECT schema_version, catalog_id, revision FROM schema_info WHERE id = 1")) ||
                !schema.next())
            {
                database_error =
                    make_error(ErrorCode::kValidation,
                               "Catalog backup database is missing valid catalog identity",
                               {{"path", std::string(path)},
                                {"detail", utf8_from_qstring(schema.lastError().text())},
                                {"reason", "backup_catalog_identity_invalid"}});
            }
            else
            {
                artifact.schema_version = schema.value(0).toLongLong();
                artifact.catalog_id = utf8_from_qstring(schema.value(1).toString());
                artifact.revision = schema.value(2).toLongLong();
                extra_schema_row = schema.next();
            }
        }
        if (!database_error && !extra_schema_row)
        {
            QSqlQuery recovery(database);
            if (!recovery.exec(
                    QStringLiteral("SELECT asset_id, generation, synchronized_generation "
                                   "FROM asset_recovery_state ORDER BY asset_id ASC")))
            {
                database_error = make_error(
                    ErrorCode::kValidation, "Catalog backup database is missing recovery state",
                    {{"path", std::string(path)},
                     {"detail", utf8_from_qstring(recovery.lastError().text())},
                     {"reason", "backup_recovery_state_invalid"}});
            }
            else
            {
                while (recovery.next())
                {
                    artifact.recovery_states.push_back(read_recovery_state(recovery));
                }
            }
        }
        if (!database_error && !extra_schema_row)
        {
            QSqlQuery assets(database);
            if (!assets.exec(QStringLiteral("SELECT COUNT(*) FROM asset")) || !assets.next() ||
                assets.value(0).toLongLong() < 0 ||
                static_cast<std::uint64_t>(assets.value(0).toLongLong()) !=
                    artifact.recovery_states.size() ||
                assets.next())
            {
                database_error =
                    make_error(ErrorCode::kValidation,
                               "Catalog backup recovery state does not cover every asset",
                               {{"path", std::string(path)},
                                {"detail", utf8_from_qstring(assets.lastError().text())},
                                {"reason", "backup_recovery_coverage_invalid"}});
            }
        }
        if (!database_error && !extra_schema_row)
        {
            QSqlQuery previews(database);
            if (!previews.exec(QStringLiteral("SELECT COUNT(*) FROM preview")) ||
                !previews.next() || previews.value(0).toLongLong() != 0 || previews.next())
            {
                database_error =
                    make_error(ErrorCode::kValidation,
                               "Catalog backup database contains rebuildable preview state",
                               {{"path", std::string(path)},
                                {"detail", utf8_from_qstring(previews.lastError().text())},
                                {"reason", "backup_contains_preview_state"}});
            }
        }
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    if (database_error)
    {
        return *database_error;
    }
    if (artifact.schema_version > kCatalogSchemaVersion)
        return make_error(ErrorCode::kUnsupported, "Catalog backup schema is newer than this Ravo",
                          {{"path", std::string(path)},
                           {"schema_version", std::to_string(artifact.schema_version)},
                           {"reason", "newer_backup_catalog_schema"}});
    if (extra_schema_row || artifact.catalog_id.empty() ||
        artifact.schema_version < kCatalogRecoveryMinimumSchemaVersion || artifact.revision < 0 ||
        std::any_of(
            artifact.recovery_states.begin(), artifact.recovery_states.end(),
            [](const AssetRecoveryState &state)
            {
                return state.asset_id.empty() || state.generation <= 0 ||
                       state.synchronized_generation != state.generation;
            }))
    {
        return make_error(
            ErrorCode::kValidation, "Catalog backup database identity is invalid",
            {{"path", std::string(path)}, {"reason", "backup_catalog_identity_invalid"}});
    }
    return artifact;
}

[[nodiscard]] Result<void> copy_database_snapshot(const std::string_view source_path,
                                                  const std::string_view output_path,
                                                  const CancellationToken &cancellation)
{
    constexpr qint64 kChunkBytes = 1024 * 1024;
    QFile source(qstring_from_utf8(source_path));
    if (!source.open(QIODevice::ReadOnly))
        return make_error(ErrorCode::kIo, "Unable to open catalog database for backup",
                          {{"detail", utf8_from_qstring(source.errorString())},
                           {"path", std::string(source_path)},
                           {"reason", "backup_database_source_open_failed"}});
    QSaveFile output(qstring_from_utf8(output_path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return make_error(ErrorCode::kIo, "Unable to create backup database temporary",
                          {{"detail", utf8_from_qstring(output.errorString())},
                           {"path", std::string(output_path)},
                           {"reason", "backup_database_temporary_open_failed"}});
    while (!source.atEnd())
    {
        auto active = cancellation.check();
        if (!active)
        {
            output.cancelWriting();
            return active.error();
        }
        const auto bytes = source.read(kChunkBytes);
        if (bytes.isEmpty() && !source.atEnd())
        {
            output.cancelWriting();
            return make_error(ErrorCode::kIo, "Unable to read catalog database snapshot",
                              {{"detail", utf8_from_qstring(source.errorString())},
                               {"path", std::string(source_path)},
                               {"reason", "backup_database_source_read_failed"}});
        }
        if (output.write(bytes) != bytes.size())
        {
            output.cancelWriting();
            return make_error(ErrorCode::kIo, "Unable to write catalog database snapshot",
                              {{"detail", utf8_from_qstring(output.errorString())},
                               {"path", std::string(output_path)},
                               {"reason", "backup_database_temporary_write_failed"}});
        }
    }
    auto active = cancellation.check();
    if (!active)
    {
        output.cancelWriting();
        return active.error();
    }
    if (!output.commit())
        return make_error(ErrorCode::kIo, "Unable to publish catalog database snapshot",
                          {{"detail", utf8_from_qstring(output.errorString())},
                           {"path", std::string(output_path)},
                           {"reason", "backup_database_publish_failed"}});
    return {};
}

[[nodiscard]] Result<void> strip_backup_preview_rows(const std::string_view path,
                                                     const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    const auto connection = next_connection_name();
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(qstring_from_utf8(path));
    if (!database.open())
    {
        const auto detail = utf8_from_qstring(database.lastError().text());
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return make_error(ErrorCode::kIo, "Unable to open backup database for cache pruning",
                          {{"path", std::string(path)},
                           {"detail", detail},
                           {"reason", "backup_cache_prune_open_failed"}});
    }
    std::optional<TaskError> failure;
    if (!database.transaction())
    {
        failure = make_error(ErrorCode::kIo, "Unable to begin backup cache-pruning transaction",
                             {{"path", std::string(path)},
                              {"detail", utf8_from_qstring(database.lastError().text())},
                              {"reason", "backup_cache_prune_begin_failed"}});
    }
    if (!failure)
    {
        QSqlQuery remove(database);
        if (!remove.exec(QStringLiteral("DELETE FROM preview")))
            failure = make_error(ErrorCode::kIo, "Unable to prune previews from backup database",
                                 {{"path", std::string(path)},
                                  {"detail", utf8_from_qstring(remove.lastError().text())},
                                  {"reason", "backup_cache_prune_failed"}});
    }
    if (failure)
    {
        static_cast<void>(database.rollback());
    }
    else if (!database.commit())
    {
        failure = make_error(ErrorCode::kIo, "Unable to commit backup cache-pruning transaction",
                             {{"path", std::string(path)},
                              {"detail", utf8_from_qstring(database.lastError().text())},
                              {"reason", "backup_cache_prune_commit_failed"}});
        static_cast<void>(database.rollback());
    }
    if (!failure)
    {
        QSqlQuery journal(database);
        if (!journal.exec(QStringLiteral("PRAGMA journal_mode = DELETE")) || !journal.next() ||
            journal.value(0).toString().compare(QStringLiteral("delete"), Qt::CaseInsensitive) != 0)
            failure = make_error(ErrorCode::kIo, "Unable to make backup database self-contained",
                                 {{"path", std::string(path)},
                                  {"detail", utf8_from_qstring(journal.lastError().text())},
                                  {"reason", "backup_journal_mode_failed"}});
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    static_cast<void>(QFile::remove(qstring_from_utf8(std::string(path) + "-wal")));
    static_cast<void>(QFile::remove(qstring_from_utf8(std::string(path) + "-shm")));
    if (failure)
        return *failure;
    active = cancellation.check();
    if (!active)
        return active.error();
    return {};
}

const char *const kAssetSelect =
    "SELECT id, normalized_uri, media_type, size_bytes, mtime_unix_ms, content_fingerprint, "
    "width, height, import_state, error_code, error_message, created_unix_ms, rating, "
    "color_label, rejected, "
    "EXISTS(SELECT 1 FROM asset_recipe WHERE asset_id = asset.id), "
    "version_ordinal, source_asset_id FROM asset";

const char *const kAssetPageSelect =
    "SELECT a.id, a.normalized_uri, a.media_type, a.size_bytes, a.mtime_unix_ms, "
    "a.content_fingerprint, a.width, a.height, a.import_state, a.error_code, a.error_message, "
    "a.created_unix_ms, a.rating, a.color_label, a.rejected, "
    "EXISTS(SELECT 1 FROM asset_recipe r WHERE r.asset_id = a.id), "
    "a.version_ordinal, a.source_asset_id "
    "FROM asset a LEFT JOIN asset_metadata m ON m.asset_id = a.id";

const char *const kPreviewSelect =
    "SELECT asset_id, contract_version, cache_key, width, height, state, cache_relpath, "
    "last_success_unix_ms FROM preview";

} // namespace sqlite_internal

using namespace sqlite_internal;

bool SqliteCatalogRepository::Impl::consume_import_failure(
    const testing::SqliteImportFailure expected) noexcept
{
    if (import_failure != expected)
        return false;
    import_failure = testing::SqliteImportFailure::kNone;
    return true;
}

TaskError SqliteCatalogRepository::Impl::abort_transaction(TaskError primary)
{
    const bool inject_rollback_failure =
        consume_import_failure(testing::SqliteImportFailure::kRollback);
    bool prepared_injected_failure = false;
    if (inject_rollback_failure)
    {
        QSqlQuery rollback(database);
        prepared_injected_failure = rollback.exec(QStringLiteral("ROLLBACK"));
    }
    const bool rolled_back = database.rollback();
    if (!rolled_back)
    {
        primary.context.insert_or_assign("rollback_failed", "true");
        primary.context.insert_or_assign(
            "rollback_error", prepared_injected_failure ?
                                  "injected_import_rollback" :
                                  utf8_from_qstring(database.lastError().text().left(128)));
    }
    return primary;
}

bool SqliteCatalogRepository::Impl::consume_recovery_failure(
    const testing::SqliteRecoveryFailure expected) noexcept
{
    if (recovery_failure != expected)
        return false;
    recovery_failure = testing::SqliteRecoveryFailure::kNone;
    return true;
}

bool SqliteCatalogRepository::Impl::consume_folder_relink_failure(
    const testing::SqliteFolderRelinkFailure expected) noexcept
{
    if (folder_relink_failure != expected)
        return false;
    folder_relink_failure = testing::SqliteFolderRelinkFailure::kNone;
    return true;
}

Result<void> SqliteCatalogRepository::Impl::exec(const QString &sql, const std::string_view action)
{
    QSqlQuery query(database);
    if (!query.exec(sql))
        return map_sql_error(query, action);
    return {};
}

Result<void> SqliteCatalogRepository::Impl::repair_v5_capture_columns()
{
    auto columns = asset_metadata_columns(database);
    if (!columns)
        return columns.error();
    std::vector<std::string> missing;
    for (const auto &[name, type] : kV5CaptureColumns)
    {
        if (!columns.value().contains(name))
            missing.emplace_back(std::string("ALTER TABLE asset_metadata ADD COLUMN ") + name +
                                 " " + type);
    }
    const bool copy_signed_altitude = columns.value().contains("gps_altitude_mm") &&
                                      (!columns.value().contains("gps_altitude_magnitude_mm") ||
                                       !columns.value().contains("gps_altitude_ref"));
    if (missing.empty() && !copy_signed_altitude)
        return {};
    if (!database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start catalog capture-column repair",
                          {{"qt_error", utf8_from_qstring(database.lastError().text())}});
    }
    for (const auto &sql : missing)
    {
        auto added = exec(QString::fromStdString(sql), "repair_v5_capture_fields");
        if (!added)
            return abort_transaction(added.error());
    }
    if (copy_signed_altitude)
    {
        auto copied = exec(
            QStringLiteral(
                "UPDATE asset_metadata SET "
                "gps_altitude_magnitude_mm = CASE "
                "WHEN gps_altitude_mm IS NULL THEN NULL "
                "WHEN gps_altitude_mm >= 0 AND gps_altitude_mm <= 100000000 THEN gps_altitude_mm "
                "WHEN gps_altitude_mm < 0 AND -gps_altitude_mm <= 12000000 THEN -gps_altitude_mm "
                "ELSE NULL END, "
                "gps_altitude_ref = CASE "
                "WHEN gps_altitude_mm IS NULL THEN NULL "
                "WHEN gps_altitude_mm >= 0 AND gps_altitude_mm <= 100000000 THEN 0 "
                "WHEN gps_altitude_mm < 0 AND -gps_altitude_mm <= 12000000 THEN 1 "
                "ELSE NULL END "
                "WHERE gps_altitude_magnitude_mm IS NULL AND gps_altitude_ref IS NULL"),
            "repair_v5_signed_altitude");
        if (!copied)
            return abort_transaction(copied.error());
    }
    if (!database.commit())
    {
        return abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit catalog capture-column repair",
                       {{"qt_error", utf8_from_qstring(database.lastError().text())}}));
    }
    return {};
}

} // namespace ravo
