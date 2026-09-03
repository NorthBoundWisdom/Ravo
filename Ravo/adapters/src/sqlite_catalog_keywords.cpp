#include "catalog_sql_internal.h"

#include "ravo/domain/types.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace ravo
{
namespace
{

using sqlite_internal::map_sql_error;
using sqlite_internal::qstring_from_utf8;
using sqlite_internal::utf8_from_qstring;

[[nodiscard]] std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] KeywordRecord read_keyword(const QSqlQuery &query)
{
    KeywordRecord record;
    record.id = utf8_from_qstring(query.value(0).toString());
    if (!query.value(1).isNull())
        record.parent_id = utf8_from_qstring(query.value(1).toString());
    record.name = utf8_from_qstring(query.value(2).toString());
    record.path = utf8_from_qstring(query.value(3).toString());
    record.depth = query.value(4).toInt();
    record.created_unix_ms = query.value(5).toLongLong();
    record.updated_unix_ms = query.value(6).toLongLong();
    return record;
}

[[nodiscard]] Result<std::int64_t> read_revision(QSqlDatabase &database, std::string_view action)
{
    QSqlQuery revision(database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return map_sql_error(revision, action);
    return revision.value(0).toLongLong();
}

[[nodiscard]] Result<void> require_revision(QSqlDatabase &database,
                                            const std::optional<std::int64_t> expected_revision,
                                            std::string_view action)
{
    auto current = read_revision(database, action);
    if (!current)
        return current.error();
    if (expected_revision && *expected_revision != current.value())
    {
        return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                          {{"reason", "stale_catalog_revision"},
                           {"expected_revision", std::to_string(*expected_revision)},
                           {"revision", std::to_string(current.value())}});
    }
    return {};
}

[[nodiscard]] Result<std::int64_t> bump_revision_locked(QSqlDatabase &database)
{
    QSqlQuery update(database);
    if (!update.exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return map_sql_error(update, "bump_keyword_revision");
    return read_revision(database, "read_keyword_revision");
}

[[nodiscard]] Result<std::optional<KeywordRecord>> load_keyword_by_id(QSqlDatabase &database,
                                                                      std::string_view keyword_id)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT id, parent_id, name, path, depth, created_unix_ms, updated_unix_ms "
                       "FROM keyword WHERE id = ?"));
    query.addBindValue(qstring_from_utf8(keyword_id));
    if (!query.exec())
        return map_sql_error(query, "find_keyword_by_id");
    if (!query.next())
        return std::optional<KeywordRecord>{};
    return std::optional<KeywordRecord>{read_keyword(query)};
}

[[nodiscard]] Result<std::optional<KeywordRecord>> load_keyword_by_path(QSqlDatabase &database,
                                                                        std::string_view path)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT id, parent_id, name, path, depth, created_unix_ms, updated_unix_ms "
                       "FROM keyword WHERE path = ?"));
    query.addBindValue(qstring_from_utf8(path));
    if (!query.exec())
        return map_sql_error(query, "find_keyword_by_path");
    if (!query.next())
        return std::optional<KeywordRecord>{};
    return std::optional<KeywordRecord>{read_keyword(query)};
}

[[nodiscard]] Result<std::size_t> count_keywords(QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM keyword")) || !query.next())
        return map_sql_error(query, "count_keywords");
    return static_cast<std::size_t>(query.value(0).toLongLong());
}

[[nodiscard]] Result<void> rebuild_asset_tag_projection(QSqlDatabase &database,
                                                        std::string_view asset_id)
{
    QSqlQuery clear(database);
    clear.prepare(QStringLiteral("DELETE FROM asset_tag WHERE asset_id = ?"));
    clear.addBindValue(qstring_from_utf8(asset_id));
    if (!clear.exec())
        return map_sql_error(clear, "clear_asset_tag_projection");

    QSqlQuery paths(database);
    paths.prepare(QStringLiteral("SELECT k.path FROM asset_keyword ak "
                                 "INNER JOIN keyword k ON k.id = ak.keyword_id "
                                 "WHERE ak.asset_id = ? ORDER BY k.path COLLATE NOCASE"));
    paths.addBindValue(qstring_from_utf8(asset_id));
    if (!paths.exec())
        return map_sql_error(paths, "list_asset_keyword_paths");

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO asset_tag(asset_id, name) VALUES (?, ?)"));
    while (paths.next())
    {
        insert.addBindValue(qstring_from_utf8(asset_id));
        insert.addBindValue(paths.value(0));
        if (!insert.exec())
            return map_sql_error(insert, "insert_asset_tag_projection");
        insert.finish();
    }
    return {};
}

