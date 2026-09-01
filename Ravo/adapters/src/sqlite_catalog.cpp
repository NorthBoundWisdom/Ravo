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

namespace ravo
{
namespace
{

const char *kSchemaStatements[] = {
    "PRAGMA foreign_keys = ON",
    "CREATE TABLE schema_info ("
    "  id INTEGER PRIMARY KEY CHECK (id = 1),"
    "  schema_version INTEGER NOT NULL,"
    "  catalog_id TEXT NOT NULL,"
    "  revision INTEGER NOT NULL,"
    "  created_unix_ms INTEGER NOT NULL,"
    "  migrated_unix_ms INTEGER NOT NULL"
    ")",
    "CREATE TABLE catalog_folder ("
    "  id TEXT PRIMARY KEY,"
    "  uri TEXT NOT NULL UNIQUE,"
    "  created_unix_ms INTEGER NOT NULL"
    ")",
    "CREATE TABLE asset ("
    "  id TEXT PRIMARY KEY,"
    "  normalized_uri TEXT NOT NULL UNIQUE,"
    "  display_name TEXT NOT NULL,"
    "  folder_uri TEXT NOT NULL,"
    "  folder_id TEXT NOT NULL REFERENCES catalog_folder(id),"
    "  media_type TEXT NOT NULL,"
    "  size_bytes INTEGER NOT NULL,"
    "  mtime_unix_ms INTEGER NOT NULL,"
    "  content_fingerprint TEXT,"
    "  width INTEGER,"
    "  height INTEGER,"
    "  import_state TEXT NOT NULL,"
    "  error_code TEXT,"
    "  error_message TEXT,"
    "  created_unix_ms INTEGER NOT NULL,"
    "  rating INTEGER NOT NULL DEFAULT 0,"
    "  color_label TEXT NOT NULL DEFAULT 'none',"
    "  rejected INTEGER NOT NULL DEFAULT 0"
    ")",
    "CREATE TABLE preview ("
    "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
    "  contract_version INTEGER NOT NULL,"
    "  cache_key TEXT NOT NULL,"
    "  width INTEGER,"
    "  height INTEGER,"
    "  state TEXT NOT NULL,"
    "  cache_relpath TEXT,"
    "  last_success_unix_ms INTEGER"
    ")",
    "CREATE TABLE asset_recipe ("
    "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
    "  recipe_schema_version INTEGER NOT NULL,"
    "  recipe_json TEXT NOT NULL,"
    "  updated_unix_ms INTEGER NOT NULL"
    ")",
    "CREATE TABLE asset_tag ("
    "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
    "  name TEXT NOT NULL,"
    "  PRIMARY KEY (asset_id, name)"
    ")",
    "CREATE TABLE asset_metadata ("
    "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
    "  title TEXT,"
    "  description TEXT,"
    "  creator TEXT,"
    "  copyright TEXT,"
    "  camera_make TEXT,"
    "  camera_model TEXT,"
    "  iso REAL,"
    "  aperture REAL,"
    "  focal_length_mm REAL,"
    "  shutter_s REAL,"
    "  captured_unix_s INTEGER,"
    "  captured_local_exif TEXT,"
    "  captured_subsecond_digits TEXT,"
    "  captured_utc_offset_minutes INTEGER,"
    "  gps_latitude_e6 INTEGER,"
    "  gps_longitude_e6 INTEGER,"
    "  gps_altitude_magnitude_mm INTEGER,"
    "  gps_altitude_ref INTEGER"
    ")",
    "CREATE TABLE asset_recipe_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
    "  seq INTEGER NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  label TEXT,"
    "  recipe_json TEXT NOT NULL,"
    "  created_unix_ms INTEGER NOT NULL,"
    "  UNIQUE(asset_id, seq)"
    ")",
};

constexpr const char *kSchemaV7Indexes[] = {
    "CREATE INDEX IF NOT EXISTS asset_created_id_idx ON asset(created_unix_ms, id)",
    "CREATE INDEX IF NOT EXISTS asset_display_name_id_idx ON asset(display_name, id)",
    "CREATE INDEX IF NOT EXISTS asset_folder_uri_idx ON asset(folder_uri)",
    "CREATE INDEX IF NOT EXISTS asset_rating_id_idx ON asset(rating, id)",
    "CREATE INDEX IF NOT EXISTS asset_size_id_idx ON asset(size_bytes, id)",
    "CREATE INDEX IF NOT EXISTS asset_media_type_idx ON asset(media_type)",
    "CREATE INDEX IF NOT EXISTS asset_tag_name_idx ON asset_tag(name, asset_id)",
    ("CREATE INDEX IF NOT EXISTS asset_metadata_capture_idx ON "
     "asset_metadata(captured_unix_s, asset_id)"),
};

constexpr const char *kSchemaV8BackupPolicy[] = {
    "CREATE TABLE IF NOT EXISTS catalog_backup_policy ("
    "  id INTEGER PRIMARY KEY CHECK(id = 1),"
    "  enabled INTEGER NOT NULL DEFAULT 0,"
    "  destination_directory TEXT NOT NULL DEFAULT '',"
    "  interval_minutes INTEGER NOT NULL DEFAULT 1440,"
    "  retention_count INTEGER NOT NULL DEFAULT 7,"
    "  last_success_unix_ms INTEGER,"
    "  next_run_unix_ms INTEGER,"
    "  last_backup_bytes INTEGER NOT NULL DEFAULT 0,"
    "  last_error TEXT"
    ")",
    "INSERT OR IGNORE INTO catalog_backup_policy(id) VALUES (1)",
};

constexpr const char *kSchemaV9FolderIdentityIndex =
    "CREATE INDEX IF NOT EXISTS asset_folder_id_idx ON asset(folder_id, id)";

constexpr const char *kSchemaV10Statements[] = {
    "CREATE TABLE IF NOT EXISTS library_set ("
    "  id TEXT PRIMARY KEY,"
    "  kind TEXT NOT NULL CHECK(kind IN ('manual', 'smart')),"
    "  name TEXT NOT NULL UNIQUE,"
    "  query_json TEXT,"
    "  created_unix_ms INTEGER NOT NULL,"
    "  updated_unix_ms INTEGER NOT NULL,"
    "  CHECK((kind = 'manual' AND query_json IS NULL) OR "
    "        (kind = 'smart' AND query_json IS NOT NULL))"
    ")",
    "CREATE TABLE IF NOT EXISTS library_set_member ("
    "  set_id TEXT NOT NULL REFERENCES library_set(id) ON DELETE CASCADE,"
    "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
    "  added_unix_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (set_id, asset_id)"
    ")",
    "CREATE INDEX IF NOT EXISTS library_set_member_asset_idx ON library_set_member(asset_id, set_id)",
    "CREATE INDEX IF NOT EXISTS library_set_name_idx ON library_set(name)",
};

constexpr const char *kSchemaV4Statements[] = {
    "CREATE TABLE asset_tag ("
    "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
    "  name TEXT NOT NULL,"
    "  PRIMARY KEY (asset_id, name)"
    ")",
    "CREATE TABLE asset_metadata ("
    "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
    "  title TEXT,"
    "  description TEXT,"
    "  creator TEXT,"
    "  copyright TEXT,"
    "  camera_make TEXT,"
    "  camera_model TEXT,"
    "  iso REAL,"
    "  aperture REAL,"
    "  focal_length_mm REAL,"
    "  shutter_s REAL,"
    "  captured_unix_s INTEGER"
    ")",
    "CREATE TABLE asset_recipe_history ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
    "  seq INTEGER NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  label TEXT,"
    "  recipe_json TEXT NOT NULL,"
    "  created_unix_ms INTEGER NOT NULL,"
    "  UNIQUE(asset_id, seq)"
    ")",
};

constexpr const char *kSchemaV6Table =
    "CREATE TABLE asset_recovery_state ("
    "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
    "  generation INTEGER NOT NULL CHECK(generation > 0),"
    "  synchronized_generation INTEGER NOT NULL DEFAULT 0 "
    "    CHECK(synchronized_generation >= 0 AND synchronized_generation <= generation)"
    ")";

constexpr const char *kSchemaV6AssetUpdateTrigger =
    "CREATE TRIGGER asset_recovery_update AFTER UPDATE OF normalized_uri, media_type, size_bytes, "
    "mtime_unix_ms, content_fingerprint, width, height, import_state, error_code, error_message, "
    "created_unix_ms, rating, color_label, rejected ON asset BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.id; "
    "END";

// Generation is catalog-owned durable state. Every durable per-asset write marks
// the derived recovery sidecar pending, while preview/cache writes intentionally
// do not participate because they are rebuildable.
constexpr const char *kSchemaV6Triggers[] = {
    "CREATE TRIGGER asset_recovery_insert AFTER INSERT ON asset BEGIN "
    "  INSERT INTO asset_recovery_state(asset_id, generation, synchronized_generation) "
    "  VALUES (NEW.id, 1, 0); "
    "END",
    kSchemaV6AssetUpdateTrigger,
    "CREATE TRIGGER asset_recipe_recovery_insert AFTER INSERT ON asset_recipe BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_recipe_recovery_update AFTER UPDATE ON asset_recipe BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_recipe_recovery_delete AFTER DELETE ON asset_recipe BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = OLD.asset_id; "
    "END",
    "CREATE TRIGGER asset_tag_recovery_insert AFTER INSERT ON asset_tag BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_tag_recovery_delete AFTER DELETE ON asset_tag BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = OLD.asset_id; "
    "END",
    "CREATE TRIGGER asset_metadata_recovery_insert AFTER INSERT ON asset_metadata BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_metadata_recovery_update AFTER UPDATE ON asset_metadata BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_metadata_recovery_delete AFTER DELETE ON asset_metadata BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = OLD.asset_id; "
    "END",
    "CREATE TRIGGER asset_history_recovery_insert AFTER INSERT ON asset_recipe_history BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_history_recovery_update AFTER UPDATE ON asset_recipe_history BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.asset_id; "
    "END",
    "CREATE TRIGGER asset_history_recovery_delete AFTER DELETE ON asset_recipe_history BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = OLD.asset_id; "
    "END",
};

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
    const auto add = [&predicates, &bindings](QString predicate,
                                              std::initializer_list<QVariant> values)
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
    const auto add_range = [&predicates, &bindings](const QString &column,
                                                    const LibraryNumericRange &range)
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
        count.prepare(
            QStringLiteral("SELECT COUNT(*) FROM library_set_member WHERE set_id = ?"));
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
                       "gps_altitude_magnitude_mm, gps_altitude_ref FROM asset_metadata "
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

[[nodiscard]] AssetRecoveryState read_recovery_state(const QSqlQuery &query,
                                                     const int first_column = 0)
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

constexpr const char *kAssetSelect =
    "SELECT id, normalized_uri, media_type, size_bytes, mtime_unix_ms, content_fingerprint, "
    "width, height, import_state, error_code, error_message, created_unix_ms, rating, "
    "color_label, rejected, "
    "EXISTS(SELECT 1 FROM asset_recipe WHERE asset_id = asset.id) FROM asset";

constexpr const char *kAssetPageSelect =
    "SELECT a.id, a.normalized_uri, a.media_type, a.size_bytes, a.mtime_unix_ms, "
    "a.content_fingerprint, a.width, a.height, a.import_state, a.error_code, a.error_message, "
    "a.created_unix_ms, a.rating, a.color_label, a.rejected, "
    "EXISTS(SELECT 1 FROM asset_recipe r WHERE r.asset_id = a.id) "
    "FROM asset a LEFT JOIN asset_metadata m ON m.asset_id = a.id";

constexpr const char *kPreviewSelect =
    "SELECT asset_id, contract_version, cache_key, width, height, state, cache_relpath, "
    "last_success_unix_ms FROM preview";

} // namespace

struct SqliteCatalogRepository::Impl
{
    QString connection_name;
    QSqlDatabase database;
    std::string database_path;
    CatalogSnapshot snapshot;
    testing::SqliteImportFailure import_failure = testing::SqliteImportFailure::kNone;
    testing::SqliteRecoveryFailure recovery_failure = testing::SqliteRecoveryFailure::kNone;
    testing::SqliteFolderRelinkFailure folder_relink_failure =
        testing::SqliteFolderRelinkFailure::kNone;

    [[nodiscard]] bool consume_import_failure(const testing::SqliteImportFailure expected) noexcept
    {
        if (import_failure != expected)
        {
            return false;
        }
        import_failure = testing::SqliteImportFailure::kNone;
        return true;
    }

