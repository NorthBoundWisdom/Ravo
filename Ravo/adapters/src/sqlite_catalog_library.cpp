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
    if (!query.exec(
            QStringLiteral("SELECT id, kind, name, query_json, created_unix_ms, updated_unix_ms "
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
    query.prepare(
        QStringLiteral("SELECT id, kind, name, query_json, created_unix_ms, updated_unix_ms "
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

Result<LibrarySetMutation>
SqliteCatalogRepository::create_library_set(const LibrarySetKind kind, const std::string_view name,
                                            const std::optional<LibraryQuery> &query,
                                            const std::vector<std::string> &asset_ids,
                                            const std::optional<std::int64_t> expected_revision)
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
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
            return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                       "Library set member does not exist",
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
            return impl_->abort_transaction(
                make_error(ErrorCode::kConflict, "A library set with that name already exists",
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
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
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

Result<LibrarySetMutation>
SqliteCatalogRepository::rename_library_set(const std::string_view set_id,
                                            const std::string_view name,
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
                       {{"reason", "stale_catalog_revision"},
                        {"expected_revision", std::to_string(*expected_revision)},
                        {"revision", std::to_string(current_revision)}}));
    }
    QSqlQuery update(impl_->database);
    update.prepare(
        QStringLiteral("UPDATE library_set SET name = ?, updated_unix_ms = ? WHERE id = ?"));
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Library set was not found",
                       {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}}));
    }
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
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

Result<std::int64_t>
SqliteCatalogRepository::delete_library_set(const std::string_view set_id,
                                            const std::optional<std::int64_t> expected_revision)
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Library set was not found",
                       {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}}));
    }
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Library set was not found",
                       {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}}));
    }
    if (utf8_from_qstring(kind.value(0).toString()) != kLibrarySetKindManual)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Only a manual library set can store members",
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
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
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
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Library set was not found",
                       {{"set_id", std::string(set_id)}, {"reason", "unknown_library_set"}}));
    }
    if (utf8_from_qstring(kind.value(0).toString()) != kLibrarySetKindManual)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Only a manual library set can store members",
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
    remove.prepare(
        QStringLiteral("DELETE FROM library_set_member WHERE set_id = ? AND asset_id IN (") +
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
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
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

Result<AssetVersionMutation>
SqliteCatalogRepository::create_asset_version(const std::string_view source_asset_id,
                                              const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start asset version transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_version_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision is stale",
                       {{"reason", "stale_catalog_revision"},
                        {"expected_revision", std::to_string(*expected_revision)},
                        {"revision", std::to_string(current_revision)}}));
    }
    auto source = find_asset_by_id(source_asset_id);
    if (!source)
        return impl_->abort_transaction(source.error());
    if (!source.value())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Asset does not exist",
                                                   {{"asset_id", std::string(source_asset_id)}}));
    }
    const auto primary_id = source.value()->source_asset_id.value_or(source.value()->id);
    QSqlQuery ordinal(impl_->database);
    ordinal.prepare(
        QStringLiteral("SELECT MAX(version_ordinal) FROM asset WHERE normalized_uri = ?"));
    ordinal.addBindValue(qstring_from_utf8(source.value()->normalized_uri));
    if (!ordinal.exec() || !ordinal.next())
        return impl_->abort_transaction(map_sql_error(ordinal, "max_version_ordinal"));
    const auto next_ordinal = ordinal.value(0).toInt() + 1;
    if (next_ordinal > kAssetVersionMaximum)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Asset already has the maximum number of versions",
                       {{"reason", "asset_version_limit"},
                        {"maximum", std::to_string(kAssetVersionMaximum)}}));
    }
    const auto version_id = generate_asset_id();
    const auto now = now_unix_ms();
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral(
        "INSERT INTO asset(id, normalized_uri, display_name, folder_uri, folder_id, media_type, "
        "size_bytes, mtime_unix_ms, content_fingerprint, width, height, import_state, error_code, "
        "error_message, created_unix_ms, rating, color_label, rejected, version_ordinal, "
        "source_asset_id) "
        "SELECT ?, normalized_uri, display_name, folder_uri, folder_id, media_type, size_bytes, "
        "mtime_unix_ms, content_fingerprint, width, height, import_state, error_code, "
        "error_message, ?, rating, color_label, rejected, ?, ? FROM asset WHERE id = ?"));
    insert.addBindValue(qstring_from_utf8(version_id));
    insert.addBindValue(static_cast<qlonglong>(now));
    insert.addBindValue(next_ordinal);
    insert.addBindValue(qstring_from_utf8(primary_id));
    insert.addBindValue(qstring_from_utf8(source_asset_id));
    if (!insert.exec())
        return impl_->abort_transaction(map_sql_error(insert, "insert_asset_version"));
    QSqlQuery copy_keywords(impl_->database);
    copy_keywords.prepare(
        QStringLiteral("INSERT INTO asset_keyword(asset_id, keyword_id) "
                       "SELECT ?, keyword_id FROM asset_keyword WHERE asset_id = ?"));
    copy_keywords.addBindValue(qstring_from_utf8(version_id));
    copy_keywords.addBindValue(qstring_from_utf8(source_asset_id));
    if (!copy_keywords.exec())
        return impl_->abort_transaction(map_sql_error(copy_keywords, "copy_version_keywords"));
    QSqlQuery copy_tags(impl_->database);
    copy_tags.prepare(QStringLiteral(
        "INSERT INTO asset_tag(asset_id, name) SELECT ?, name FROM asset_tag WHERE asset_id = ?"));
    copy_tags.addBindValue(qstring_from_utf8(version_id));
    copy_tags.addBindValue(qstring_from_utf8(source_asset_id));
    if (!copy_tags.exec())
        return impl_->abort_transaction(map_sql_error(copy_tags, "copy_version_tags"));
    QSqlQuery copy_metadata(impl_->database);
    copy_metadata.prepare(QStringLiteral(
        "INSERT INTO asset_metadata SELECT ?, title, description, creator, copyright, camera_make, "
        "camera_model, iso, aperture, focal_length_mm, shutter_s, captured_unix_s, "
        "captured_local_exif, captured_subsecond_digits, captured_utc_offset_minutes, "
        "gps_latitude_e6, gps_longitude_e6, gps_altitude_magnitude_mm, gps_altitude_ref, "
        "country, province_state, city, sublocation "
        "FROM asset_metadata WHERE asset_id = ?"));
    copy_metadata.addBindValue(qstring_from_utf8(version_id));
    copy_metadata.addBindValue(qstring_from_utf8(source_asset_id));
    if (!copy_metadata.exec())
        return impl_->abort_transaction(map_sql_error(copy_metadata, "copy_version_metadata"));
    QSqlQuery copy_recipe(impl_->database);
    copy_recipe.prepare(QStringLiteral(
        "INSERT INTO asset_recipe(asset_id, recipe_schema_version, recipe_json, updated_unix_ms) "
        "SELECT ?, recipe_schema_version, recipe_json, ? FROM asset_recipe WHERE asset_id = ?"));
    copy_recipe.addBindValue(qstring_from_utf8(version_id));
    copy_recipe.addBindValue(static_cast<qlonglong>(now));
    copy_recipe.addBindValue(qstring_from_utf8(source_asset_id));
    if (!copy_recipe.exec())
        return impl_->abort_transaction(map_sql_error(copy_recipe, "copy_version_recipe"));
    QSqlQuery copy_history(impl_->database);
    copy_history.prepare(QStringLiteral(
        "INSERT INTO asset_recipe_history(asset_id, seq, kind, label, recipe_json, created_unix_ms) "
        "SELECT ?, seq, kind, label, recipe_json, created_unix_ms FROM asset_recipe_history "
        "WHERE asset_id = ?"));
    copy_history.addBindValue(qstring_from_utf8(version_id));
    copy_history.addBindValue(qstring_from_utf8(source_asset_id));
    if (!copy_history.exec())
        return impl_->abort_transaction(map_sql_error(copy_history, "copy_version_history"));
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                    "bump_version_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_version_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit asset version",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_asset_by_id(version_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kIo, "Asset version was not visible after commit",
                          {{"reason", "asset_version_missing_after_commit"}});
    return AssetVersionMutation{std::move(*loaded.value()), next_revision};
}