[[nodiscard]] Result<void> write_asset_tag_projection(QSqlDatabase &database,
                                                      std::string_view asset_id,
                                                      const std::vector<std::string> &keyword_ids)
{
    QSqlQuery clear(database);
    clear.prepare(QStringLiteral("DELETE FROM asset_tag WHERE asset_id = ?"));
    clear.addBindValue(qstring_from_utf8(asset_id));
    if (!clear.exec())
        return map_sql_error(clear, "clear_asset_tag_projection");
    QSqlQuery lookup(database);
    lookup.prepare(QStringLiteral("SELECT path FROM keyword WHERE id = ?"));
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO asset_tag(asset_id, name) VALUES (?, ?)"));
    for (const auto &keyword_id : keyword_ids)
    {
        lookup.addBindValue(qstring_from_utf8(keyword_id));
        if (!lookup.exec() || !lookup.next())
            return map_sql_error(lookup, "lookup_keyword_path");
        insert.addBindValue(qstring_from_utf8(asset_id));
        insert.addBindValue(lookup.value(0));
        if (!insert.exec())
            return map_sql_error(insert, "insert_asset_tag_projection");
        insert.finish();
        lookup.finish();
    }
    return {};
}

[[nodiscard]] Result<void> rebuild_projections_for_keyword_subtree(QSqlDatabase &database,
                                                                   std::string_view root_path)
{
    QSqlQuery assets(database);
    assets.prepare(QStringLiteral("SELECT DISTINCT ak.asset_id FROM asset_keyword ak "
                                  "INNER JOIN keyword k ON k.id = ak.keyword_id "
                                  "WHERE k.path = ? OR k.path LIKE ?"));
    assets.addBindValue(qstring_from_utf8(root_path));
    assets.addBindValue(qstring_from_utf8(std::string(root_path) + kKeywordPathSeparator + "%"));
    if (!assets.exec())
        return map_sql_error(assets, "list_keyword_subtree_assets");
    while (assets.next())
    {
        const auto asset_id = utf8_from_qstring(assets.value(0).toString());
        auto rebuilt = rebuild_asset_tag_projection(database, asset_id);
        if (!rebuilt)
            return rebuilt.error();
    }
    return {};
}