    [[nodiscard]] TaskError abort_transaction(TaskError primary)
    {
        const bool inject_rollback_failure =
            consume_import_failure(testing::SqliteImportFailure::kRollback);
        bool prepared_injected_failure = false;
        if (inject_rollback_failure)
        {
            // End the real transaction first, then make the Qt transaction API
            // take its genuine no-active-transaction failure branch. This keeps
            // the database reusable while still exercising rollback-failure
            // context instead of merely pretending rollback failed.
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

    [[nodiscard]] bool
    consume_recovery_failure(const testing::SqliteRecoveryFailure expected) noexcept
    {
        if (recovery_failure != expected)
            return false;
        recovery_failure = testing::SqliteRecoveryFailure::kNone;
        return true;
    }

    [[nodiscard]] bool
    consume_folder_relink_failure(const testing::SqliteFolderRelinkFailure expected) noexcept
    {
        if (folder_relink_failure != expected)
            return false;
        folder_relink_failure = testing::SqliteFolderRelinkFailure::kNone;
        return true;
    }

    [[nodiscard]] Result<void> exec(const QString &sql, const std::string_view action)
    {
        QSqlQuery query(database);
        if (!query.exec(sql))
        {
            return map_sql_error(query, action);
        }
        return {};
    }

    // Catalogs written before ADR-0040 claimed schema v5 with signed
    // gps_altitude_mm. Repair the on-disk layout without bumping the schema.
    [[nodiscard]] Result<void> repair_v5_capture_columns()
    {
        auto columns = asset_metadata_columns(database);
        if (!columns)
        {
            return columns.error();
        }
        std::vector<std::string> missing;
        for (const auto &[name, type] : kV5CaptureColumns)
        {
            if (!columns.value().contains(name))
            {
                missing.emplace_back(std::string("ALTER TABLE asset_metadata ADD COLUMN ") + name +
                                     " " + type);
            }
        }
        const bool copy_signed_altitude = columns.value().contains("gps_altitude_mm") &&
                                          (!columns.value().contains("gps_altitude_magnitude_mm") ||
                                           !columns.value().contains("gps_altitude_ref"));
        if (missing.empty() && !copy_signed_altitude)
        {
            return {};
        }
        if (!database.transaction())
        {
            return make_error(ErrorCode::kIo, "Unable to start catalog capture-column repair",
                              {{"qt_error", utf8_from_qstring(database.lastError().text())}});
        }
        for (const auto &sql : missing)
        {
            auto added = exec(QString::fromStdString(sql), "repair_v5_capture_fields");
            if (!added)
            {
                return abort_transaction(added.error());
            }
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
            {
                return abort_transaction(copied.error());
            }
        }
        if (!database.commit())
        {
            return abort_transaction(
                make_error(ErrorCode::kIo, "Unable to commit catalog capture-column repair",
                           {{"qt_error", utf8_from_qstring(database.lastError().text())}}));
        }
        return {};
    }
};

void testing::SqliteCatalogTestControl::inject(SqliteCatalogRepository &repository,
                                               const SqliteImportFailure failure) noexcept
{
    if (repository.impl_ != nullptr)
    {
        repository.impl_->import_failure = failure;
    }
}

void testing::SqliteCatalogTestControl::inject_recovery(
    SqliteCatalogRepository &repository, const SqliteRecoveryFailure failure) noexcept
{
    if (repository.impl_ != nullptr)
        repository.impl_->recovery_failure = failure;
}

void testing::SqliteCatalogTestControl::inject_folder_relink(
    SqliteCatalogRepository &repository, const SqliteFolderRelinkFailure failure) noexcept
{
    if (repository.impl_ != nullptr)
        repository.impl_->folder_relink_failure = failure;
}

SqliteCatalogRepository::SqliteCatalogRepository(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SqliteCatalogRepository::~SqliteCatalogRepository()
{
    static_cast<void>(close());
}

Result<void> SqliteCatalogRepository::close()
{
    if (impl_ == nullptr)
    {
        return {};
    }
    if (impl_->database.isValid())
    {
        if (impl_->database.isOpen())
        {
            impl_->database.close();
        }
        impl_->database = QSqlDatabase();
    }
    if (!impl_->connection_name.isEmpty() && QSqlDatabase::contains(impl_->connection_name))
    {
        QSqlDatabase::removeDatabase(impl_->connection_name);
    }
    impl_.reset();
    return {};
}

Result<std::unique_ptr<SqliteCatalogRepository::Impl>>
SqliteCatalogRepository::open_database(const std::string_view database_path, const bool create)
{
    if (database_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog database path must not be empty");
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
    {
        return make_error(ErrorCode::kInternal, "The QSQLITE driver is not available");
    }

    const QString qt_path = qstring_from_utf8(database_path);
    const bool exists = QFileInfo::exists(qt_path);
    if (create && exists)
    {
        return make_error(ErrorCode::kConflict, "Catalog database already exists",
                          {{"path", std::string(database_path)}});
    }
    if (!create && !exists)
    {
        return make_error(ErrorCode::kNotFound, "Catalog database does not exist",
                          {{"path", std::string(database_path)}});
    }

    auto impl = std::make_unique<SqliteCatalogRepository::Impl>();
    impl->connection_name = next_connection_name();
    impl->database_path = std::string(database_path);
    impl->database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), impl->connection_name);
    impl->database.setDatabaseName(qt_path);
    if (!impl->database.open())
    {
        const auto message = utf8_from_qstring(impl->database.lastError().text());
        impl->database = QSqlDatabase();
        QSqlDatabase::removeDatabase(impl->connection_name);
        return make_error(ErrorCode::kIo, "Unable to open catalog database",
                          {{"path", std::string(database_path)}, {"qt_error", message}});
    }

    QSqlQuery pragma(impl->database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON")))
    {
        return map_sql_error(pragma, "enable_foreign_keys");
    }
    if (!pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL")))
    {
        return map_sql_error(pragma, "enable_wal");
    }
    if (!pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL")))
    {
        return map_sql_error(pragma, "set_synchronous");
    }
    if (!pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000")))
    {
        return map_sql_error(pragma, "set_busy_timeout");
    }
    return impl;
}

Result<std::unique_ptr<SqliteCatalogRepository>>
SqliteCatalogRepository::create(const std::string_view database_path)
{
    auto opened = open_database(database_path, true);
    if (!opened)
    {
        return opened.error();
    }
    auto impl = std::move(opened).value();
    if (!impl->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start catalog schema transaction",
                          {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}});
    }
    for (const char *statement : kSchemaStatements)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_schema");
        if (!created)
        {
            return impl->abort_transaction(created.error());
        }
    }
    auto recovery_table = impl->exec(QString::fromUtf8(kSchemaV6Table), "create_recovery_state");
    if (!recovery_table)
    {
        return impl->abort_transaction(recovery_table.error());
    }
    for (const char *statement : kSchemaV6Triggers)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_recovery_trigger");
        if (!created)
        {
            return impl->abort_transaction(created.error());
        }
    }
    for (const char *statement : kSchemaV7Indexes)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_library_index");
        if (!created)
            return impl->abort_transaction(created.error());
    }
    for (const char *statement : kSchemaV8BackupPolicy)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_backup_policy");
        if (!created)
            return impl->abort_transaction(created.error());
    }
    const auto folder_index =
        impl->exec(QString::fromUtf8(kSchemaV9FolderIdentityIndex), "create_folder_identity_index");
    if (!folder_index)
        return impl->abort_transaction(folder_index.error());
    for (const char *statement : kSchemaV10Statements)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_library_set");
        if (!created)
            return impl->abort_transaction(created.error());
    }

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    impl->snapshot.catalog_id = generate_catalog_id();
    impl->snapshot.database_path = impl->database_path;
    impl->snapshot.schema_version = kCatalogSchemaVersion;
    impl->snapshot.revision = 0;

    QSqlQuery insert(impl->database);
    insert.prepare(
        QStringLiteral("INSERT INTO schema_info(id, schema_version, catalog_id, revision, "
                       "created_unix_ms, migrated_unix_ms) VALUES (1, ?, ?, 0, ?, ?)"));
    insert.addBindValue(static_cast<qlonglong>(kCatalogSchemaVersion));
    insert.addBindValue(qstring_from_utf8(impl->snapshot.catalog_id));
    insert.addBindValue(static_cast<qlonglong>(now));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
    {
        return impl->abort_transaction(map_sql_error(insert, "insert_schema_info"));
    }
    if (!impl->database.commit())
    {
        return impl->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit catalog schema",
                       {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}}));
    }
    return std::unique_ptr<SqliteCatalogRepository>(new SqliteCatalogRepository(std::move(impl)));
}