Result<std::optional<LibraryStackRecord>>
SqliteCatalogRepository::find_library_stack(const std::string_view stack_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT id, pick_asset_id, created_unix_ms FROM library_stack WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(stack_id));
    if (!query.exec())
        return map_sql_error(query, "find_library_stack");
    if (!query.next())
        return std::optional<LibraryStackRecord>{};
    LibraryStackRecord record;
    record.id = utf8_from_qstring(query.value(0).toString());
    record.pick_asset_id = utf8_from_qstring(query.value(1).toString());
    record.created_unix_ms = query.value(2).toLongLong();
    QSqlQuery members(impl_->database);
    members.prepare(QStringLiteral(
        "SELECT asset_id FROM library_stack_member WHERE stack_id = ? ORDER BY position, asset_id"));
    members.addBindValue(qstring_from_utf8(record.id));
    if (!members.exec())
        return map_sql_error(members, "list_library_stack_members");
    while (members.next())
        record.member_ids.push_back(utf8_from_qstring(members.value(0).toString()));
    return std::optional<LibraryStackRecord>{std::move(record)};
}

Result<LibraryStackMutation>
SqliteCatalogRepository::stack_assets(const std::vector<std::string> &asset_ids,
                                      const std::string_view pick_asset_id,
                                      const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    const auto members = unique_asset_ids(asset_ids);
    if (members.size() < 2 || members.size() > kLibraryStackMaximumMembers)
    {
        return make_error(ErrorCode::kValidation, "A stack requires between 2 and 64 assets",
                          {{"reason", "invalid_library_stack_members"}});
    }
    bool pick_found = false;
    for (const auto &id : members)
    {
        if (id == pick_asset_id)
            pick_found = true;
    }
    if (!pick_found)
    {
        return make_error(ErrorCode::kValidation, "Stack pick must be one of the members",
                          {{"reason", "invalid_library_stack_pick"}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start stack transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_stack_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Catalog revision is stale",
                                                   {{"reason", "stale_catalog_revision"}}));
    }
    QSqlQuery count(impl_->database);
    if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM library_stack")) || !count.next())
        return impl_->abort_transaction(map_sql_error(count, "count_library_stacks"));
    if (static_cast<std::size_t>(count.value(0).toLongLong()) >= kLibraryStackMaximumCount)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Catalog already has the maximum number of stacks",
                       {{"reason", "library_stack_limit"}}));
    }
    QStringList placeholders;
    for (std::size_t index = 0; index < members.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    QSqlQuery existing(impl_->database);
    existing.prepare(QStringLiteral("SELECT COUNT(*) FROM asset WHERE id IN (") +
                     placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const auto &id : members)
        existing.addBindValue(qstring_from_utf8(id));
    if (!existing.exec() || !existing.next())
        return impl_->abort_transaction(map_sql_error(existing, "verify_stack_assets"));
    if (static_cast<std::size_t>(existing.value(0).toLongLong()) != members.size())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                   "Stack member does not exist",
                                                   {{"reason", "unknown_library_stack_asset"}}));
    }
    QSqlQuery occupied(impl_->database);
    occupied.prepare(
        QStringLiteral("SELECT COUNT(*) FROM library_stack_member WHERE asset_id IN (") +
        placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const auto &id : members)
        occupied.addBindValue(qstring_from_utf8(id));
    if (!occupied.exec() || !occupied.next())
        return impl_->abort_transaction(map_sql_error(occupied, "verify_stack_free"));
    if (occupied.value(0).toLongLong() != 0)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "An asset already belongs to a stack",
                                                   {{"reason", "asset_already_stacked"}}));
    }
    const auto stack_id = generate_library_stack_id();
    const auto now = now_unix_ms();
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral(
        "INSERT INTO library_stack(id, pick_asset_id, created_unix_ms) VALUES (?, ?, ?)"));
    insert.addBindValue(qstring_from_utf8(stack_id));
    insert.addBindValue(qstring_from_utf8(pick_asset_id));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
        return impl_->abort_transaction(map_sql_error(insert, "insert_library_stack"));
    QSqlQuery member(impl_->database);
    member.prepare(QStringLiteral(
        "INSERT INTO library_stack_member(stack_id, asset_id, position) VALUES (?, ?, ?)"));
    int position = 0;
    for (const auto &id : members)
    {
        member.addBindValue(qstring_from_utf8(stack_id));
        member.addBindValue(qstring_from_utf8(id));
        member.addBindValue(position++);
        if (!member.exec())
            return impl_->abort_transaction(map_sql_error(member, "insert_stack_member"));
        member.finish();
    }
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                    "bump_stack_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_stack_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit stack",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_stack(stack_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kIo, "Stack was not visible after commit",
                          {{"reason", "library_stack_missing_after_commit"}});
    return LibraryStackMutation{std::move(*loaded.value()), next_revision};
}