[[nodiscard]] Result<KeywordRecord> insert_keyword_row(QSqlDatabase &database,
                                                       std::string_view name,
                                                       const std::optional<std::string> &parent_id,
                                                       std::string_view path, int depth)
{
    KeywordRecord record;
    record.id = generate_keyword_id();
    record.parent_id = parent_id;
    record.name = std::string(name);
    record.path = std::string(path);
    record.depth = depth;
    const auto now = now_unix_ms();
    record.created_unix_ms = now;
    record.updated_unix_ms = now;
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO keyword(id, parent_id, name, path, depth, created_unix_ms, updated_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(qstring_from_utf8(record.id));
    if (parent_id)
        insert.addBindValue(qstring_from_utf8(*parent_id));
    else
        insert.addBindValue(QVariant{});
    insert.addBindValue(qstring_from_utf8(record.name));
    insert.addBindValue(qstring_from_utf8(record.path));
    insert.addBindValue(depth);
    insert.addBindValue(static_cast<qlonglong>(now));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
    {
        const auto error = map_sql_error(insert, "insert_keyword");
        if (error.code == ErrorCode::kConflict ||
            utf8_from_qstring(insert.lastError().text()).find("UNIQUE") != std::string::npos)
        {
            return make_error(ErrorCode::kConflict, "Keyword already exists under that parent",
                              {{"reason", "duplicate_keyword"}, {"path", record.path}});
        }
        return error;
    }
    return record;
}

[[nodiscard]] Result<KeywordRecord> ensure_keyword_path(QSqlDatabase &database,
                                                        const std::vector<std::string> &segments)
{
    std::optional<std::string> parent_id;
    std::string path;
    KeywordRecord current;
    for (std::size_t index = 0; index < segments.size(); ++index)
    {
        if (index != 0)
            path.push_back(kKeywordPathSeparator);
        path += segments[index];
        auto existing = load_keyword_by_path(database, path);
        if (!existing)
            return existing.error();
        if (existing.value())
        {
            current = *existing.value();
            parent_id = current.id;
            continue;
        }
        auto counted = count_keywords(database);
        if (!counted)
            return counted.error();
        if (counted.value() >= kKeywordMaximumCount)
        {
            return make_error(
                ErrorCode::kValidation, "Catalog already has the maximum number of keywords",
                {{"reason", "keyword_limit"}, {"maximum", std::to_string(kKeywordMaximumCount)}});
        }
        auto created =
            insert_keyword_row(database, segments[index], parent_id, path, static_cast<int>(index));
        if (!created)
            return created.error();
        current = std::move(created).value();
        parent_id = current.id;
    }
    return current;
}

[[nodiscard]] Result<void> replace_one_asset_membership(QSqlDatabase &database,
                                                        std::string_view asset_id,
                                                        const std::vector<std::string> &keyword_ids)
{
    QSqlQuery clear_links(database);
    clear_links.prepare(QStringLiteral("DELETE FROM asset_keyword WHERE asset_id = ?"));
    clear_links.addBindValue(qstring_from_utf8(asset_id));
    if (!clear_links.exec())
        return map_sql_error(clear_links, "clear_asset_keywords");

    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO asset_keyword(asset_id, keyword_id) VALUES (?, ?)"));
    for (const auto &keyword_id : keyword_ids)
    {
        insert.addBindValue(qstring_from_utf8(asset_id));
        insert.addBindValue(qstring_from_utf8(keyword_id));
        if (!insert.exec())
            return map_sql_error(insert, "insert_asset_keyword");
        insert.finish();
    }
    return write_asset_tag_projection(database, asset_id, keyword_ids);
}

[[nodiscard]] Result<std::vector<std::string>>
canonicalize_tag_paths(const std::vector<std::string> &tag_paths)
{
    std::vector<std::string> paths;
    paths.reserve(tag_paths.size());
    for (const auto &raw : tag_paths)
    {
        auto segments = parse_keyword_path(raw);
        if (!segments)
            return segments.error();
        auto joined = join_keyword_path(segments.value());
        if (!joined)
            return joined.error();
        if (std::find(paths.begin(), paths.end(), joined.value()) == paths.end())
            paths.push_back(std::move(joined).value());
    }
    return paths;
}

[[nodiscard]] Result<void> update_subtree_paths(QSqlDatabase &database, const KeywordRecord &node,
                                                std::string_view new_path, int new_depth)
{
    const auto old_path = node.path;
    const auto now = now_unix_ms();
    QSqlQuery self(database);
    self.prepare(
        QStringLiteral("UPDATE keyword SET path = ?, depth = ?, updated_unix_ms = ? WHERE id = ?"));
    self.addBindValue(qstring_from_utf8(new_path));
    self.addBindValue(new_depth);
    self.addBindValue(static_cast<qlonglong>(now));
    self.addBindValue(qstring_from_utf8(node.id));
    if (!self.exec())
        return map_sql_error(self, "update_keyword_path");

    QSqlQuery children(database);
    children.prepare(
        QStringLiteral("SELECT id, parent_id, name, path, depth, created_unix_ms, updated_unix_ms "
                       "FROM keyword WHERE path LIKE ? ORDER BY depth ASC, path ASC"));
    children.addBindValue(qstring_from_utf8(old_path + kKeywordPathSeparator + "%"));
    if (!children.exec())
        return map_sql_error(children, "list_keyword_descendants");
    while (children.next())
    {
        auto child = read_keyword(children);
        if (child.path.size() < old_path.size() ||
            child.path.compare(0, old_path.size(), old_path) != 0)
        {
            return make_error(ErrorCode::kValidation, "Keyword descendant path is inconsistent",
                              {{"reason", "invalid_keyword_descendant"}, {"path", child.path}});
        }
        std::string suffix = child.path.substr(old_path.size());
        std::string updated = std::string(new_path) + suffix;
        const int depth_delta = new_depth - node.depth;
        const int updated_depth = child.depth + depth_delta;
        if (updated_depth < 0 || static_cast<std::size_t>(updated_depth) >= kKeywordMaximumDepth)
        {
            return make_error(ErrorCode::kValidation, "Keyword move exceeds the maximum depth",
                              {{"reason", "keyword_path_too_deep"},
                               {"maximum", std::to_string(kKeywordMaximumDepth)}});
        }
        if (updated.size() > kKeywordPathMaxLength)
        {
            return make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum length",
                              {{"reason", "keyword_path_too_long"}});
        }
        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE keyword SET path = ?, depth = ?, updated_unix_ms = ? WHERE id = ?"));
        update.addBindValue(qstring_from_utf8(updated));
        update.addBindValue(updated_depth);
        update.addBindValue(static_cast<qlonglong>(now));
        update.addBindValue(qstring_from_utf8(child.id));
        if (!update.exec())
            return map_sql_error(update, "update_keyword_descendant_path");
    }
    return {};
}

} // namespace