Result<std::unique_ptr<SqliteCatalogRepository>>
SqliteCatalogRepository::open(const std::string_view database_path)
{
    auto opened = open_database(database_path, false);
    if (!opened)
    {
        return opened.error();
    }
    auto impl = std::move(opened).value();
    QSqlQuery query(impl->database);
    if (!query.exec(QStringLiteral(
            "SELECT schema_version, catalog_id, revision FROM schema_info WHERE id = 1")))
    {
        return map_sql_error(query, "read_schema_info");
    }
    if (!query.next())
    {
        return make_error(ErrorCode::kValidation, "Catalog is missing schema_info",
                          {{"path", std::string(database_path)}});
    }
    auto version = query.value(0).toLongLong();
    if (version > kCatalogSchemaVersion)
    {
        return make_error(
            ErrorCode::kUnsupported, "Catalog schema version is newer than this Ravo",
            {{"path", std::string(database_path)}, {"schema_version", std::to_string(version)}});
    }
    if (version < 1)
    {
        return make_error(
            ErrorCode::kValidation, "Catalog schema version is invalid",
            {{"path", std::string(database_path)}, {"schema_version", std::to_string(version)}});
    }
    if (version < kCatalogSchemaVersion)
    {
        if (!impl->database.transaction())
        {
            return make_error(ErrorCode::kIo, "Unable to start catalog migration transaction",
                              {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}});
        }
        if (version == 1)
        {
            const auto rating = impl->exec(
                QStringLiteral("ALTER TABLE asset ADD COLUMN rating INTEGER NOT NULL DEFAULT 0"),
                "migrate_v2_rating");
            const auto color = impl->exec(
                QStringLiteral(
                    "ALTER TABLE asset ADD COLUMN color_label TEXT NOT NULL DEFAULT 'none'"),
                "migrate_v2_color_label");
            const auto rejected = impl->exec(
                QStringLiteral("ALTER TABLE asset ADD COLUMN rejected INTEGER NOT NULL DEFAULT 0"),
                "migrate_v2_rejected");
            if (!rating || !color || !rejected)
            {
                return impl->abort_transaction(!rating ? rating.error() :
                                               !color  ? color.error() :
                                                         rejected.error());
            }
            version = 2;
        }
        if (version == 2)
        {
            const auto recipes = impl->exec(
                QStringLiteral("CREATE TABLE asset_recipe ("
                               "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                               "  recipe_schema_version INTEGER NOT NULL,"
                               "  recipe_json TEXT NOT NULL,"
                               "  updated_unix_ms INTEGER NOT NULL)"),
                "migrate_v3_asset_recipe");
            if (!recipes)
            {
                return impl->abort_transaction(recipes.error());
            }
            version = 3;
        }
        if (version == 3)
        {
            for (const char *sql : kSchemaV4Statements)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(sql), "migrate_v4_catalog_fields");
                if (!created)
                {
                    return impl->abort_transaction(created.error());
                }
            }
            version = 4;
        }
        if (version == 4)
        {
            static constexpr const char *kSchemaV5Columns[] = {
                "ALTER TABLE asset_metadata ADD COLUMN captured_local_exif TEXT",
                "ALTER TABLE asset_metadata ADD COLUMN captured_subsecond_digits TEXT",
                "ALTER TABLE asset_metadata ADD COLUMN captured_utc_offset_minutes INTEGER",
                "ALTER TABLE asset_metadata ADD COLUMN gps_latitude_e6 INTEGER",
                "ALTER TABLE asset_metadata ADD COLUMN gps_longitude_e6 INTEGER",
                "ALTER TABLE asset_metadata ADD COLUMN gps_altitude_magnitude_mm INTEGER",
                "ALTER TABLE asset_metadata ADD COLUMN gps_altitude_ref INTEGER",
            };
            for (const char *sql : kSchemaV5Columns)
            {
                const auto added = impl->exec(QString::fromUtf8(sql), "migrate_v5_capture_fields");
                if (!added)
                {
                    return impl->abort_transaction(added.error());
                }
            }
            version = 5;
        }
        if (version == 5)
        {
            auto recovery_table =
                impl->exec(QString::fromUtf8(kSchemaV6Table), "migrate_v6_recovery_state");
            if (!recovery_table)
            {
                return impl->abort_transaction(recovery_table.error());
            }
            auto initialized =
                impl->exec(QStringLiteral("INSERT INTO asset_recovery_state(asset_id, generation, "
                                          "synchronized_generation) SELECT id, 1, 0 FROM asset"),
                           "migrate_v6_recovery_assets");
            if (!initialized)
            {
                return impl->abort_transaction(initialized.error());
            }
            for (const char *statement : kSchemaV6Triggers)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(statement), "migrate_v6_recovery_trigger");
                if (!created)
                {
                    return impl->abort_transaction(created.error());
                }
            }
            version = 6;
        }
        if (version == 6)
        {
            auto columns = asset_columns(impl->database);
            if (!columns)
                return impl->abort_transaction(columns.error());
            if (!columns.value().contains("display_name"))
            {
                auto display_column = impl->exec(
                    QStringLiteral(
                        "ALTER TABLE asset ADD COLUMN display_name TEXT NOT NULL DEFAULT ''"),
                    "migrate_v7_display_name");
                if (!display_column)
                    return impl->abort_transaction(display_column.error());
            }
            if (!columns.value().contains("folder_uri"))
            {
                auto folder_column = impl->exec(
                    QStringLiteral(
                        "ALTER TABLE asset ADD COLUMN folder_uri TEXT NOT NULL DEFAULT ''"),
                    "migrate_v7_folder_uri");
                if (!folder_column)
                    return impl->abort_transaction(folder_column.error());
            }
            auto dropped_update_trigger =
                impl->exec(QStringLiteral("DROP TRIGGER IF EXISTS asset_recovery_update"),
                           "migrate_v7_drop_asset_recovery_update");
            if (!dropped_update_trigger)
                return impl->abort_transaction(dropped_update_trigger.error());
            auto recreated_update_trigger =
                impl->exec(QString::fromUtf8(kSchemaV6AssetUpdateTrigger),
                           "migrate_v7_create_asset_recovery_update");
            if (!recreated_update_trigger)
                return impl->abort_transaction(recreated_update_trigger.error());
            QSqlQuery assets(impl->database);
            if (!assets.exec(QStringLiteral("SELECT id, normalized_uri FROM asset ORDER BY id")))
                return impl->abort_transaction(map_sql_error(assets, "migrate_v7_read_assets"));
            QSqlQuery update_asset(impl->database);
            update_asset.prepare(
                QStringLiteral("UPDATE asset SET display_name = ?, folder_uri = ? WHERE id = ?"));
            while (assets.next())
            {
                const auto id = utf8_from_qstring(assets.value(0).toString());
                const auto uri = utf8_from_qstring(assets.value(1).toString());
                update_asset.bindValue(0, qstring_from_utf8(uri_display_name(uri)));
                update_asset.bindValue(1, qstring_from_utf8(uri_parent(uri)));
                update_asset.bindValue(2, qstring_from_utf8(id));
                if (!update_asset.exec())
                    return impl->abort_transaction(
                        map_sql_error(update_asset, "migrate_v7_index_asset"));
            }
            for (const char *statement : kSchemaV7Indexes)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(statement), "migrate_v7_library_index");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            version = 7;
        }
        if (version == 7)
        {
            for (const char *statement : kSchemaV8BackupPolicy)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(statement), "migrate_v8_backup_policy");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            version = 8;
        }
        if (version == 8)
        {
            auto folders =
                impl->exec(QStringLiteral("CREATE TABLE IF NOT EXISTS catalog_folder ("
                                          "id TEXT PRIMARY KEY, uri TEXT NOT NULL UNIQUE, "
                                          "created_unix_ms INTEGER NOT NULL)"),
                           "migrate_v9_folder_table");
            if (!folders)
                return impl->abort_transaction(folders.error());
            auto columns = asset_columns(impl->database);
            if (!columns)
                return impl->abort_transaction(columns.error());
            if (!columns.value().contains("folder_id"))
            {
                auto folder_column =
                    impl->exec(QStringLiteral("ALTER TABLE asset ADD COLUMN folder_id TEXT "
                                              "REFERENCES catalog_folder(id)"),
                               "migrate_v9_folder_column");
                if (!folder_column)
                    return impl->abort_transaction(folder_column.error());
            }
            QSqlQuery folder_uris(impl->database);
            if (!folder_uris.exec(QStringLiteral(
                    "SELECT folder_uri, MIN(created_unix_ms) FROM asset GROUP BY folder_uri "
                    "ORDER BY folder_uri")))
                return impl->abort_transaction(
                    map_sql_error(folder_uris, "migrate_v9_read_folders"));
            QSqlQuery insert_folder(impl->database);
            insert_folder.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO catalog_folder(id, uri, created_unix_ms) VALUES (?, ?, ?)"));
            QSqlQuery read_folder(impl->database);
            read_folder.prepare(QStringLiteral("SELECT id FROM catalog_folder WHERE uri = ?"));
            QSqlQuery assign_folder(impl->database);
            assign_folder.prepare(QStringLiteral(
                "UPDATE asset SET folder_id = ? WHERE folder_uri = ? AND folder_id IS NULL"));
            while (folder_uris.next())
            {
                const auto id = generate_folder_id();
                const auto uri = folder_uris.value(0).toString();
                insert_folder.bindValue(0, qstring_from_utf8(id));
                insert_folder.bindValue(1, uri);
                insert_folder.bindValue(2, folder_uris.value(1));
                if (!insert_folder.exec())
                    return impl->abort_transaction(
                        map_sql_error(insert_folder, "migrate_v9_insert_folder"));
                read_folder.bindValue(0, uri);
                if (!read_folder.exec() || !read_folder.next())
                    return impl->abort_transaction(
                        map_sql_error(read_folder, "migrate_v9_resolve_folder"));
                assign_folder.bindValue(0, read_folder.value(0));
                assign_folder.bindValue(1, uri);
                if (!assign_folder.exec())
                    return impl->abort_transaction(
                        map_sql_error(assign_folder, "migrate_v9_assign_folder"));
            }
            QSqlQuery unassigned(impl->database);
            if (!unassigned.exec(
                    QStringLiteral("SELECT COUNT(*) FROM asset WHERE folder_id IS NULL")) ||
                !unassigned.next())
                return impl->abort_transaction(
                    map_sql_error(unassigned, "migrate_v9_verify_folders"));
            if (unassigned.value(0).toLongLong() != 0)
                return impl->abort_transaction(make_error(
                    ErrorCode::kValidation, "Catalog folder migration left unassigned assets",
                    {{"reason", "unassigned_folder_identity"}}));
            auto folder_index = impl->exec(QString::fromUtf8(kSchemaV9FolderIdentityIndex),
                                           "migrate_v9_folder_index");
            if (!folder_index)
                return impl->abort_transaction(folder_index.error());
            version = 9;
        }
        if (version == 9)
        {
            for (const char *sql : kSchemaV10Statements)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(sql), "migrate_v10_library_set");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            version = 10;
        }
        if (version != kCatalogSchemaVersion)
        {
            return impl->abort_transaction(
                make_error(ErrorCode::kValidation, "Catalog schema version cannot be upgraded",
                           {{"path", std::string(database_path)},
                            {"schema_version", std::to_string(version)}}));
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        QSqlQuery update(impl->database);
        update.prepare(QStringLiteral(
            "UPDATE schema_info SET schema_version = ?, migrated_unix_ms = ? WHERE id = 1"));
        update.addBindValue(static_cast<qlonglong>(kCatalogSchemaVersion));
        update.addBindValue(static_cast<qlonglong>(now));
        if (!update.exec())
        {
            return impl->abort_transaction(map_sql_error(update, "migrate_schema_info"));
        }
        if (!impl->database.commit())
        {
            return impl->abort_transaction(
                make_error(ErrorCode::kIo, "Unable to commit catalog migration",
                           {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}}));
        }
    }
    auto repaired = impl->repair_v5_capture_columns();
    if (!repaired)
    {
        return repaired.error();
    }
    impl->snapshot.schema_version = kCatalogSchemaVersion;
    impl->snapshot.catalog_id = utf8_from_qstring(query.value(1).toString());
    impl->snapshot.revision = query.value(2).toLongLong();
    impl->snapshot.database_path = impl->database_path;
    return std::unique_ptr<SqliteCatalogRepository>(new SqliteCatalogRepository(std::move(impl)));
}

Result<CatalogSnapshot> SqliteCatalogRepository::snapshot() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !query.next())
    {
        return map_sql_error(query, "read_revision");
    }
    impl_->snapshot.revision = query.value(0).toLongLong();
    return impl_->snapshot;
}

Result<std::vector<AssetRecord>> SqliteCatalogRepository::list_assets() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QString(kAssetSelect) +
                    QStringLiteral(" ORDER BY created_unix_ms ASC, id ASC")))
    {
        return map_sql_error(query, "list_assets");
    }
    std::vector<AssetRecord> assets;
    while (query.next())
    {
        assets.push_back(read_asset(query));
    }
    auto attached = attach_asset_fields(impl_->database, assets);
    if (!attached)
    {
        return attached.error();
    }
    return assets;
}

Result<LibraryPage>
SqliteCatalogRepository::list_assets_page(const LibraryPageRequest &request) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto valid = validate_library_page_request(request);
    if (!valid)
        return valid.error();
    const auto started = std::chrono::steady_clock::now();
    QStringList predicates;
    QVariantList bindings;
    append_library_query_predicates(request.query, predicates, bindings);
    if (request.additional_query)
        append_library_query_predicates(*request.additional_query, predicates, bindings);

    const auto filter_where =
        predicates.empty() ? QString{} :
                             QStringLiteral(" WHERE ") + predicates.join(QStringLiteral(" AND "));
    const auto filter_bindings = bindings;
    std::size_t total = 0U;
    if (request.known_total)
    {
        total = *request.known_total;
    }
    else
    {
        QSqlQuery count(impl_->database);
        count.prepare(QStringLiteral("SELECT COUNT(*) FROM asset a LEFT JOIN asset_metadata m "
                                     "ON m.asset_id = a.id") +
                      filter_where);
        for (const auto &binding : filter_bindings)
            count.addBindValue(binding);
        if (!count.exec() || !count.next())
            return map_sql_error(count, "list_assets_page_count");
        const auto total_value = count.value(0).toLongLong();
        if (total_value < 0)
            return make_error(ErrorCode::kValidation, "Library page count is invalid",
                              {{"reason", "invalid_library_page_count"}});
        total = static_cast<std::size_t>(total_value);
    }
    const auto direction = request.query.sort_direction == SortDirection::kAscending ?
                               QStringLiteral(" ASC") :
                               QStringLiteral(" DESC");
    const auto comparison = request.query.sort_direction == SortDirection::kAscending ?
                                QStringLiteral(" > ") :
                                QStringLiteral(" < ");
    QStringList order;
    switch (request.query.sort_field)
    {
    case AssetSortField::kImportTime:
        order.push_back(QStringLiteral("a.created_unix_ms") + direction);
        break;
    case AssetSortField::kCaptureTime:
        order.push_back(
            QStringLiteral("CASE WHEN m.captured_unix_s IS NULL THEN 1 ELSE 0 END ASC"));
        order.push_back(QStringLiteral("m.captured_unix_s") + direction);
        break;
    case AssetSortField::kDisplayName:
        order.push_back(QStringLiteral("a.display_name COLLATE BINARY") + direction);
        break;
    case AssetSortField::kRating:
        order.push_back(QStringLiteral("a.rating") + direction);
        break;
    case AssetSortField::kFileSize:
        order.push_back(QStringLiteral("a.size_bytes") + direction);
        break;
    }
    order.push_back(QStringLiteral("a.id") + direction);

    if (request.after_asset_id)
    {
        QSqlQuery anchor(impl_->database);
        anchor.prepare(QStringLiteral(
            "SELECT a.created_unix_ms, a.display_name, a.rating, a.size_bytes, "
            "m.captured_unix_s FROM asset a LEFT JOIN asset_metadata m ON m.asset_id = a.id "
            "WHERE a.id = ?"));
        anchor.addBindValue(qstring_from_utf8(*request.after_asset_id));
        if (!anchor.exec())
            return map_sql_error(anchor, "list_assets_page_cursor");
        if (!anchor.next())
            return make_error(
                ErrorCode::kConflict, "Library page cursor no longer exists",
                {{"asset_id", *request.after_asset_id}, {"reason", "stale_library_page_cursor"}});
        const auto add_cursor = [&](const QString &column, const QVariant &value)
        {
            predicates.push_back(QLatin1Char('(') + column + comparison + QStringLiteral("? OR (") +
                                 column + QStringLiteral(" = ? AND a.id") + comparison +
                                 QStringLiteral("?))"));
            bindings.push_back(value);
            bindings.push_back(value);
            bindings.push_back(qstring_from_utf8(*request.after_asset_id));
        };
        switch (request.query.sort_field)
        {
        case AssetSortField::kImportTime:
            add_cursor(QStringLiteral("a.created_unix_ms"), anchor.value(0));
            break;
        case AssetSortField::kCaptureTime:
            if (anchor.isNull(4))
            {
                predicates.push_back(QStringLiteral("(m.captured_unix_s IS NULL AND a.id") +
                                     comparison + QStringLiteral("?)"));
                bindings.push_back(qstring_from_utf8(*request.after_asset_id));
            }
            else
            {
                predicates.push_back(
                    QStringLiteral("((m.captured_unix_s IS NOT NULL AND "
                                   "(m.captured_unix_s") +
                    comparison + QStringLiteral("? OR (m.captured_unix_s = ? AND a.id") +
                    comparison + QStringLiteral("?))) OR m.captured_unix_s IS NULL)"));
                bindings.push_back(anchor.value(4));
                bindings.push_back(anchor.value(4));
                bindings.push_back(qstring_from_utf8(*request.after_asset_id));
            }
            break;
        case AssetSortField::kDisplayName:
            add_cursor(QStringLiteral("a.display_name COLLATE BINARY"), anchor.value(1));
            break;
        case AssetSortField::kRating:
            add_cursor(QStringLiteral("a.rating"), anchor.value(2));
            break;
        case AssetSortField::kFileSize:
            add_cursor(QStringLiteral("a.size_bytes"), anchor.value(3));
            break;
        }
    }
    const auto page_where =
        predicates.empty() ? QString{} :
                             QStringLiteral(" WHERE ") + predicates.join(QStringLiteral(" AND "));
    QSqlQuery page_query(impl_->database);
    page_query.prepare(QString::fromUtf8(kAssetPageSelect) + page_where +
                       QStringLiteral(" ORDER BY ") + order.join(QStringLiteral(", ")) +
                       (request.after_asset_id ? QStringLiteral(" LIMIT ?") :
                                                 QStringLiteral(" LIMIT ? OFFSET ?")));
    for (const auto &binding : bindings)
        page_query.addBindValue(binding);
    page_query.addBindValue(static_cast<qlonglong>(request.limit));
    if (!request.after_asset_id)
        page_query.addBindValue(static_cast<qlonglong>(request.offset));
    if (!page_query.exec())
        return map_sql_error(page_query, "list_assets_page");
    std::vector<AssetRecord> assets;
    assets.reserve(request.limit);
    while (page_query.next())
        assets.push_back(read_asset(page_query));
    auto attached = attach_asset_fields(impl_->database, assets);
    if (!attached)
        return attached.error();

    LibraryPage page;
    page.assets = std::move(assets);
    page.offset = request.offset;
    page.total = total;
    page.has_more = !page.assets.empty() && page.offset < page.total &&
                    page.assets.size() < page.total - page.offset;
    if (page.has_more)
        page.next_cursor = page.assets.back().id;
    page.materialized_rows = page.assets.size();
    page.query_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    return page;
}

