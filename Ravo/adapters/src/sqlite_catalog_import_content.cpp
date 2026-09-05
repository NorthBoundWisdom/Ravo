#include "ravo/adapters/sqlite_catalog.h"

#include <algorithm>
#include <limits>
#include <QSqlQuery>
#include "catalog_sql_internal.h"

namespace ravo
{
using namespace sqlite_internal;

Result<std::vector<ImportContentSource>>
SqliteCatalogRepository::import_content_sources(const std::uint64_t size_bytes,
                                                const std::string_view after_asset_id) const
{
    if (!impl_)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (size_bytes > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
        return make_error(ErrorCode::kValidation, "Import file size is out of range");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT a.id, a.normalized_uri, a.size_bytes, a.mtime_unix_ms, h.sha256 "
        "FROM asset a LEFT JOIN asset_content_hash h ON h.asset_id = a.id "
        "AND h.normalized_uri = a.normalized_uri AND h.size_bytes = a.size_bytes "
        "AND h.mtime_unix_ms = a.mtime_unix_ms "
        "WHERE a.size_bytes = ? AND a.id > ? AND a.version_ordinal = 0 ORDER BY a.id LIMIT 200"));
    query.addBindValue(static_cast<qlonglong>(size_bytes));
    query.addBindValue(qstring_from_utf8(after_asset_id));
    if (!query.exec())
        return map_sql_error(query, "list_import_content_sources");
    std::vector<ImportContentSource> sources;
    while (query.next())
    {
        ImportContentSource source;
        source.asset_id = utf8_from_qstring(query.value(0).toString());
        source.normalized_uri = utf8_from_qstring(query.value(1).toString());
        source.size_bytes = query.value(2).toULongLong();
        source.mtime_unix_ms = query.value(3).toLongLong();
        if (!query.value(4).isNull())
            source.sha256 = utf8_from_qstring(query.value(4).toString());
        sources.push_back(std::move(source));
    }
    return sources;
}

Result<void> SqliteCatalogRepository::cache_import_content(const ImportContentSource &source,
                                                           const std::string_view sha256)
{
    if (!impl_)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (sha256.size() != 64 ||
        !std::all_of(sha256.begin(), sha256.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return make_error(ErrorCode::kValidation, "Invalid SHA-256 content identity",
                          {{"reason", "invalid_import_content_hash"}});
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_content_hash(asset_id, normalized_uri, size_bytes, mtime_unix_ms, sha256) "
        "SELECT id, normalized_uri, size_bytes, mtime_unix_ms, ? FROM asset "
        "WHERE id = ? AND normalized_uri = ? AND size_bytes = ? AND mtime_unix_ms = ? "
        "ON CONFLICT(asset_id) DO UPDATE SET normalized_uri = excluded.normalized_uri, "
        "size_bytes = excluded.size_bytes, mtime_unix_ms = excluded.mtime_unix_ms, sha256 = excluded.sha256"));
    query.addBindValue(qstring_from_utf8(sha256));
    query.addBindValue(qstring_from_utf8(source.asset_id));
    query.addBindValue(qstring_from_utf8(source.normalized_uri));
    query.addBindValue(static_cast<qlonglong>(source.size_bytes));
    query.addBindValue(static_cast<qlonglong>(source.mtime_unix_ms));
    if (!query.exec())
        return map_sql_error(query, "cache_import_content");
    if (query.numRowsAffected() != 1)
        return make_error(ErrorCode::kConflict, "Catalog source changed while hashing",
                          {{"reason", "import_content_source_changed"}});
    return {};
}

Result<std::optional<std::string>>
SqliteCatalogRepository::find_import_content(const std::uint64_t size_bytes,
                                             const std::string_view sha256) const
{
    if (!impl_)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT a.id FROM asset_content_hash h JOIN asset a ON a.id = h.asset_id "
        "AND a.normalized_uri = h.normalized_uri AND a.size_bytes = h.size_bytes "
        "AND a.mtime_unix_ms = h.mtime_unix_ms WHERE h.size_bytes = ? AND h.sha256 = ? "
        "ORDER BY a.id LIMIT 1"));
    query.addBindValue(static_cast<qlonglong>(size_bytes));
    query.addBindValue(qstring_from_utf8(sha256));
    if (!query.exec())
        return map_sql_error(query, "find_import_content");
    if (!query.next())
        return std::optional<std::string>{};
    return std::optional<std::string>{utf8_from_qstring(query.value(0).toString())};
}
} // namespace ravo