Result<std::vector<KeywordRecord>> SqliteCatalogRepository::list_keywords() const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    if (!query.exec(QStringLiteral(
            "SELECT id, parent_id, name, path, depth, created_unix_ms, updated_unix_ms "
            "FROM keyword ORDER BY path COLLATE NOCASE")))
        return map_sql_error(query, "list_keywords");
    std::vector<KeywordRecord> records;
    while (query.next())
        records.push_back(read_keyword(query));
    return records;
}

Result<std::optional<KeywordRecord>>
SqliteCatalogRepository::find_keyword_by_id(const std::string_view keyword_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    return load_keyword_by_id(impl_->database, keyword_id);
}

Result<std::optional<KeywordRecord>>
SqliteCatalogRepository::find_keyword_by_path(const std::string_view path) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto segments = parse_keyword_path(path);
    if (!segments)
        return segments.error();
    auto joined = join_keyword_path(segments.value());
    if (!joined)
        return joined.error();
    return load_keyword_by_path(impl_->database, joined.value());
}

Result<KeywordMutation>
SqliteCatalogRepository::create_keyword(const std::string_view name,
                                        const std::optional<std::string_view> parent_id,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto normalized = normalize_keyword_name(name);
    if (!normalized)
        return normalized.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start keyword create transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok =
        require_revision(impl_->database, expected_revision, "read_create_keyword_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());

    std::optional<std::string> parent;
    int depth = 0;
    std::string path = normalized.value();
    if (parent_id)
    {
        auto parent_row = load_keyword_by_id(impl_->database, *parent_id);
        if (!parent_row)
            return impl_->abort_transaction(parent_row.error());
        if (!parent_row.value())
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kNotFound, "Keyword parent does not exist",
                {{"reason", "unknown_keyword_parent"}, {"parent_id", std::string(*parent_id)}}));
        }
        parent = parent_row.value()->id;
        depth = parent_row.value()->depth + 1;
        if (static_cast<std::size_t>(depth) >= kKeywordMaximumDepth)
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kValidation, "Keyword path exceeds the maximum depth",
                           {{"reason", "keyword_path_too_deep"},
                            {"maximum", std::to_string(kKeywordMaximumDepth)}}));
        }
        path = parent_row.value()->path + kKeywordPathSeparator + normalized.value();
        if (path.size() > kKeywordPathMaxLength)
        {
            return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                       "Keyword path exceeds the maximum length",
                                                       {{"reason", "keyword_path_too_long"}}));
        }
    }
    auto counted = count_keywords(impl_->database);
    if (!counted)
        return impl_->abort_transaction(counted.error());
    if (counted.value() >= kKeywordMaximumCount)
    {
        return impl_->abort_transaction(make_error(
            ErrorCode::kValidation, "Catalog already has the maximum number of keywords",
            {{"reason", "keyword_limit"}, {"maximum", std::to_string(kKeywordMaximumCount)}}));
    }
    auto created = insert_keyword_row(impl_->database, normalized.value(), parent, path, depth);
    if (!created)
        return impl_->abort_transaction(created.error());
    auto revision = bump_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit keyword create",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    return KeywordMutation{std::move(created).value(), revision.value()};
}