Result<std::vector<FolderRecord>> SqliteCatalogRepository::list_folders() const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery total_query(impl_->database);
    if (!total_query.exec(QStringLiteral("SELECT COUNT(*) FROM asset")) || !total_query.next())
        return map_sql_error(total_query, "list_folder_total");
    const auto total_value = total_query.value(0).toLongLong();
    if (total_value < 0 || total_value > std::numeric_limits<int>::max())
        return make_error(ErrorCode::kValidation, "Folder asset count is outside its bounds",
                          {{"reason", "invalid_folder_asset_count"}});
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral(
            "SELECT f.id, f.uri, COUNT(*) FROM catalog_folder f "
            "JOIN asset a ON a.folder_id = f.id GROUP BY f.id, f.uri ORDER BY f.uri")))
        return map_sql_error(query, "list_folders");
    std::vector<FolderAssetCount> direct;
    while (query.next())
    {
        const auto count = query.value(2).toLongLong();
        if (count <= 0 || count > std::numeric_limits<int>::max())
            return make_error(ErrorCode::kValidation, "Folder asset count is outside its bounds",
                              {{"reason", "invalid_folder_asset_count"}});
        direct.push_back({utf8_from_qstring(query.value(0).toString()),
                          utf8_from_qstring(query.value(1).toString()), static_cast<int>(count),
                          false});
    }
    return library_folders_from_counts(direct, static_cast<int>(total_value));
}

Result<std::optional<FolderRecord>>
SqliteCatalogRepository::find_folder_by_id(const std::string_view folder_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT f.id, f.uri, COUNT(a.id) FROM catalog_folder f "
        "LEFT JOIN asset a ON a.folder_id = f.id WHERE f.id = ? GROUP BY f.id, f.uri"));
    query.addBindValue(qstring_from_utf8(folder_id));
    if (!query.exec())
        return map_sql_error(query, "find_folder_by_id");
    if (!query.next())
        return std::optional<FolderRecord>{};
    const auto count = query.value(2).toLongLong();
    if (count < 0 || count > std::numeric_limits<int>::max())
        return make_error(ErrorCode::kValidation, "Folder asset count is outside its bounds",
                          {{"reason", "invalid_folder_asset_count"}});
    FolderRecord folder;
    folder.id = utf8_from_qstring(query.value(0).toString());
    folder.uri = utf8_from_qstring(query.value(1).toString());
    folder.display_name = uri_display_name(folder.uri);
    folder.asset_count = static_cast<int>(count);
    return std::optional<FolderRecord>{std::move(folder)};
}

Result<std::vector<AssetRecord>>
SqliteCatalogRepository::list_folder_assets(const std::string_view folder_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QString(kAssetSelect) + QStringLiteral(" WHERE folder_id = ? ORDER BY id ASC"));
    query.addBindValue(qstring_from_utf8(folder_id));
    if (!query.exec())
        return map_sql_error(query, "list_folder_assets");
    std::vector<AssetRecord> assets;
    while (query.next())
        assets.push_back(read_asset(query));
    auto attached = attach_asset_fields(impl_->database, assets);
    if (!attached)
        return attached.error();
    return assets;
}

Result<void> SqliteCatalogRepository::commit_folder_relink(const FolderRelinkCommit &relink,
                                                           const CancellationToken &cancellation)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (relink.folder_id.empty() || relink.expected_old_uri.empty() ||
        relink.replacement_uri.empty() || relink.assets.empty() ||
        relink.assets.size() > kImportBatchMaximumAssets)
        return make_error(ErrorCode::kInvalidArgument, "Folder relink commit is invalid",
                          {{"reason", "invalid_folder_relink_commit"}});
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (!impl_->database.transaction())
        return make_error(ErrorCode::kIo, "Unable to start folder relink transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});

    QSqlQuery folder(impl_->database);
    folder.prepare(QStringLiteral("SELECT uri FROM catalog_folder WHERE id = ?"));
    folder.addBindValue(qstring_from_utf8(relink.folder_id));
    if (!folder.exec())
        return impl_->abort_transaction(map_sql_error(folder, "relink_read_folder"));
    if (!folder.next())
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                   "Folder identity does not exist",
                                                   {{"folder_id", relink.folder_id}}));
    const auto current_uri = utf8_from_qstring(folder.value(0).toString());
    if (current_uri != relink.expected_old_uri)
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Folder path changed before relink",
                                                   {{"reason", "stale_folder_path"},
                                                    {"expected_uri", relink.expected_old_uri},
                                                    {"actual_uri", current_uri}}));

    std::map<std::string, const FolderRelinkAsset *, std::less<>> updates;
    for (const auto &asset : relink.assets)
    {
        if (asset.asset_id.empty() || asset.expected_old_uri.empty() ||
            asset.replacement_uri.empty() || !updates.emplace(asset.asset_id, &asset).second)
            return impl_->abort_transaction(
                make_error(ErrorCode::kInvalidArgument, "Folder relink asset set is invalid",
                           {{"reason", "invalid_folder_relink_assets"}}));
    }
    QSqlQuery current_assets(impl_->database);
    current_assets.prepare(
        QStringLiteral("SELECT id, normalized_uri FROM asset WHERE folder_id = ? ORDER BY id"));
    current_assets.addBindValue(qstring_from_utf8(relink.folder_id));
    if (!current_assets.exec())
        return impl_->abort_transaction(map_sql_error(current_assets, "relink_read_folder_assets"));
    std::size_t matched = 0U;
    while (current_assets.next())
    {
        const auto id = utf8_from_qstring(current_assets.value(0).toString());
        const auto found = updates.find(id);
        if (found == updates.end() || found->second->expected_old_uri !=
                                          utf8_from_qstring(current_assets.value(1).toString()))
            return impl_->abort_transaction(
                make_error(ErrorCode::kConflict, "Folder contents changed before relink",
                           {{"reason", "stale_folder_assets"}, {"asset_id", id}}));
        ++matched;
    }
    if (matched != updates.size())
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Folder contents changed before relink",
                                                   {{"reason", "stale_folder_asset_count"},
                                                    {"expected", std::to_string(updates.size())},
                                                    {"actual", std::to_string(matched)}}));

    QSqlQuery update_folder(impl_->database);
    update_folder.prepare(
        QStringLiteral("UPDATE catalog_folder SET uri = ? WHERE id = ? AND uri = ?"));
    update_folder.addBindValue(qstring_from_utf8(relink.replacement_uri));
    update_folder.addBindValue(qstring_from_utf8(relink.folder_id));
    update_folder.addBindValue(qstring_from_utf8(relink.expected_old_uri));
    if (!update_folder.exec())
    {
        const auto sql_text = update_folder.lastError().text();
        if (update_folder.lastError().nativeErrorCode() == QStringLiteral("2067") ||
            sql_text.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive))
            return impl_->abort_transaction(make_error(
                ErrorCode::kConflict, "Replacement folder is already cataloged",
                {{"reason", "folder_uri_conflict"}, {"replacement_uri", relink.replacement_uri}}));
        return impl_->abort_transaction(map_sql_error(update_folder, "relink_update_folder"));
    }
    if (update_folder.numRowsAffected() != 1)
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Folder path changed before relink",
                                                   {{"reason", "stale_folder_path"}}));
    if (impl_->consume_folder_relink_failure(
            testing::SqliteFolderRelinkFailure::kAfterFolderUpdate))
        return impl_->abort_transaction(make_error(ErrorCode::kIo, "Injected folder relink failure",
                                                   {{"reason", "injected_after_folder_update"}}));

    QSqlQuery update_asset(impl_->database);
    update_asset.prepare(
        QStringLiteral("UPDATE asset SET normalized_uri = ?, display_name = ?, folder_uri = ? "
                       "WHERE id = ? AND folder_id = ? AND normalized_uri = ?"));
    std::size_t updated_count = 0U;
    for (const auto &[id, asset] : updates)
    {
        active = cancellation.check();
        if (!active)
            return impl_->abort_transaction(active.error());
        update_asset.bindValue(0, qstring_from_utf8(asset->replacement_uri));
        update_asset.bindValue(1, qstring_from_utf8(uri_display_name(asset->replacement_uri)));
        update_asset.bindValue(2, qstring_from_utf8(relink.replacement_uri));
        update_asset.bindValue(3, qstring_from_utf8(id));
        update_asset.bindValue(4, qstring_from_utf8(relink.folder_id));
        update_asset.bindValue(5, qstring_from_utf8(asset->expected_old_uri));
        if (!update_asset.exec())
        {
            const auto sql_text = update_asset.lastError().text();
            if (update_asset.lastError().nativeErrorCode() == QStringLiteral("2067") ||
                sql_text.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive))
                return impl_->abort_transaction(
                    make_error(ErrorCode::kConflict, "Replacement asset URI is already cataloged",
                               {{"reason", "asset_uri_conflict"},
                                {"asset_id", id},
                                {"replacement_uri", asset->replacement_uri}}));
            return impl_->abort_transaction(map_sql_error(update_asset, "relink_update_asset"));
        }
        if (update_asset.numRowsAffected() != 1)
            return impl_->abort_transaction(
                make_error(ErrorCode::kConflict, "Folder contents changed before relink",
                           {{"reason", "stale_folder_assets"}, {"asset_id", id}}));
        ++updated_count;
        if (updated_count == 1U && impl_->consume_folder_relink_failure(
                                       testing::SqliteFolderRelinkFailure::kAfterFirstAssetUpdate))
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Injected folder relink failure",
                           {{"reason", "injected_after_first_asset_update"}}));
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return impl_->abort_transaction(map_sql_error(revision, "relink_revision"));
    if (impl_->consume_folder_relink_failure(testing::SqliteFolderRelinkFailure::kBeforeCommit))
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Injected folder relink failure",
                       {{"reason", "injected_before_folder_relink_commit"}}));
    active = cancellation.check();
    if (!active)
        return impl_->abort_transaction(active.error());
    if (!impl_->database.commit())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit folder relink transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    return {};
}

Result<std::optional<AssetRecord>>
SqliteCatalogRepository::find_asset_by_id(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QString(kAssetSelect) + QStringLiteral(" WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "find_asset_by_id");
    }
    if (!query.next())
    {
        return std::optional<AssetRecord>{};
    }
    auto asset = read_asset(query);
    auto attached = attach_asset_fields(impl_->database, asset);
    if (!attached)
    {
        return attached.error();
    }
    return std::optional<AssetRecord>{std::move(asset)};
}

Result<std::optional<AssetRecord>>
SqliteCatalogRepository::find_asset_by_uri(const std::string_view normalized_uri) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QString(kAssetSelect) + QStringLiteral(" WHERE normalized_uri = ?"));
    query.addBindValue(qstring_from_utf8(normalized_uri));
    if (!query.exec())
    {
        return map_sql_error(query, "find_asset_by_uri");
    }
    if (!query.next())
    {
        return std::optional<AssetRecord>{};
    }
    auto asset = read_asset(query);
    auto attached = attach_asset_fields(impl_->database, asset);
    if (!attached)
    {
        return attached.error();
    }
    return std::optional<AssetRecord>{std::move(asset)};
}