Result<std::int64_t>
SqliteCatalogRepository::unstack_assets(const std::string_view stack_id,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start stack transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_stack_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Catalog revision is stale",
                                                   {{"reason", "stale_catalog_revision"}}));
    }
    QSqlQuery drop(impl_->database);
    drop.prepare(QStringLiteral("DELETE FROM library_stack WHERE id = ?"));
    drop.addBindValue(qstring_from_utf8(stack_id));
    if (!drop.exec())
        return impl_->abort_transaction(map_sql_error(drop, "delete_library_stack"));
    if (drop.numRowsAffected() != 1)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Stack was not found",
                                                   {{"reason", "unknown_library_stack"}}));
    }
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                    "bump_stack_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_stack_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit unstack",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    return next_revision;
}

Result<LibraryStackMutation>
SqliteCatalogRepository::set_stack_pick(const std::string_view stack_id,
                                        const std::string_view pick_asset_id,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start stack transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    QSqlQuery revision(impl_->database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_stack_revision"));
    const auto current_revision = revision.value(0).toLongLong();
    if (expected_revision && *expected_revision != current_revision)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Catalog revision is stale",
                                                   {{"reason", "stale_catalog_revision"}}));
    }
    QSqlQuery member(impl_->database);
    member.prepare(
        QStringLiteral("SELECT 1 FROM library_stack_member WHERE stack_id = ? AND asset_id = ?"));
    member.addBindValue(qstring_from_utf8(stack_id));
    member.addBindValue(qstring_from_utf8(pick_asset_id));
    if (!member.exec())
        return impl_->abort_transaction(map_sql_error(member, "verify_stack_pick"));
    if (!member.next())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                   "Stack pick must be a member of the stack",
                                                   {{"reason", "invalid_library_stack_pick"}}));
    }
    QSqlQuery update(impl_->database);
    update.prepare(QStringLiteral("UPDATE library_stack SET pick_asset_id = ? WHERE id = ?"));
    update.addBindValue(qstring_from_utf8(pick_asset_id));
    update.addBindValue(qstring_from_utf8(stack_id));
    if (!update.exec())
        return impl_->abort_transaction(map_sql_error(update, "update_stack_pick"));
    if (update.numRowsAffected() != 1)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound, "Stack was not found",
                                                   {{"reason", "unknown_library_stack"}}));
    }
    auto bumped =
        impl_->exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1"),
                    "bump_stack_revision");
    if (!bumped)
        return impl_->abort_transaction(bumped.error());
    QSqlQuery read_revision(impl_->database);
    if (!read_revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !read_revision.next())
        return impl_->abort_transaction(map_sql_error(read_revision, "read_stack_revision"));
    const auto next_revision = read_revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit stack pick",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    auto loaded = find_library_stack(stack_id);
    if (!loaded || !loaded.value())
        return make_error(ErrorCode::kNotFound, "Stack was not found",
                          {{"reason", "unknown_library_stack"}});
    return LibraryStackMutation{std::move(*loaded.value()), next_revision};
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