Result<KeywordMutation>
SqliteCatalogRepository::rename_keyword(const std::string_view keyword_id,
                                        const std::string_view name,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto normalized = normalize_keyword_name(name);
    if (!normalized)
        return normalized.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start keyword rename transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok =
        require_revision(impl_->database, expected_revision, "read_rename_keyword_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());
    auto existing = load_keyword_by_id(impl_->database, keyword_id);
    if (!existing)
        return impl_->abort_transaction(existing.error());
    if (!existing.value())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Keyword does not exist",
                       {{"reason", "unknown_keyword"}, {"keyword_id", std::string(keyword_id)}}));
    }
    auto node = *existing.value();
    if (node.name == normalized.value())
    {
        auto revision = read_revision(impl_->database, "read_rename_noop_revision");
        if (!revision)
            return impl_->abort_transaction(revision.error());
        if (!impl_->database.commit())
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Unable to commit keyword rename",
                           {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
        }
        return KeywordMutation{std::move(node), revision.value()};
    }
    std::string new_path = normalized.value();
    if (node.parent_id)
    {
        auto parent = load_keyword_by_id(impl_->database, *node.parent_id);
        if (!parent || !parent.value())
        {
            return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                       "Keyword parent does not exist",
                                                       {{"reason", "unknown_keyword_parent"}}));
        }
        new_path = parent.value()->path + kKeywordPathSeparator + normalized.value();
    }
    if (new_path.size() > kKeywordPathMaxLength)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                   "Keyword path exceeds the maximum length",
                                                   {{"reason", "keyword_path_too_long"}}));
    }
    const auto old_path = node.path;
    QSqlQuery rename(impl_->database);
    rename.prepare(QStringLiteral("UPDATE keyword SET name = ?, updated_unix_ms = ? WHERE id = ?"));
    rename.addBindValue(qstring_from_utf8(normalized.value()));
    rename.addBindValue(static_cast<qlonglong>(now_unix_ms()));
    rename.addBindValue(qstring_from_utf8(node.id));
    if (!rename.exec())
        return impl_->abort_transaction(map_sql_error(rename, "rename_keyword"));
    auto paths = update_subtree_paths(impl_->database, node, new_path, node.depth);
    if (!paths)
        return impl_->abort_transaction(paths.error());
    auto rebuilt = rebuild_projections_for_keyword_subtree(impl_->database, new_path);
    if (!rebuilt)
    {
        // Also rebuild any assets that still referenced the old path projection via membership.
        rebuilt = rebuild_projections_for_keyword_subtree(impl_->database, old_path);
        if (!rebuilt)
            return impl_->abort_transaction(rebuilt.error());
    }
    // Membership IDs unchanged; rebuild using new path prefix coverage.
    rebuilt = rebuild_projections_for_keyword_subtree(impl_->database, new_path);
    if (!rebuilt)
        return impl_->abort_transaction(rebuilt.error());
    auto refreshed = load_keyword_by_id(impl_->database, keyword_id);
    if (!refreshed || !refreshed.value())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Keyword disappeared after rename"));
    auto revision = bump_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit keyword rename",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    return KeywordMutation{*refreshed.value(), revision.value()};
}