Result<void> SqliteCatalogRepository::insert_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    const auto folder_uri = uri_parent(asset.normalized_uri);
    QSqlQuery insert_folder(impl_->database);
    insert_folder.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO catalog_folder(id, uri, created_unix_ms) VALUES (?, ?, ?)"));
    insert_folder.addBindValue(qstring_from_utf8(generate_folder_id()));
    insert_folder.addBindValue(qstring_from_utf8(folder_uri));
    insert_folder.addBindValue(static_cast<qlonglong>(asset.created_unix_ms));
    if (!insert_folder.exec())
        return map_sql_error(insert_folder, "ensure_import_folder");
    QSqlQuery read_folder(impl_->database);
    read_folder.prepare(QStringLiteral("SELECT id FROM catalog_folder WHERE uri = ?"));
    read_folder.addBindValue(qstring_from_utf8(folder_uri));
    if (!read_folder.exec())
        return map_sql_error(read_folder, "read_import_folder");
    if (!read_folder.next())
        return make_error(ErrorCode::kConflict, "Unable to establish stable folder identity",
                          {{"reason", "folder_identity_conflict"}, {"uri", folder_uri}});
    const auto folder_id = read_folder.value(0).toString();

    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset(id, normalized_uri, display_name, folder_uri, folder_id, media_type, size_bytes, "
        "mtime_unix_ms, "
        "content_fingerprint, width, height, import_state, error_code, error_message, "
        "created_unix_ms, rating, color_label, rejected) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(qstring_from_utf8(asset.id));
    query.addBindValue(qstring_from_utf8(asset.normalized_uri));
    query.addBindValue(qstring_from_utf8(uri_display_name(asset.normalized_uri)));
    query.addBindValue(qstring_from_utf8(folder_uri));
    query.addBindValue(folder_id);
    query.addBindValue(qstring_from_utf8(asset.media_type));
    query.addBindValue(static_cast<qlonglong>(asset.size_bytes));
    query.addBindValue(static_cast<qlonglong>(asset.mtime_unix_ms));
    query.addBindValue(optional_string(asset.content_fingerprint));
    query.addBindValue(optional_u32(asset.width));
    query.addBindValue(optional_u32(asset.height));
    query.addBindValue(qstring_from_utf8(asset.import_state));
    query.addBindValue(optional_string(asset.error_code));
    query.addBindValue(optional_string(asset.error_message));
    query.addBindValue(static_cast<qlonglong>(asset.created_unix_ms));
    query.addBindValue(asset.review.rating);
    query.addBindValue(qstring_from_utf8(color_label_name(asset.review.color_label)));
    query.addBindValue(asset.review.rejected ? 1 : 0);
    if (!query.exec())
    {
        if (query.lastError().nativeErrorCode() == QStringLiteral("2067") ||
            query.lastError().text().contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive))
        {
            return make_error(ErrorCode::kConflict, "Asset URI already exists in this catalog",
                              {{"uri", asset.normalized_uri}});
        }
        return map_sql_error(query, "insert_asset");
    }
    return {};
}

Result<void> SqliteCatalogRepository::update_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "UPDATE asset SET media_type = ?, size_bytes = ?, mtime_unix_ms = ?, "
        "content_fingerprint = ?, width = ?, height = ?, import_state = ?, error_code = ?, "
        "error_message = ?, rating = ?, color_label = ?, rejected = ? WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(asset.media_type));
    query.addBindValue(static_cast<qlonglong>(asset.size_bytes));
    query.addBindValue(static_cast<qlonglong>(asset.mtime_unix_ms));
    query.addBindValue(optional_string(asset.content_fingerprint));
    query.addBindValue(optional_u32(asset.width));
    query.addBindValue(optional_u32(asset.height));
    query.addBindValue(qstring_from_utf8(asset.import_state));
    query.addBindValue(optional_string(asset.error_code));
    query.addBindValue(optional_string(asset.error_message));
    query.addBindValue(asset.review.rating);
    query.addBindValue(qstring_from_utf8(color_label_name(asset.review.color_label)));
    query.addBindValue(asset.review.rejected ? 1 : 0);
    query.addBindValue(qstring_from_utf8(asset.id));
    if (!query.exec())
    {
        return map_sql_error(query, "update_asset");
    }
    if (query.numRowsAffected() == 0)
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist", {{"asset_id", asset.id}});
    }
    return {};
}

Result<void> SqliteCatalogRepository::update_review(const std::string_view asset_id,
                                                    const ReviewState &review)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    auto valid = validate_rating(review.rating);
    if (!valid)
    {
        return valid.error();
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("UPDATE asset SET rating = ?, color_label = ?, rejected = ? WHERE id = ?"));
    query.addBindValue(review.rating);
    query.addBindValue(qstring_from_utf8(color_label_name(review.color_label)));
    query.addBindValue(review.rejected ? 1 : 0);
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "update_review");
    }
    if (query.numRowsAffected() == 0)
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return {};
}

Result<void> SqliteCatalogRepository::remove_asset(const std::string_view asset_id)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to begin asset removal transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("DELETE FROM asset WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return impl_->abort_transaction(map_sql_error(query, "remove_asset"));
    }
    if (query.numRowsAffected() == 0)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Asset does not exist",
                                                   {{"asset_id", std::string(asset_id)}}));
    }
    if (!query.exec(
            QStringLiteral("DELETE FROM catalog_folder WHERE NOT EXISTS "
                           "(SELECT 1 FROM asset WHERE asset.folder_id = catalog_folder.id)")))
        return impl_->abort_transaction(map_sql_error(query, "remove_empty_catalog_folders"));
    QSqlQuery revision(impl_->database);
    if (!revision.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return impl_->abort_transaction(map_sql_error(revision, "bump_remove_revision"));
    }
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
    {
        return impl_->abort_transaction(map_sql_error(revision, "read_remove_revision"));
    }
    const auto next_revision = revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit asset removal transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    return {};
}

Result<std::optional<PreviewRecord>>
SqliteCatalogRepository::find_preview(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QString(kPreviewSelect) + QStringLiteral(" WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "find_preview");
    }
    if (!query.next())
    {
        return std::optional<PreviewRecord>{};
    }
    return std::optional<PreviewRecord>{read_preview(query)};
}

Result<std::vector<PreviewRecord>> SqliteCatalogRepository::list_previews() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QString(kPreviewSelect)))
    {
        return map_sql_error(query, "list_previews");
    }
    std::vector<PreviewRecord> previews;
    while (query.next())
    {
        previews.push_back(read_preview(query));
    }
    return previews;
}

Result<std::vector<PreviewRecord>>
SqliteCatalogRepository::list_previews_for_assets(const std::vector<std::string> &asset_ids) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (asset_ids.empty())
        return std::vector<PreviewRecord>{};
    if (asset_ids.size() > kLibraryPageMaximumSize)
        return make_error(ErrorCode::kInvalidArgument, "Preview page exceeds its asset bound",
                          {{"reason", "preview_page_too_large"}});
    std::set<std::string, std::less<>> unique;
    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(asset_ids.size()));
    for (const auto &asset_id : asset_ids)
    {
        if (asset_id.empty() || !unique.insert(asset_id).second)
            return make_error(ErrorCode::kInvalidArgument,
                              "Preview page asset IDs must be non-empty and unique",
                              {{"asset_id", asset_id}, {"reason", "invalid_preview_page"}});
        placeholders.push_back(QStringLiteral("?"));
    }
    QSqlQuery query(impl_->database);
    query.prepare(QString(kPreviewSelect) + QStringLiteral(" WHERE asset_id IN (") +
                  placeholders.join(QLatin1Char(',')) + QStringLiteral(")"));
    for (const auto &asset_id : asset_ids)
        query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
        return map_sql_error(query, "list_previews_for_assets");
    std::vector<PreviewRecord> previews;
    previews.reserve(asset_ids.size());
    while (query.next())
        previews.push_back(read_preview(query));
    return previews;
}

Result<void> SqliteCatalogRepository::upsert_preview(const PreviewRecord &preview)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO preview(asset_id, contract_version, cache_key, width, height, state, "
        "cache_relpath, last_success_unix_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET contract_version = excluded.contract_version, "
        "cache_key = excluded.cache_key, width = excluded.width, height = excluded.height, "
        "state = excluded.state, cache_relpath = excluded.cache_relpath, "
        "last_success_unix_ms = excluded.last_success_unix_ms"));
    query.addBindValue(qstring_from_utf8(preview.asset_id));
    query.addBindValue(static_cast<qlonglong>(preview.contract_version));
    query.addBindValue(qstring_from_utf8(preview.cache_key));
    query.addBindValue(optional_u32(preview.width));
    query.addBindValue(optional_u32(preview.height));
    query.addBindValue(qstring_from_utf8(preview.state));
    query.addBindValue(optional_string(preview.cache_relpath));
    query.addBindValue(optional_i64(preview.last_success_unix_ms));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_preview");
    }
    return {};
}

Result<AssetRecoveryState>
SqliteCatalogRepository::recovery_state(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("SELECT asset_id, generation, synchronized_generation "
                                 "FROM asset_recovery_state WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "read_recovery_state");
    }
    if (!query.next())
    {
        return make_error(ErrorCode::kNotFound, "Asset recovery state does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return read_recovery_state(query);
}

Result<std::vector<AssetRecoveryState>> SqliteCatalogRepository::list_pending_recovery() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(
            QStringLiteral("SELECT asset_id, generation, synchronized_generation "
                           "FROM asset_recovery_state WHERE generation > synchronized_generation "
                           "ORDER BY asset_id ASC")))
    {
        return map_sql_error(query, "list_pending_recovery");
    }
    std::vector<AssetRecoveryState> states;
    while (query.next())
    {
        states.push_back(read_recovery_state(query));
    }
    return states;
}

Result<std::vector<AssetRecoveryState>> SqliteCatalogRepository::list_recovery_states() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral("SELECT asset_id, generation, synchronized_generation "
                                   "FROM asset_recovery_state ORDER BY asset_id ASC")))
    {
        return map_sql_error(query, "list_recovery_states");
    }
    std::vector<AssetRecoveryState> states;
    while (query.next())
    {
        states.push_back(read_recovery_state(query));
    }
    return states;
}

Result<AssetRecoverySnapshot>
SqliteCatalogRepository::load_recovery_snapshot(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to begin recovery snapshot transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }

    QSqlQuery identity(impl_->database);
    identity.prepare(
        QStringLiteral("SELECT recovery.asset_id, recovery.generation, "
                       "recovery.synchronized_generation, schema.catalog_id, schema.revision "
                       "FROM asset_recovery_state AS recovery CROSS JOIN schema_info AS schema "
                       "WHERE recovery.asset_id = ? AND schema.id = 1"));
    identity.addBindValue(qstring_from_utf8(asset_id));
    if (!identity.exec())
    {
        return impl_->abort_transaction(map_sql_error(identity, "read_recovery_identity"));
    }
    if (!identity.next())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                   "Asset recovery state does not exist",
                                                   {{"asset_id", std::string(asset_id)}}));
    }

    AssetRecoverySnapshot snapshot;
    snapshot.state = read_recovery_state(identity);
    snapshot.catalog_id = utf8_from_qstring(identity.value(3).toString());
    snapshot.catalog_revision = identity.value(4).toLongLong();

    auto asset = find_asset_by_id(asset_id);
    if (!asset)
    {
        return impl_->abort_transaction(asset.error());
    }
    if (!asset.value())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Asset does not exist",
                                                   {{"asset_id", std::string(asset_id)}}));
    }
    snapshot.asset = std::move(*asset.value());

    auto recipe = load_recipe_json(asset_id);
    if (!recipe)
    {
        return impl_->abort_transaction(recipe.error());
    }
    snapshot.recipe_json = std::move(recipe).value();

    QSqlQuery history(impl_->database);
    history.prepare(
        QStringLiteral("SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
                       "FROM asset_recipe_history WHERE asset_id = ? ORDER BY seq ASC, id ASC "
                       "LIMIT ?"));
    history.addBindValue(qstring_from_utf8(asset_id));
    history.addBindValue(static_cast<qlonglong>(kRecoveryHistoryMaximumEntries + 1U));
    if (!history.exec())
    {
        return impl_->abort_transaction(map_sql_error(history, "read_recovery_history"));
    }
    while (history.next())
    {
        snapshot.history.push_back(read_history(history));
    }
    if (snapshot.history.size() > kRecoveryHistoryMaximumEntries)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kValidation, "Asset history exceeds the recovery sidecar entry limit",
            {{"asset_id", std::string(asset_id)},
             {"limit", std::to_string(kRecoveryHistoryMaximumEntries)}}));
    }
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit recovery snapshot transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    return snapshot;
}

Result<AssetRecoveryState>
SqliteCatalogRepository::acknowledge_recovery(const std::string_view asset_id,
                                              const std::int64_t generation)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (generation <= 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Recovery generation must be positive",
                          {{"generation", std::to_string(generation)}});
    }
    if (impl_->consume_recovery_failure(testing::SqliteRecoveryFailure::kAcknowledge))
        return make_error(ErrorCode::kIo, "Unable to acknowledge recovery generation",
                          {{"action", "acknowledge_recovery"},
                           {"reason", "injected_recovery_acknowledgement_failure"}});
    QSqlQuery update(impl_->database);
    update.prepare(
        QStringLiteral("UPDATE asset_recovery_state SET synchronized_generation = ? "
                       "WHERE asset_id = ? AND generation = ? AND synchronized_generation < ?"));
    update.addBindValue(static_cast<qlonglong>(generation));
    update.addBindValue(qstring_from_utf8(asset_id));
    update.addBindValue(static_cast<qlonglong>(generation));
    update.addBindValue(static_cast<qlonglong>(generation));
    if (!update.exec())
    {
        return map_sql_error(update, "acknowledge_recovery");
    }
    auto current = recovery_state(asset_id);
    if (!current)
    {
        return current.error();
    }
    if (current.value().generation != generation)
    {
        return make_error(ErrorCode::kConflict,
                          "Asset changed while its recovery sidecar was published",
                          {{"asset_id", std::string(asset_id)},
                           {"expected_generation", std::to_string(generation)},
                           {"actual_generation", std::to_string(current.value().generation)},
                           {"reason", "recovery_generation_changed"}});
    }
    if (current.value().synchronized_generation != generation)
    {
        return make_error(ErrorCode::kConflict, "Recovery generation could not be acknowledged",
                          {{"asset_id", std::string(asset_id)},
                           {"generation", std::to_string(generation)},
                           {"reason", "recovery_generation_not_acknowledged"}});
    }
    return current;
}

