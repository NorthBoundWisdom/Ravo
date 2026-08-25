#include "ravo/adapters/sqlite_catalog.h"

#include <chrono>
#include <map>
#include <utility>
#include <vector>

#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include "ravo/domain/types.h"

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
    "CREATE TABLE asset ("
    "  id TEXT PRIMARY KEY,"
    "  normalized_uri TEXT NOT NULL UNIQUE,"
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

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] std::string utf8_from_qstring(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] TaskError map_sql_error(const QSqlQuery &query, const std::string_view action)
{
    return make_error(ErrorCode::kIo, "Catalog SQL statement failed",
                      {{"action", std::string(action)},
                       {"qt_error", utf8_from_qstring(query.lastError().text())}});
}

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

    QSqlQuery tags(database);
    if (!tags.exec(QStringLiteral("SELECT asset_id, name FROM asset_tag ORDER BY name ASC")))
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
    if (!metadata.exec(QStringLiteral(
            "SELECT asset_id, title, description, creator, copyright, camera_make, camera_model, "
            "iso, aperture, focal_length_mm, shutter_s, captured_unix_s FROM asset_metadata")))
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

[[nodiscard]] QString next_connection_name()
{
    static int counter = 0;
    return QString("ravo_catalog_%1").arg(++counter);
}

constexpr const char *kAssetSelect =
    "SELECT id, normalized_uri, media_type, size_bytes, mtime_unix_ms, content_fingerprint, "
    "width, height, import_state, error_code, error_message, created_unix_ms, rating, "
    "color_label, rejected, "
    "EXISTS(SELECT 1 FROM asset_recipe WHERE asset_id = asset.id) FROM asset";

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

    [[nodiscard]] Result<void> exec(const QString &sql, const std::string_view action)
    {
        QSqlQuery query(database);
        if (!query.exec(sql))
        {
            return map_sql_error(query, action);
        }
        return {};
    }
};

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
            impl->database.rollback();
            return created.error();
        }
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
        impl->database.rollback();
        return map_sql_error(insert, "insert_schema_info");
    }
    if (!impl->database.commit())
    {
        impl->database.rollback();
        return make_error(ErrorCode::kIo, "Unable to commit catalog schema",
                          {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}});
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
                impl->database.rollback();
                return !rating ? rating.error() : !color ? color.error() : rejected.error();
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
                impl->database.rollback();
                return recipes.error();
            }
            version = 3;
        }
        if (version == 3)
        {
            for (const char *sql : kSchemaV4Statements)
            {
                const auto created = impl->exec(QString::fromUtf8(sql), "migrate_v4_catalog_fields");
                if (!created)
                {
                    impl->database.rollback();
                    return created.error();
                }
            }
            version = 4;
        }
        if (version != kCatalogSchemaVersion)
        {
            impl->database.rollback();
            return make_error(ErrorCode::kValidation, "Catalog schema version cannot be upgraded",
                              {{"path", std::string(database_path)},
                               {"schema_version", std::to_string(version)}});
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
            impl->database.rollback();
            return map_sql_error(update, "migrate_schema_info");
        }
        if (!impl->database.commit())
        {
            impl->database.rollback();
            return make_error(ErrorCode::kIo, "Unable to commit catalog migration",
                              {{"qt_error", utf8_from_qstring(impl->database.lastError().text())}});
        }
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
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
        "content_fingerprint, width, height, import_state, error_code, error_message, "
        "created_unix_ms, rating, color_label, rejected) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(qstring_from_utf8(asset.id));
    query.addBindValue(qstring_from_utf8(asset.normalized_uri));
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
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("DELETE FROM asset WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "remove_asset");
    }
    if (query.numRowsAffected() == 0)
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
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
        impl_->database.rollback();
        return map_sql_error(clear, "clear_asset_tags");
    }
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral("INSERT INTO asset_tag(asset_id, name) VALUES (?, ?)"));
    for (const auto &tag : tags)
    {
        insert.addBindValue(qstring_from_utf8(asset_id));
        insert.addBindValue(qstring_from_utf8(tag));
        if (!insert.exec())
        {
            impl_->database.rollback();
            return map_sql_error(insert, "insert_asset_tag");
        }
        insert.finish();
    }
    if (!impl_->database.commit())
    {
        impl_->database.rollback();
        return make_error(ErrorCode::kIo, "Unable to commit tag replacement",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
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
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata(asset_id, camera_make, camera_model, iso, aperture, "
        "focal_length_mm, shutter_s, captured_unix_s) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET camera_make = excluded.camera_make, "
        "camera_model = excluded.camera_model, iso = excluded.iso, aperture = excluded.aperture, "
        "focal_length_mm = excluded.focal_length_mm, shutter_s = excluded.shutter_s, "
        "captured_unix_s = excluded.captured_unix_s"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(optional_string(capture.camera_make));
    query.addBindValue(optional_string(capture.camera_model));
    query.addBindValue(optional_double(capture.iso));
    query.addBindValue(optional_double(capture.aperture));
    query.addBindValue(optional_double(capture.focal_length_mm));
    query.addBindValue(optional_double(capture.shutter_s));
    query.addBindValue(optional_i64(capture.captured_unix_s));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_capture_metadata");
    }
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
    query.prepare(QStringLiteral(
        "SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
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
    query.prepare(QStringLiteral(
        "SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
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

Result<RecipeHistoryEntry>
SqliteCatalogRepository::append_recipe_history(const std::string_view asset_id,
                                               const std::string_view kind,
                                               const std::optional<std::string_view> label,
                                               const std::string_view recipe_json)
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
    max_seq.prepare(
        QStringLiteral("SELECT COALESCE(MAX(seq), 0) FROM asset_recipe_history WHERE asset_id = ?"));
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