Result<KeywordMutation>
SqliteCatalogRepository::move_keyword(const std::string_view keyword_id,
                                      const std::optional<std::string_view> parent_id,
                                      const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start keyword move transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok =
        require_revision(impl_->database, expected_revision, "read_move_keyword_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());
    auto existing = load_keyword_by_id(impl_->database, keyword_id);
    if (!existing)
        return impl_->abort_transaction(existing.error());
    if (!existing.value())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Keyword does not exist",
                       {{"reason", "unknown_keyword"}, {"keyword_id", std::string(keyword_id)}}));
    }
    auto node = *existing.value();
    std::optional<std::string> new_parent;
    int new_depth = 0;
    std::string new_path = node.name;
    if (parent_id)
    {
        if (*parent_id == keyword_id)
        {
            return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                       "Keyword cannot be its own parent",
                                                       {{"reason", "keyword_cycle"}}));
        }
        auto parent = load_keyword_by_id(impl_->database, *parent_id);
        if (!parent)
            return impl_->abort_transaction(parent.error());
        if (!parent.value())
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kNotFound, "Keyword parent does not exist",
                {{"reason", "unknown_keyword_parent"}, {"parent_id", std::string(*parent_id)}}));
        }
        if (parent.value()->path == node.path ||
            (parent.value()->path.size() > node.path.size() &&
             parent.value()->path.compare(0, node.path.size(), node.path) == 0 &&
             parent.value()->path[node.path.size()] == kKeywordPathSeparator))
        {
            return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                       "Keyword move would create a cycle",
                                                       {{"reason", "keyword_cycle"}}));
        }
        new_parent = parent.value()->id;
        new_depth = parent.value()->depth + 1;
        new_path = parent.value()->path + kKeywordPathSeparator + node.name;
    }
    if (static_cast<std::size_t>(new_depth) >= kKeywordMaximumDepth)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kValidation, "Keyword move exceeds the maximum depth",
                       {{"reason", "keyword_path_too_deep"},
                        {"maximum", std::to_string(kKeywordMaximumDepth)}}));
    }
    if (new_path.size() > kKeywordPathMaxLength)
    {
        return impl_->abort_transaction(make_error(ErrorCode::kValidation,
                                                   "Keyword path exceeds the maximum length",
                                                   {{"reason", "keyword_path_too_long"}}));
    }
    const auto old_path = node.path;
    QSqlQuery reparent(impl_->database);
    reparent.prepare(
        QStringLiteral("UPDATE keyword SET parent_id = ?, updated_unix_ms = ? WHERE id = ?"));
    if (new_parent)
        reparent.addBindValue(qstring_from_utf8(*new_parent));
    else
        reparent.addBindValue(QVariant{});
    reparent.addBindValue(static_cast<qlonglong>(now_unix_ms()));
    reparent.addBindValue(qstring_from_utf8(node.id));
    if (!reparent.exec())
        return impl_->abort_transaction(map_sql_error(reparent, "move_keyword"));
    auto paths = update_subtree_paths(impl_->database, node, new_path, new_depth);
    if (!paths)
        return impl_->abort_transaction(paths.error());
    auto rebuilt = rebuild_projections_for_keyword_subtree(impl_->database, new_path);
    if (!rebuilt)
        return impl_->abort_transaction(rebuilt.error());
    auto refreshed = load_keyword_by_id(impl_->database, keyword_id);
    if (!refreshed || !refreshed.value())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Keyword disappeared after move"));
    auto revision = bump_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit keyword move",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    (void)old_path;
    return KeywordMutation{*refreshed.value(), revision.value()};
}

Result<std::int64_t>
SqliteCatalogRepository::delete_keyword(const std::string_view keyword_id, const bool recursive,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start keyword delete transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok =
        require_revision(impl_->database, expected_revision, "read_delete_keyword_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());
    auto existing = load_keyword_by_id(impl_->database, keyword_id);
    if (!existing)
        return impl_->abort_transaction(existing.error());
    if (!existing.value())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Keyword does not exist",
                       {{"reason", "unknown_keyword"}, {"keyword_id", std::string(keyword_id)}}));
    }
    const auto path = existing.value()->path;
    if (!recursive)
    {
        QSqlQuery children(impl_->database);
        children.prepare(QStringLiteral("SELECT COUNT(*) FROM keyword WHERE parent_id = ?"));
        children.addBindValue(qstring_from_utf8(keyword_id));
        if (!children.exec() || !children.next())
            return impl_->abort_transaction(map_sql_error(children, "count_keyword_children"));
        if (children.value(0).toLongLong() > 0)
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kValidation, "Keyword still has children",
                {{"reason", "keyword_has_children"}, {"keyword_id", std::string(keyword_id)}}));
        }
    }
    QSqlQuery assets(impl_->database);
    assets.prepare(QStringLiteral("SELECT DISTINCT ak.asset_id FROM asset_keyword ak "
                                  "INNER JOIN keyword k ON k.id = ak.keyword_id "
                                  "WHERE k.path = ? OR k.path LIKE ?"));
    assets.addBindValue(qstring_from_utf8(path));
    assets.addBindValue(qstring_from_utf8(path + kKeywordPathSeparator + "%"));
    if (!assets.exec())
        return impl_->abort_transaction(map_sql_error(assets, "list_delete_keyword_assets"));
    std::vector<std::string> affected;
    while (assets.next())
        affected.push_back(utf8_from_qstring(assets.value(0).toString()));

    QSqlQuery remove(impl_->database);
    remove.prepare(QStringLiteral("DELETE FROM keyword WHERE id = ?"));
    remove.addBindValue(qstring_from_utf8(keyword_id));
    if (!remove.exec())
        return impl_->abort_transaction(map_sql_error(remove, "delete_keyword"));

    for (const auto &asset_id : affected)
    {
        auto rebuilt = rebuild_asset_tag_projection(impl_->database, asset_id);
        if (!rebuilt)
            return impl_->abort_transaction(rebuilt.error());
    }
    auto revision = bump_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit keyword delete",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    return revision.value();
}