Result<void> SqliteCatalogRepository::integrity_check() const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral("PRAGMA integrity_check")))
    {
        return map_sql_error(query, "catalog_integrity_check");
    }
    if (!query.next() || query.value(0).toString() != QStringLiteral("ok") || query.next())
    {
        return make_error(ErrorCode::kValidation, "Catalog failed its integrity check",
                          {{"path", impl_->database_path}, {"reason", "catalog_integrity_failed"}});
    }
    return {};
}

Result<CatalogDatabaseArtifact>
SqliteCatalogRepository::create_backup_database(const std::string_view output_path,
                                                const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (output_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog backup database path must not be empty");
    }
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (QFileInfo::exists(qstring_from_utf8(output_path)))
    {
        return make_error(
            ErrorCode::kConflict, "Catalog backup database destination already exists",
            {{"path", std::string(output_path)}, {"reason", "backup_database_conflict"}});
    }
    auto integrity = integrity_check();
    if (!integrity)
    {
        return integrity.error();
    }
    bool locked = false;
    for (int attempt = 0; attempt < 3 && !locked; ++attempt)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        QSqlQuery checkpoint(impl_->database);
        if (!checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) ||
            !checkpoint.next() || checkpoint.value(0).toInt() != 0)
            return make_error(ErrorCode::kIo, "Unable to checkpoint catalog before backup",
                              {{"detail", utf8_from_qstring(checkpoint.lastError().text())},
                               {"path", impl_->database_path},
                               {"reason", "backup_database_checkpoint_failed"}});
        QSqlQuery version_before(impl_->database);
        if (!version_before.exec(QStringLiteral("PRAGMA data_version")) || !version_before.next())
            return map_sql_error(version_before, "backup_database_version_before");
        const auto observed_version = version_before.value(0).toLongLong();
        QSqlQuery begin(impl_->database);
        if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE")))
            return map_sql_error(begin, "backup_database_begin_snapshot");
        QSqlQuery version_after(impl_->database);
        if (!version_after.exec(QStringLiteral("PRAGMA data_version")) || !version_after.next())
        {
            QSqlQuery rollback(impl_->database);
            static_cast<void>(rollback.exec(QStringLiteral("ROLLBACK")));
            return map_sql_error(version_after, "backup_database_version_after");
        }
        if (version_after.value(0).toLongLong() == observed_version)
        {
            locked = true;
            break;
        }
        QSqlQuery rollback(impl_->database);
        if (!rollback.exec(QStringLiteral("ROLLBACK")))
            return map_sql_error(rollback, "backup_database_retry_rollback");
    }
    if (!locked)
        return make_error(
            ErrorCode::kConflict, "Catalog changed repeatedly while backup snapshot was acquired",
            {{"path", impl_->database_path}, {"reason", "backup_database_snapshot_changed"}});
    auto copied = copy_database_snapshot(impl_->database_path, output_path, cancellation);
    QSqlQuery rollback(impl_->database);
    const bool unlocked = rollback.exec(QStringLiteral("ROLLBACK"));
    if (!copied)
    {
        if (QFileInfo::exists(qstring_from_utf8(output_path)))
            static_cast<void>(QFile::remove(qstring_from_utf8(output_path)));
        auto error = copied.error();
        if (!unlocked)
        {
            error.context.insert_or_assign("snapshot_unlock_failed", "true");
            error.context.insert_or_assign("snapshot_unlock_error",
                                           utf8_from_qstring(rollback.lastError().text()));
        }
        return error;
    }
    if (!unlocked)
    {
        static_cast<void>(QFile::remove(qstring_from_utf8(output_path)));
        return map_sql_error(rollback, "backup_database_end_snapshot");
    }
    auto pruned = strip_backup_preview_rows(output_path, cancellation);
    if (!pruned)
    {
        return pruned.error();
    }
    active = cancellation.check();
    if (!active)
    {
        if (!QFile::remove(qstring_from_utf8(output_path)))
        {
            auto error = active.error();
            error.context.insert_or_assign("cleanup_failed", "true");
            error.context.insert_or_assign("path", std::string(output_path));
            return error;
        }
        return active.error();
    }
    return inspect_backup_database(output_path, {}, cancellation);
}

Result<CatalogDatabaseArtifact>
SqliteCatalogRepository::verify_backup_database(const std::string_view backup_path,
                                                const std::string_view expected_sha256,
                                                const CancellationToken &cancellation) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    return inspect_backup_database(backup_path, expected_sha256, cancellation);
}

Result<CatalogDatabaseArtifact>
SqliteCatalogBackupVerifier::verify_backup_database(const std::string_view backup_path,
                                                    const std::string_view expected_sha256,
                                                    const CancellationToken &cancellation) const
{
    return inspect_backup_database(backup_path, expected_sha256, cancellation);
}

Result<CatalogBackupPolicy> SqliteCatalogRepository::backup_policy() const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral(
            "SELECT enabled, destination_directory, interval_minutes, retention_count, "
            "last_success_unix_ms, next_run_unix_ms, last_backup_bytes, last_error "
            "FROM catalog_backup_policy WHERE id = 1")))
        return map_sql_error(query, "read_backup_policy");
    if (!query.next() || query.value(6).toLongLong() < 0)
        return make_error(ErrorCode::kValidation, "Catalog backup policy row is invalid",
                          {{"reason", "invalid_catalog_backup_policy_row"}});
    CatalogBackupPolicy policy;
    policy.enabled = query.value(0).toInt() != 0;
    policy.destination_directory = utf8_from_qstring(query.value(1).toString());
    policy.interval_minutes = query.value(2).toLongLong();
    policy.retention_count = query.value(3).toInt();
    policy.last_success_unix_ms = i64_column(query, 4);
    policy.next_run_unix_ms = i64_column(query, 5);
    policy.last_backup_bytes = static_cast<std::uint64_t>(query.value(6).toLongLong());
    policy.last_error = string_column(query, 7);
    auto valid = validate_catalog_backup_policy(policy);
    if (!valid)
        return valid.error();
    return policy;
}

Result<void> SqliteCatalogRepository::save_backup_policy(const CatalogBackupPolicy &policy)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto valid = validate_catalog_backup_policy(policy);
    if (!valid)
        return valid.error();
    if (!impl_->database.transaction())
        return make_error(ErrorCode::kIo, "Unable to start backup policy transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    QSqlQuery update(impl_->database);
    update.prepare(
        QStringLiteral("UPDATE catalog_backup_policy SET enabled = ?, destination_directory = ?, "
                       "interval_minutes = ?, retention_count = ?, last_success_unix_ms = ?, "
                       "next_run_unix_ms = ?, last_backup_bytes = ?, last_error = ? WHERE id = 1"));
    update.addBindValue(policy.enabled ? 1 : 0);
    update.addBindValue(qstring_from_utf8(policy.destination_directory));
    update.addBindValue(static_cast<qlonglong>(policy.interval_minutes));
    update.addBindValue(policy.retention_count);
    update.addBindValue(optional_i64(policy.last_success_unix_ms));
    update.addBindValue(optional_i64(policy.next_run_unix_ms));
    update.addBindValue(static_cast<qlonglong>(policy.last_backup_bytes));
    update.addBindValue(optional_string(policy.last_error));
    if (!update.exec())
        return impl_->abort_transaction(map_sql_error(update, "save_backup_policy"));
    if (update.numRowsAffected() != 1)
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Catalog backup policy owner row is missing",
                       {{"reason", "missing_catalog_backup_policy_row"}}));
    QSqlQuery revision(impl_->database);
    if (!revision.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return impl_->abort_transaction(map_sql_error(revision, "save_backup_policy_revision"));
    if (!impl_->database.commit())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit backup policy",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    return {};
}

Result<CatalogSnapshot>
SqliteCatalogBackupVerifier::verify_restored_catalog(const std::string_view catalog_path,
                                                     const std::string_view expected_catalog_id,
                                                     const CancellationToken &cancellation) const
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto opened = SqliteCatalogRepository::open(catalog_path);
    if (!opened)
        return opened.error();
    auto repository = std::move(opened).value();
    auto snapshot = repository->snapshot();
    if (!snapshot)
    {
        static_cast<void>(repository->close());
        return snapshot.error();
    }
    if (snapshot.value().catalog_id != expected_catalog_id)
    {
        static_cast<void>(repository->close());
        return make_error(ErrorCode::kValidation, "Restored catalog identity does not match backup",
                          {{"actual_catalog_id", snapshot.value().catalog_id},
                           {"expected_catalog_id", std::string(expected_catalog_id)},
                           {"path", std::string(catalog_path)},
                           {"reason", "restored_catalog_identity_mismatch"}});
    }
    auto closed = repository->close();
    if (!closed)
        return closed.error();
    active = cancellation.check();
    if (!active)
        return active.error();
    return snapshot;
}

Result<std::optional<std::string>>
SqliteCatalogRepository::load_recipe_json(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("SELECT recipe_json FROM asset_recipe WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "load_recipe_json");
    }
    if (!query.next())
    {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{utf8_from_qstring(query.value(0).toString())};
}

Result<void> SqliteCatalogRepository::save_recipe_json(const std::string_view asset_id,
                                                       const std::int64_t recipe_schema_version,
                                                       const std::string_view recipe_json)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_recipe(asset_id, recipe_schema_version, recipe_json, updated_unix_ms) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET recipe_schema_version = excluded.recipe_schema_version, "
        "recipe_json = excluded.recipe_json, updated_unix_ms = excluded.updated_unix_ms"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(static_cast<qlonglong>(recipe_schema_version));
    query.addBindValue(qstring_from_utf8(recipe_json));
    query.addBindValue(static_cast<qlonglong>(now));
    if (!query.exec())
    {
        return map_sql_error(query, "save_recipe_json");
    }
    return {};
}

Result<void> SqliteCatalogRepository::clear_recipe(const std::string_view asset_id)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("DELETE FROM asset_recipe WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "clear_recipe");
    }
    return {};
}

Result<RecipeCommitResult> SqliteCatalogRepository::commit_recipe(
    const std::string_view asset_id, const std::int64_t recipe_schema_version,
    const std::optional<std::string_view> recipe_json, const std::string_view history_json,
    const RecipeHistoryWrite history_write,
    const std::optional<std::int64_t> discard_history_after_seq,
    const std::optional<std::int64_t> coalesce_history_id)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (discard_history_after_seq && *discard_history_after_seq < 0)
    {
        return make_error(ErrorCode::kValidation, "Recipe history cursor is invalid",
                          {{"seq", std::to_string(*discard_history_after_seq)}});
    }
    if (coalesce_history_id &&
        (*coalesce_history_id <= 0 || history_write != RecipeHistoryWrite::kAppendIfNew))
    {
        return make_error(ErrorCode::kValidation, "Recipe history coalesce request is invalid",
                          {{"history_id", std::to_string(*coalesce_history_id)}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start recipe transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }

    if (discard_history_after_seq)
    {
        QSqlQuery discard(impl_->database);
        discard.prepare(
            QStringLiteral("DELETE FROM asset_recipe_history WHERE asset_id = ? AND seq > ?"));
        discard.addBindValue(qstring_from_utf8(asset_id));
        discard.addBindValue(static_cast<qlonglong>(*discard_history_after_seq));
        if (!discard.exec())
        {
            return impl_->abort_transaction(map_sql_error(discard, "discard_recipe_history"));
        }
    }

    const auto written = recipe_json ?
                             save_recipe_json(asset_id, recipe_schema_version, *recipe_json) :
                             clear_recipe(asset_id);
    if (!written)
    {
        return impl_->abort_transaction(written.error());
    }

    std::optional<std::int64_t> committed_history_id;
    if (history_write == RecipeHistoryWrite::kAppendIfNew)
    {
        QSqlQuery latest(impl_->database);
        latest.prepare(QStringLiteral(
            "SELECT id, kind, recipe_json FROM asset_recipe_history WHERE asset_id = ? "
            "ORDER BY seq DESC, id DESC LIMIT 1"));
        latest.addBindValue(qstring_from_utf8(asset_id));
        if (!latest.exec())
        {
            return impl_->abort_transaction(map_sql_error(latest, "latest_recipe_history"));
        }
        const bool has_latest = latest.next();
        const auto latest_id = has_latest ? latest.value(0).toLongLong() : 0;
        const bool latest_is_history =
            has_latest &&
            utf8_from_qstring(latest.value(1).toString()) == kRecipeHistoryKindHistory;
        const bool duplicate =
            latest_is_history && utf8_from_qstring(latest.value(2).toString()) == history_json;
        if (!duplicate)
        {
            if (coalesce_history_id && latest_is_history &&
                latest_id == static_cast<qlonglong>(*coalesce_history_id))
            {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                QSqlQuery replace(impl_->database);
                replace.prepare(QStringLiteral(
                    "UPDATE asset_recipe_history SET recipe_json = ?, created_unix_ms = ? "
                    "WHERE id = ? AND asset_id = ? AND kind = ?"));
                replace.addBindValue(qstring_from_utf8(history_json));
                replace.addBindValue(static_cast<qlonglong>(now));
                replace.addBindValue(latest_id);
                replace.addBindValue(qstring_from_utf8(asset_id));
                replace.addBindValue(qstring_from_utf8(kRecipeHistoryKindHistory));
                if (!replace.exec())
                {
                    return impl_->abort_transaction(
                        map_sql_error(replace, "coalesce_recipe_history"));
                }
                if (replace.numRowsAffected() != 1)
                {
                    return impl_->abort_transaction(make_error(
                        ErrorCode::kConflict, "Recipe history changed before it could coalesce",
                        {{"history_id", std::to_string(*coalesce_history_id)}}));
                }
                committed_history_id = *coalesce_history_id;
            }
            else
            {
                auto recorded = append_recipe_history(asset_id, kRecipeHistoryKindHistory,
                                                      std::nullopt, history_json);
                if (!recorded)
                {
                    return impl_->abort_transaction(recorded.error());
                }
                committed_history_id = recorded.value().id;
            }
        }
        else
        {
            committed_history_id = static_cast<std::int64_t>(latest_id);
        }
    }

    QSqlQuery revision_query(impl_->database);
    if (!revision_query.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "bump_recipe_revision"));
    }
    if (!revision_query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision_query.next())
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "read_recipe_revision"));
    }
    const auto revision = revision_query.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit recipe transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = revision;
    return RecipeCommitResult{revision, committed_history_id};
}

