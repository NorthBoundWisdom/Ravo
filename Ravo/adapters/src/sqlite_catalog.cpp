#include "ravo/adapters/sqlite_catalog.h"

#include "catalog_sql_internal.h"

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
using namespace sqlite_internal;

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
    "  normalized_uri TEXT NOT NULL,"
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
    "  rejected INTEGER NOT NULL DEFAULT 0,"
    "  version_ordinal INTEGER NOT NULL DEFAULT 0,"
    "  source_asset_id TEXT REFERENCES asset(id) ON DELETE CASCADE,"
    "  UNIQUE(normalized_uri, version_ordinal),"
    "  CHECK((version_ordinal = 0 AND source_asset_id IS NULL) OR "
    "        (version_ordinal > 0 AND source_asset_id IS NOT NULL))"
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

constexpr const char *kSchemaV7AssetIndexes[] = {
    "CREATE INDEX IF NOT EXISTS asset_created_id_idx ON asset(created_unix_ms, id)",
    "CREATE INDEX IF NOT EXISTS asset_display_name_id_idx ON asset(display_name, id)",
    "CREATE INDEX IF NOT EXISTS asset_folder_uri_idx ON asset(folder_uri)",
    "CREATE INDEX IF NOT EXISTS asset_rating_id_idx ON asset(rating, id)",
    "CREATE INDEX IF NOT EXISTS asset_size_id_idx ON asset(size_bytes, id)",
    "CREATE INDEX IF NOT EXISTS asset_media_type_idx ON asset(media_type)",
};