Result<void> SqliteCatalogRepository::replace_asset_tags(const std::string_view asset_id,
                                                         const std::vector<std::string> &tags)
{
    auto mutated = replace_assets_tags({std::string(asset_id)}, tags, std::nullopt);
    if (!mutated)
        return mutated.error();
    return {};
}

Result<KeywordMembershipMutation>
SqliteCatalogRepository::replace_assets_tags(const std::vector<std::string> &asset_ids,
                                             const std::vector<std::string> &tag_paths,
                                             const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (asset_ids.empty())
    {
        return make_error(ErrorCode::kValidation, "Tag replacement requires at least one asset",
                          {{"reason", "empty_tag_asset_list"}});
    }
    auto paths = canonicalize_tag_paths(tag_paths);
    if (!paths)
        return paths.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start tag replacement transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok = require_revision(impl_->database, expected_revision, "read_tag_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());

    std::vector<std::string> unique_assets;
    unique_assets.reserve(asset_ids.size());
    std::set<std::string, std::less<>> seen;
    for (const auto &asset_id : asset_ids)
    {
        if (asset_id.empty() || !seen.insert(asset_id).second)
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kValidation, "Tag replacement asset list is invalid",
                           {{"reason", "invalid_tag_asset_list"}, {"asset_id", asset_id}}));
        }
        unique_assets.push_back(asset_id);
    }

    QSqlQuery existing(impl_->database);
    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(unique_assets.size()));
    for (std::size_t index = 0; index < unique_assets.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    existing.prepare(QStringLiteral("SELECT COUNT(*) FROM asset WHERE id IN (") +
                     placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const auto &asset_id : unique_assets)
        existing.addBindValue(qstring_from_utf8(asset_id));
    if (!existing.exec() || !existing.next())
        return impl_->abort_transaction(map_sql_error(existing, "verify_tag_assets"));
    if (static_cast<std::size_t>(existing.value(0).toLongLong()) != unique_assets.size())
    {
        return impl_->abort_transaction(make_error(ErrorCode::kNotFound,
                                                   "Tagged asset does not exist",
                                                   {{"reason", "unknown_tag_asset"}}));
    }

    std::vector<std::string> keyword_ids;
    keyword_ids.reserve(paths.value().size());
    for (const auto &path : paths.value())
    {
        auto segments = parse_keyword_path(path);
        if (!segments)
            return impl_->abort_transaction(segments.error());
        auto ensured = ensure_keyword_path(impl_->database, segments.value());
        if (!ensured)
            return impl_->abort_transaction(ensured.error());
        keyword_ids.push_back(ensured.value().id);
    }

    for (const auto &asset_id : unique_assets)
    {
        auto replaced = replace_one_asset_membership(impl_->database, asset_id, keyword_ids);
        if (!replaced)
            return impl_->abort_transaction(replaced.error());
    }

    auto revision = bump_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit tag replacement",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }

    KeywordMembershipMutation mutation;
    mutation.revision = revision.value();
    mutation.assets.reserve(unique_assets.size());
    for (const auto &asset_id : unique_assets)
    {
        auto asset = find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(ErrorCode::kNotFound, "Tagged asset does not exist",
                              {{"reason", "unknown_tag_asset"}, {"asset_id", asset_id}});
        }
        mutation.assets.push_back(std::move(*asset.value()));
    }
    return mutation;
}

} // namespace ravo