Result<void> SqliteCatalogRepository::replace_asset_tags(const std::string_view asset_id,
                                                         const std::vector<std::string> &tags)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start tag replacement transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery clear(impl_->database);
    clear.prepare(QStringLiteral("DELETE FROM asset_tag WHERE asset_id = ?"));
    clear.addBindValue(qstring_from_utf8(asset_id));
    if (!clear.exec())
    {
        return impl_->abort_transaction(map_sql_error(clear, "clear_asset_tags"));
    }
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral("INSERT INTO asset_tag(asset_id, name) VALUES (?, ?)"));
    for (const auto &tag : tags)
    {
        insert.addBindValue(qstring_from_utf8(asset_id));
        insert.addBindValue(qstring_from_utf8(tag));
        if (!insert.exec())
        {
            return impl_->abort_transaction(map_sql_error(insert, "insert_asset_tag"));
        }
        insert.finish();
    }
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit tag replacement",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    return {};
}

Result<void> SqliteCatalogRepository::upsert_writable_metadata(const std::string_view asset_id,
                                                               const WritableMetadata &metadata)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata(asset_id, title, description, creator, copyright) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET title = excluded.title, "
        "description = excluded.description, creator = excluded.creator, "
        "copyright = excluded.copyright"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(optional_string(metadata.title));
    query.addBindValue(optional_string(metadata.description));
    query.addBindValue(optional_string(metadata.creator));
    query.addBindValue(optional_string(metadata.copyright));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_writable_metadata");
    }
    return {};
}

Result<void> SqliteCatalogRepository::upsert_capture_metadata(const std::string_view asset_id,
                                                              const CaptureMetadata &capture)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    auto valid = validate_capture_metadata(capture);
    if (!valid)
    {
        return valid.error();
    }
    std::optional<std::string> local_exif;
    std::optional<std::string> subsecond;
    std::optional<std::int32_t> offset;
    std::optional<std::int32_t> latitude;
    std::optional<std::int32_t> longitude;
    std::optional<std::uint32_t> altitude_magnitude;
    std::optional<std::int32_t> altitude_ref;
    if (capture.captured_datetime)
    {
        local_exif = capture.captured_datetime->local_exif;
        subsecond = capture.captured_datetime->subsecond_digits;
        offset = capture.captured_datetime->utc_offset_minutes;
    }
    if (capture.location)
    {
        latitude = capture.location->latitude_e6;
        longitude = capture.location->longitude_e6;
        if (capture.location->altitude)
        {
            altitude_magnitude = capture.location->altitude->magnitude_mm;
            altitude_ref =
                capture.location->altitude->reference == CaptureAltitudeReference::kBelowSeaLevel ?
                    1 :
                    0;
        }
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata(asset_id, camera_make, camera_model, iso, aperture, "
        "focal_length_mm, shutter_s, captured_unix_s, captured_local_exif, "
        "captured_subsecond_digits, captured_utc_offset_minutes, gps_latitude_e6, "
        "gps_longitude_e6, gps_altitude_magnitude_mm, gps_altitude_ref) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET camera_make = excluded.camera_make, "
        "camera_model = excluded.camera_model, iso = excluded.iso, aperture = excluded.aperture, "
        "focal_length_mm = excluded.focal_length_mm, shutter_s = excluded.shutter_s, "
        "captured_unix_s = excluded.captured_unix_s, "
        "captured_local_exif = excluded.captured_local_exif, "
        "captured_subsecond_digits = excluded.captured_subsecond_digits, "
        "captured_utc_offset_minutes = excluded.captured_utc_offset_minutes, "
        "gps_latitude_e6 = excluded.gps_latitude_e6, "
        "gps_longitude_e6 = excluded.gps_longitude_e6, "
        "gps_altitude_magnitude_mm = excluded.gps_altitude_magnitude_mm, "
        "gps_altitude_ref = excluded.gps_altitude_ref"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(optional_string(capture.camera_make));
    query.addBindValue(optional_string(capture.camera_model));
    query.addBindValue(optional_double(capture.iso));
    query.addBindValue(optional_double(capture.aperture));
    query.addBindValue(optional_double(capture.focal_length_mm));
    query.addBindValue(optional_double(capture.shutter_s));
    query.addBindValue(optional_i64(capture.captured_unix_s));
    query.addBindValue(optional_string(local_exif));
    query.addBindValue(optional_string(subsecond));
    query.addBindValue(optional_i32(offset));
    query.addBindValue(optional_i32(latitude));
    query.addBindValue(optional_i32(longitude));
    if (altitude_magnitude)
    {
        query.addBindValue(static_cast<qlonglong>(*altitude_magnitude));
    }
    else
    {
        query.addBindValue(QVariant());
    }
    query.addBindValue(optional_i32(altitude_ref));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_capture_metadata");
    }
    return {};
}

Result<void> SqliteCatalogRepository::commit_imported_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (capture_metadata_has_values(asset.capture))
    {
        auto valid = validate_capture_metadata(asset.capture);
        if (!valid)
        {
            return valid.error();
        }
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kTransactionBegin))
    {
        return make_error(ErrorCode::kIo, "Injected catalog import failure",
                          {{"reason", "injected_import_transaction_begin"}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start import publication transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kAssetBind))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_asset_bind"}}));
    }
    const auto inserted = insert_asset(asset);
    if (!inserted)
    {
        return impl_->abort_transaction(inserted.error());
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kAssetWrite))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_asset_write"}}));
    }
    if (capture_metadata_has_values(asset.capture))
    {
        if (impl_->consume_import_failure(testing::SqliteImportFailure::kCaptureBind))
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Injected catalog import failure",
                           {{"reason", "injected_import_capture_bind"}}));
        }
        const auto captured = upsert_capture_metadata(asset.id, asset.capture);
        if (!captured)
        {
            return impl_->abort_transaction(captured.error());
        }
        if (impl_->consume_import_failure(testing::SqliteImportFailure::kCaptureWrite))
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Injected catalog import failure",
                           {{"reason", "injected_import_capture_write"}}));
        }
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kRevisionUpdate))
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Injected catalog import failure",
                       {{"reason", "injected_import_revision_update"}}));
    }
    QSqlQuery revision_query(impl_->database);
    if (!revision_query.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "bump_import_revision"));
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kRevisionRead))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_revision_read"}}));
    }
    if (!revision_query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision_query.next())
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "read_import_revision"));
    }
    const auto revision = revision_query.value(0).toLongLong();
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kCommit))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_commit"}}));
    }
    if (impl_->import_failure == testing::SqliteImportFailure::kRollback)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Injected catalog import failure",
                       {{"reason", "injected_import_before_rollback"}}));
    }
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit import publication",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = revision;
    return {};
}

Result<void> SqliteCatalogRepository::commit_refreshed_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto valid = validate_capture_metadata(asset.capture);
    if (!valid)
        return valid.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to begin metadata refresh transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto updated = update_asset(asset);
    if (!updated)
        return impl_->abort_transaction(updated.error());
    auto captured = upsert_capture_metadata(asset.id, asset.capture);
    if (!captured)
        return impl_->abort_transaction(captured.error());
    QSqlQuery revision(impl_->database);
    if (!revision.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return impl_->abort_transaction(map_sql_error(revision, "bump_refresh_revision"));
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_refresh_revision"));
    const auto next_revision = revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit metadata refresh transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    return {};
}

Result<std::vector<RecipeHistoryEntry>>
SqliteCatalogRepository::list_recipe_history(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
                       "FROM asset_recipe_history WHERE asset_id = ? ORDER BY seq DESC, id DESC"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "list_recipe_history");
    }
    std::vector<RecipeHistoryEntry> entries;
    while (query.next())
    {
        entries.push_back(read_history(query));
    }
    return entries;
}

Result<std::optional<RecipeHistoryEntry>>
SqliteCatalogRepository::find_recipe_history(const std::int64_t history_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
                       "FROM asset_recipe_history WHERE id = ?"));
    query.addBindValue(static_cast<qlonglong>(history_id));
    if (!query.exec())
    {
        return map_sql_error(query, "find_recipe_history");
    }
    if (!query.next())
    {
        return std::optional<RecipeHistoryEntry>{};
    }
    return std::optional<RecipeHistoryEntry>{read_history(query)};
}

Result<RecipeHistoryEntry> SqliteCatalogRepository::append_recipe_history(
    const std::string_view asset_id, const std::string_view kind,
    const std::optional<std::string_view> label, const std::string_view recipe_json)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (kind != kRecipeHistoryKindHistory && kind != kRecipeHistoryKindSnapshot)
    {
        return make_error(ErrorCode::kValidation, "Recipe history kind is unsupported",
                          {{"kind", std::string(kind)}});
    }
    QSqlQuery max_seq(impl_->database);
    max_seq.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(seq), 0) FROM asset_recipe_history WHERE asset_id = ?"));
    max_seq.addBindValue(qstring_from_utf8(asset_id));
    if (!max_seq.exec() || !max_seq.next())
    {
        return map_sql_error(max_seq, "max_recipe_history_seq");
    }
    const auto seq = max_seq.value(0).toLongLong() + 1;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral(
        "INSERT INTO asset_recipe_history(asset_id, seq, kind, label, recipe_json, created_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(qstring_from_utf8(asset_id));
    insert.addBindValue(seq);
    insert.addBindValue(qstring_from_utf8(kind));
    if (label)
    {
        insert.addBindValue(qstring_from_utf8(*label));
    }
    else
    {
        insert.addBindValue(QVariant());
    }
    insert.addBindValue(qstring_from_utf8(recipe_json));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
    {
        return map_sql_error(insert, "append_recipe_history");
    }
    RecipeHistoryEntry entry;
    entry.id = insert.lastInsertId().toLongLong();
    entry.asset_id = std::string(asset_id);
    entry.seq = seq;
    entry.kind = std::string(kind);
    if (label)
    {
        entry.label = std::string(*label);
    }
    entry.recipe_json = std::string(recipe_json);
    entry.created_unix_ms = now;
    return entry;
}

Result<void> SqliteCatalogRepository::update_recipe_history_label(const std::int64_t history_id,
                                                                  const std::string_view label)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("UPDATE asset_recipe_history SET label = ? WHERE id = ? AND kind = ?"));
    query.addBindValue(qstring_from_utf8(label));
    query.addBindValue(static_cast<qlonglong>(history_id));
    query.addBindValue(qstring_from_utf8(kRecipeHistoryKindSnapshot));
    if (!query.exec())
    {
        return map_sql_error(query, "update_recipe_history_label");
    }
    if (query.numRowsAffected() == 0)
    {
        return make_error(ErrorCode::kNotFound, "Recipe snapshot does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    return {};
}

namespace
{

[[nodiscard]] std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::vector<std::string> unique_asset_ids(const std::vector<std::string> &asset_ids)
{
    std::vector<std::string> unique;
    std::set<std::string, std::less<>> seen;
    unique.reserve(asset_ids.size());
    for (const auto &asset_id : asset_ids)
    {
        if (seen.insert(asset_id).second)
            unique.push_back(asset_id);
    }
    return unique;
}

} // namespace

Result<std::vector<LibrarySetRecord>> SqliteCatalogRepository::list_library_sets() const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral(
            "SELECT id, kind, name, query_json, created_unix_ms, updated_unix_ms "
            "FROM library_set ORDER BY name COLLATE BINARY, id")))
        return map_sql_error(query, "list_library_sets");
    std::vector<LibrarySetRecord> sets;
    while (query.next())
    {
        auto record = read_library_set(impl_->database, query);
        if (!record)
            return record.error();
        sets.push_back(std::move(record).value());
    }
    return sets;
}