constexpr const char *kSchemaV7RelatedIndexes[] = {
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

constexpr const char *kSchemaV11VersionIndex =
    "CREATE INDEX IF NOT EXISTS asset_source_version_idx ON asset(source_asset_id, version_ordinal)";

constexpr const char *kSchemaV11Statements[] = {
    "CREATE TABLE IF NOT EXISTS library_stack ("
    "  id TEXT PRIMARY KEY,"
    "  pick_asset_id TEXT NOT NULL REFERENCES asset(id),"
    "  created_unix_ms INTEGER NOT NULL"
    ")",
    "CREATE TABLE IF NOT EXISTS library_stack_member ("
    "  stack_id TEXT NOT NULL REFERENCES library_stack(id) ON DELETE CASCADE,"
    "  asset_id TEXT NOT NULL UNIQUE REFERENCES asset(id) ON DELETE CASCADE,"
    "  position INTEGER NOT NULL,"
    "  PRIMARY KEY (stack_id, asset_id)"
    ")",
    "CREATE INDEX IF NOT EXISTS library_stack_pick_idx ON library_stack(pick_asset_id)",
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

} // namespace

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
    for (const char *statement : kSchemaV7AssetIndexes)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_library_index");
        if (!created)
            return impl->abort_transaction(created.error());
    }
    for (const char *statement : kSchemaV7RelatedIndexes)
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
    const auto version_index =
        impl->exec(QString::fromUtf8(kSchemaV11VersionIndex), "create_asset_version_index");
    if (!version_index)
        return impl->abort_transaction(version_index.error());
    for (const char *statement : kSchemaV11Statements)
    {
        const auto created = impl->exec(QString::fromUtf8(statement), "create_library_stack");
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
    const auto catalog_id = utf8_from_qstring(query.value(1).toString());
    const auto revision = query.value(2).toLongLong();
    query.finish();
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
        auto disable_fk =
            impl->exec(QStringLiteral("PRAGMA foreign_keys = OFF"), "disable_foreign_keys");
        if (!disable_fk)
            return disable_fk.error();
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
            for (const char *statement : kSchemaV7AssetIndexes)
            {
                const auto created =
                    impl->exec(QString::fromUtf8(statement), "migrate_v7_library_index");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            for (const char *statement : kSchemaV7RelatedIndexes)
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
                const auto created = impl->exec(QString::fromUtf8(sql), "migrate_v10_library_set");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            version = 10;
        }
        if (version == 10)
        {
            auto rebuilt = impl->exec(
                QStringLiteral("CREATE TABLE asset_v11 ("
                               "  id TEXT PRIMARY KEY,"
                               "  normalized_uri TEXT NOT NULL,"
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
                               "  rejected INTEGER NOT NULL DEFAULT 0,"
                               "  version_ordinal INTEGER NOT NULL DEFAULT 0,"
                               "  source_asset_id TEXT REFERENCES asset_v11(id) ON DELETE CASCADE,"
                               "  UNIQUE(normalized_uri, version_ordinal),"
                               "  CHECK((version_ordinal = 0 AND source_asset_id IS NULL) OR "
                               "        (version_ordinal > 0 AND source_asset_id IS NOT NULL))"
                               ")"),
                "migrate_v11_create_asset");
            if (!rebuilt)
                return impl->abort_transaction(rebuilt.error());
            auto copied = impl->exec(
                QStringLiteral(
                    "INSERT INTO asset_v11(id, normalized_uri, display_name, folder_uri, folder_id, "
                    "media_type, size_bytes, mtime_unix_ms, content_fingerprint, width, height, "
                    "import_state, error_code, error_message, created_unix_ms, rating, color_label, "
                    "rejected, version_ordinal, source_asset_id) "
                    "SELECT id, normalized_uri, display_name, folder_uri, folder_id, media_type, "
                    "size_bytes, mtime_unix_ms, content_fingerprint, width, height, import_state, "
                    "error_code, error_message, created_unix_ms, rating, color_label, rejected, 0, "
                    "NULL FROM asset"),
                "migrate_v11_copy_asset");
            if (!copied)
                return impl->abort_transaction(copied.error());
            auto dropped = impl->exec(QStringLiteral("DROP TABLE asset"), "migrate_v11_drop_asset");
            if (!dropped)
                return impl->abort_transaction(dropped.error());
            auto renamed = impl->exec(QStringLiteral("ALTER TABLE asset_v11 RENAME TO asset"),
                                      "migrate_v11_rename_asset");
            if (!renamed)
                return impl->abort_transaction(renamed.error());
            for (const char *sql : kSchemaV7AssetIndexes)
            {
                auto index = impl->exec(QString::fromUtf8(sql), "migrate_v11_restore_indexes");
                if (!index)
                    return impl->abort_transaction(index.error());
            }
            auto folder_index = impl->exec(QString::fromUtf8(kSchemaV9FolderIdentityIndex),
                                           "migrate_v11_folder_index");
            if (!folder_index)
                return impl->abort_transaction(folder_index.error());
            auto version_index =
                impl->exec(QString::fromUtf8(kSchemaV11VersionIndex), "migrate_v11_version_index");
            if (!version_index)
                return impl->abort_transaction(version_index.error());
            auto insert_trigger = impl->exec(QString::fromUtf8(kSchemaV6Triggers[0]),
                                             "migrate_v11_asset_insert_trigger");
            if (!insert_trigger)
                return impl->abort_transaction(insert_trigger.error());
            auto update_trigger = impl->exec(QString::fromUtf8(kSchemaV6AssetUpdateTrigger),
                                             "migrate_v11_asset_update_trigger");
            if (!update_trigger)
                return impl->abort_transaction(update_trigger.error());
            for (const char *sql : kSchemaV11Statements)
            {
                auto created = impl->exec(QString::fromUtf8(sql), "migrate_v11_library_stack");
                if (!created)
                    return impl->abort_transaction(created.error());
            }
            version = 11;
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
        auto enable_fk =
            impl->exec(QStringLiteral("PRAGMA foreign_keys = ON"), "enable_foreign_keys");
        if (!enable_fk)
            return enable_fk.error();
    }
    auto repaired = impl->repair_v5_capture_columns();
    if (!repaired)
    {
        return repaired.error();
    }
    impl->snapshot.schema_version = kCatalogSchemaVersion;
    impl->snapshot.catalog_id = catalog_id;
    impl->snapshot.revision = revision;
    impl->snapshot.database_path = impl->database_path;
    return std::unique_ptr<SqliteCatalogRepository>(new SqliteCatalogRepository(std::move(impl)));
}

} // namespace ravo