Result<LibraryCaptureFacets> SqliteCatalogRepository::list_capture_facets() const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");

    LibraryCaptureFacets facets;
    const auto limit = static_cast<qlonglong>(kLibraryFacetMaximumValues);

    {
        QSqlQuery query(impl_->database);
        query.prepare(QStringLiteral(
            "SELECT camera_make, camera_model, COUNT(*) AS asset_count "
            "FROM asset_metadata "
            "WHERE (camera_make IS NOT NULL AND camera_make != '') "
            "   OR (camera_model IS NOT NULL AND camera_model != '') "
            "GROUP BY camera_make, camera_model "
            "ORDER BY "
            "  lower(trim(coalesce(camera_make, '') || ' ' || coalesce(camera_model, ''))) ASC, "
            "  camera_make ASC, camera_model ASC "
            "LIMIT ?"));
        query.addBindValue(limit + 1);
        if (!query.exec())
            return map_sql_error(query, "list_capture_facets_cameras");
        while (query.next())
        {
            if (facets.cameras.size() >= kLibraryFacetMaximumValues)
            {
                facets.truncated = true;
                break;
            }
            LibraryFacetEntry entry;
            entry.camera_make = string_column(query, 0);
            entry.camera_model = string_column(query, 1);
            if (entry.camera_make && entry.camera_make->empty())
                entry.camera_make.reset();
            if (entry.camera_model && entry.camera_model->empty())
                entry.camera_model.reset();
            std::string label;
            if (entry.camera_make)
                label = *entry.camera_make;
            if (entry.camera_model)
            {
                if (!label.empty())
                    label.push_back(' ');
                label.append(*entry.camera_model);
            }
            entry.label = label;
            entry.key = (entry.camera_make ? *entry.camera_make : std::string{}) + "\x1f" +
                        (entry.camera_model ? *entry.camera_model : std::string{});
            entry.count = static_cast<std::size_t>(query.value(2).toULongLong());
            facets.cameras.push_back(std::move(entry));
        }
        if (!facets.truncated && query.next())
            facets.truncated = true;
    }

    {
        QSqlQuery query(impl_->database);
        query.prepare(QStringLiteral("SELECT focal_length_mm, COUNT(*) AS asset_count "
                                     "FROM asset_metadata "
                                     "WHERE focal_length_mm IS NOT NULL "
                                     "GROUP BY focal_length_mm "
                                     "ORDER BY focal_length_mm ASC "
                                     "LIMIT ?"));
        query.addBindValue(limit + 1);
        if (!query.exec())
            return map_sql_error(query, "list_capture_facets_lenses");
        while (query.next())
        {
            if (facets.lenses.size() >= kLibraryFacetMaximumValues)
            {
                facets.truncated = true;
                break;
            }
            LibraryFacetEntry entry;
            entry.focal_length_mm = query.value(0).toDouble();
            entry.label = std::to_string(*entry.focal_length_mm);
            // Trim trailing zeros for label readability without locale.
            while (entry.label.size() > 1 && entry.label.find('.') != std::string::npos &&
                   (entry.label.back() == '0' || entry.label.back() == '.'))
            {
                const bool drop_dot = entry.label.back() == '.';
                entry.label.pop_back();
                if (drop_dot)
                    break;
            }
            entry.label.append(" mm");
            entry.key = std::to_string(*entry.focal_length_mm);
            entry.count = static_cast<std::size_t>(query.value(1).toULongLong());
            facets.lenses.push_back(std::move(entry));
        }
        if (query.next())
            facets.truncated = true;
    }

    {
        QSqlQuery query(impl_->database);
        query.prepare(QStringLiteral(
            "SELECT substr(captured_local_exif, 1, 10) AS capture_day, COUNT(*) AS asset_count "
            "FROM asset_metadata "
            "WHERE captured_local_exif IS NOT NULL AND length(captured_local_exif) >= 10 "
            "GROUP BY capture_day "
            "ORDER BY capture_day DESC "
            "LIMIT ?"));
        query.addBindValue(limit + 1);
        if (!query.exec())
            return map_sql_error(query, "list_capture_facets_dates");
        while (query.next())
        {
            if (facets.capture_dates.size() >= kLibraryFacetMaximumValues)
            {
                facets.truncated = true;
                break;
            }
            auto day = string_column(query, 0);
            if (!day || day->size() != 10U)
                continue;
            LibraryFacetEntry entry;
            entry.captured_local_date = *day;
            entry.key = *day;
            entry.label = *day;
            entry.count = static_cast<std::size_t>(query.value(1).toULongLong());
            facets.capture_dates.push_back(std::move(entry));
        }
        if (query.next())
            facets.truncated = true;
    }

    return facets;
}

} // namespace ravo