Result<std::optional<LibrarySetRecord>>
SqliteCatalogRepository::find_library_set(const std::string_view set_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT id, kind, name, query_json, created_unix_ms, updated_unix_ms "
        "FROM library_set WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(set_id));
    if (!query.exec())
        return map_sql_error(query, "find_library_set");
    if (!query.next())
        return std::optional<LibrarySetRecord>{};
    auto record = read_library_set(impl_->database, query);
    if (!record)
        return record.error();
    return std::optional<LibrarySetRecord>{std::move(record).value()};
}

Result<LibrarySetMutation> SqliteCatalogRepository::create_library_set(
    const LibrarySetKind kind, const std::string_view name, const std::optional<LibraryQuery> &query,
    const std::vector<std::string> &asset_ids, const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto normalized = normalize_library_set_name(name);
    if (!normalized)
        return normalized.error();
    LibrarySetRecord record;
    record.id = generate_library_set_id();
    record.kind = kind;
    record.name = normalized.value();
    record.query = query;
    auto valid = validate_library_set_record(record);
    if (!valid)
        return valid.error();
    if (kind == LibrarySetKind::kSmart && !asset_ids.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "A smart library set cannot store explicit members",
                          {{"reason", "invalid_library_set_members"}});
    }
    const auto members = unique_asset_ids(asset_ids);
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start library set transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery count(impl_->database);
    if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM library_set")) || !count.next())
        return impl_->abort_transaction(map_sql_error(count, "count_library_sets"));
    if (static_cast<std::size_t>(count.value(0).toLongLong()) >= kLibrarySetMaximumCount)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kValidation, "Catalog already has the maximum number of library sets",
            {{"reason", "library_set_limit"},
             {"maximum", std::to_string(kLibrarySetMaximumCount)}}));
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_library_set_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*expected_revision)},
             {"revision", std::to_string(current_revision)}}));
    }
    if (!members.empty())
    {
        QSqlQuery existing(impl_->database);
        QStringList placeholders;
        placeholders.reserve(static_cast<qsizetype>(members.size()));
        for (std::size_t index = 0; index < members.size(); ++index)
            placeholders.push_back(QStringLiteral("?"));
        existing.prepare(QStringLiteral("SELECT COUNT(*) FROM asset WHERE id IN (") +
                         placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
        for (const auto &asset_id : members)
            existing.addBindValue(qstring_from_utf8(asset_id));
        if (!existing.exec() || !existing.next())
            return impl_->abort_transaction(map_sql_error(existing, "verify_library_set_assets"));
        if (static_cast<std::size_t>(existing.value(0).toLongLong()) != members.size())
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kNotFound, "Library set member does not exist",
                {{"reason", "unknown_library_set_asset"}}));
        }
    }
    std::optional<std::string> query_json;
    if (record.query)
    {
        auto serialized = serialize_library_query_document(*record.query);
        if (!serialized)
            return impl_->abort_transaction(serialized.error());
        query_json = std::move(serialized).value();
    }
    const auto now = now_unix_ms();
    record.created_unix_ms = now;
    record.updated_unix_ms = now;
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral(
        "INSERT INTO library_set(id, kind, name, query_json, created_unix_ms, updated_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(qstring_from_utf8(record.id));
    insert.addBindValue(qstring_from_utf8(std::string(library_set_kind_name(kind))));
    insert.addBindValue(qstring_from_utf8(record.name));
    if (query_json)
        insert.addBindValue(qstring_from_utf8(*query_json));
    else
        insert.addBindValue(QVariant{});
    insert.addBindValue(static_cast<qlonglong>(now));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
    {
        const auto error_text = utf8_from_qstring(insert.lastError().text());
        if (error_text.find("UNIQUE") != std::string::npos)
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kConflict, "A library set with that name already exists",
                {{"name", record.name}, {"reason", "duplicate_library_set_name"}}));
        }
        return impl_->abort_transaction(map_sql_error(insert, "insert_library_set"));
    }
    if (!members.empty())
    {
        QSqlQuery member(impl_->database);
        member.prepare(QStringLiteral(
            "INSERT INTO library_set_member(set_id, asset_id, added_unix_ms) VALUES (?, ?, ?)"));
        for (const auto &asset_id : members)
        {
            member.addBindValue(qstring_from_utf8(record.id));
            member.addBindValue(qstring_from_utf8(asset_id));
            member.addBindValue(static_cast<qlonglong>(now));
            if (!member.exec())
                return impl_->abort_transaction(map_sql_error(member, "insert_library_set_member"));
            member.finish();
        }
    }
    auto bumped = impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                              "bump_library_set_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_library_set_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit library set write",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_set(record.id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kIo, "Library set was not visible after commit",
                          {{"reason", "library_set_missing_after_commit"}});
    return LibrarySetMutation{std::move(*loaded.value()), next_revision};
}

Result<LibrarySetMutation> SqliteCatalogRepository::rename_library_set(
    const std::string_view set_id, const std::string_view name,
    const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto normalized = normalize_library_set_name(name);
    if (!normalized)
        return normalized.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start library set transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_library_set_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*expected_revision)},
             {"revision", std::to_string(current_revision)}}));
    }
    QSqlQuery update(impl_->database);
    update.prepare(QStringLiteral(
        "UPDATE library_set SET name = ?, updated_unix_ms = ? WHERE id = ?"));
    update.addBindValue(qstring_from_utf8(normalized.value()));
    update.addBindValue(static_cast<qlonglong>(now_unix_ms()));
    update.addBindValue(qstring_from_utf8(set_id));
    if (!update.exec())
    {
        const auto error_text = utf8_from_qstring(update.lastError().text());
        if (error_text.find("UNIQUE") != std::string::npos)
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kConflict, "A library set with that name already exists",
                {{"name", normalized.value()}, {"reason", "duplicate_library_set_name"}}));
        }
        return impl_->abort_transaction(map_sql_error(update, "rename_library_set"));
    }
    if (update.numRowsAffected() != 1)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Library set was not found",
                                                   {{"set_id", std::string(set_id)},
                                                    {"reason", "unknown_library_set"}}));
    }
    auto bumped = impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                              "bump_library_set_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_library_set_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit library set write",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_set(set_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kNotFound, "Library set was not found",
                          {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}});
    return LibrarySetMutation{std::move(*loaded.value()), next_revision};
}

Result<std::int64_t> SqliteCatalogRepository::delete_library_set(
    const std::string_view set_id, const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start library set transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_library_set_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*expected_revision)},
             {"revision", std::to_string(current_revision)}}));
    }
    QSqlQuery remove(impl_->database);
    remove.prepare(QStringLiteral("DELETE FROM library_set WHERE id = ?"));
    remove.addBindValue(qstring_from_utf8(set_id));
    if (!remove.exec())
        return impl_->abort_transaction(map_sql_error(remove, "delete_library_set"));
    if (remove.numRowsAffected() != 1)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Library set was not found",
                                                   {{"set_id", std::string(set_id)},
                                                    {"reason", "unknown_library_set"}}));
    }
    auto bumped = impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                              "bump_library_set_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_library_set_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit library set write",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    return next_revision;
}

Result<LibrarySetMutation> SqliteCatalogRepository::add_library_set_members(
    const std::string_view set_id, const std::vector<std::string> &asset_ids,
    const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    const auto members = unique_asset_ids(asset_ids);
    if (members.empty())
    {
        return make_error(ErrorCode::kValidation, "Library set member list must not be empty",
                          {{"reason", "invalid_library_set_members"}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start library set transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_library_set_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*expected_revision)},
             {"revision", std::to_string(current_revision)}}));
    }
    QSqlQuery kind(impl_->database);
    kind.prepare(QStringLiteral("SELECT kind FROM library_set WHERE id = ?"));
    kind.addBindValue(qstring_from_utf8(set_id));
    if (!kind.exec())
        return impl_->abort_transaction(map_sql_error(kind, "read_library_set_kind"));
    if (!kind.next())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Library set was not found",
                                                   {{"set_id", std::string(set_id)},
                                                    {"reason", "unknown_library_set"}}));
    }
    if (utf8_from_qstring(kind.value(0).toString()) != kLibrarySetKindManual)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kValidation, "Only a manual library set can store members",
            {{"set_id", std::string(set_id)}, {"reason", "invalid_library_set_kind"}}));
    }
    QSqlQuery existing(impl_->database);
    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(members.size()));
    for (std::size_t index = 0; index < members.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    existing.prepare(QStringLiteral("SELECT COUNT(*) FROM asset WHERE id IN (") +
                     placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const auto &asset_id : members)
        existing.addBindValue(qstring_from_utf8(asset_id));
    if (!existing.exec() || !existing.next())
        return impl_->abort_transaction(map_sql_error(existing, "verify_library_set_assets"));
    if (static_cast<std::size_t>(existing.value(0).toLongLong()) != members.size())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                   "Library set member does not exist",
                                                   {{"reason", "unknown_library_set_asset"}}));
    }
    const auto now = now_unix_ms();
    QSqlQuery member(impl_->database);
    member.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO library_set_member(set_id, asset_id, added_unix_ms) VALUES (?, ?, ?)"));
    for (const auto &asset_id : members)
    {
        member.addBindValue(qstring_from_utf8(set_id));
        member.addBindValue(qstring_from_utf8(asset_id));
        member.addBindValue(static_cast<qlonglong>(now));
        if (!member.exec())
            return impl_->abort_transaction(map_sql_error(member, "insert_library_set_member"));
        member.finish();
    }
    QSqlQuery touch(impl_->database);
    touch.prepare(QStringLiteral("UPDATE library_set SET updated_unix_ms = ? WHERE id = ?"));
    touch.addBindValue(static_cast<qlonglong>(now));
    touch.addBindValue(qstring_from_utf8(set_id));
    if (!touch.exec())
        return impl_->abort_transaction(map_sql_error(touch, "touch_library_set"));
    auto bumped = impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                              "bump_library_set_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_library_set_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit library set write",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_set(set_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kNotFound, "Library set was not found",
                          {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}});
    return LibrarySetMutation{std::move(*loaded.value()), next_revision};
}

Result<LibrarySetMutation> SqliteCatalogRepository::remove_library_set_members(
    const std::string_view set_id, const std::vector<std::string> &asset_ids,
    const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    const auto members = unique_asset_ids(asset_ids);
    if (members.empty())
    {
        return make_error(ErrorCode::kValidation, "Library set member list must not be empty",
                          {{"reason", "invalid_library_set_members"}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start library set transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_library_set_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*expected_revision)},
             {"revision", std::to_string(current_revision)}}));
    }
    QSqlQuery kind(impl_->database);
    kind.prepare(QStringLiteral("SELECT kind FROM library_set WHERE id = ?"));
    kind.addBindValue(qstring_from_utf8(set_id));
    if (!kind.exec())
        return impl_->abort_transaction(map_sql_error(kind, "read_library_set_kind"));
    if (!kind.next())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Library set was not found",
                                                   {{"set_id", std::string(set_id)},
                                                    {"reason", "unknown_library_set"}}));
    }
    if (utf8_from_qstring(kind.value(0).toString()) != kLibrarySetKindManual)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kValidation, "Only a manual library set can store members",
            {{"set_id", std::string(set_id)}, {"reason", "invalid_library_set_kind"}}));
    }
    QSqlQuery present(impl_->database);
    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(members.size()));
    for (std::size_t index = 0; index < members.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    present.prepare(QStringLiteral("SELECT COUNT(*) FROM library_set_member WHERE set_id = ? AND "
                                   "asset_id IN (") +
                    placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    present.addBindValue(qstring_from_utf8(set_id));
    for (const auto &asset_id : members)
        present.addBindValue(qstring_from_utf8(asset_id));
    if (!present.exec() || !present.next())
        return impl_->abort_transaction(map_sql_error(present, "count_library_set_members"));
    if (static_cast<std::size_t>(present.value(0).toLongLong()) != members.size())
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kNotFound, "Library set member was not found",
            {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set_member"}}));
    }
    QSqlQuery remove(impl_->database);
    remove.prepare(QStringLiteral("DELETE FROM library_set_member WHERE set_id = ? AND asset_id IN (") +
                   placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    remove.addBindValue(qstring_from_utf8(set_id));
    for (const auto &asset_id : members)
        remove.addBindValue(qstring_from_utf8(asset_id));
    if (!remove.exec())
        return impl_->abort_transaction(map_sql_error(remove, "delete_library_set_member"));
    QSqlQuery touch(impl_->database);
    touch.prepare(QStringLiteral("UPDATE library_set SET updated_unix_ms = ? WHERE id = ?"));
    touch.addBindValue(static_cast<qlonglong>(now_unix_ms()));
    touch.addBindValue(qstring_from_utf8(set_id));
    if (!touch.exec())
        return impl_->abort_transaction(map_sql_error(touch, "touch_library_set"));
    auto bumped = impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                              "bump_library_set_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_library_set_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit library set write",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_set(set_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kNotFound, "Library set was not found",
                          {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}});
    return LibrarySetMutation{std::move(*loaded.value()), next_revision};
}

Result<std::int64_t> SqliteCatalogRepository::bump_revision()
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return map_sql_error(query, "bump_revision");
    }
    if (!query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !query.next())
    {
        return map_sql_error(query, "read_revision");
    }
    impl_->snapshot.revision = query.value(0).toLongLong();
    return impl_->snapshot.revision;
}

} // namespace ravo
