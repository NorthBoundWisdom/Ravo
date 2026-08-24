#include "ravo/adapters/sqlite_catalog.h"

#include <chrono>
#include <utility>

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
    return std::unique_ptr<SqliteCatalogRepository>(
        new SqliteCatalogRepository(std::move(impl)));
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
        return make_error(ErrorCode::kUnsupported, "Catalog schema version is newer than this Ravo",
                          {{"path", std::string(database_path)},
                           {"schema_version", std::to_string(version)}});
    }
    if (version < 1)
    {
        return make_error(ErrorCode::kValidation, "Catalog schema version is invalid",
                          {{"path", std::string(database_path)},
                           {"schema_version", std::to_string(version)}});
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
                QStringLiteral(
                    "CREATE TABLE asset_recipe ("
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
    return std::unique_ptr<SqliteCatalogRepository>(
        new SqliteCatalogRepository(std::move(impl)));
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
    return std::optional<AssetRecord>{read_asset(query)};
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
    return std::optional<AssetRecord>{read_asset(query)};
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
    query.prepare(QStringLiteral(
        "UPDATE asset SET rating = ?, color_label = ?, rejected = ? WHERE id = ?"));
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
