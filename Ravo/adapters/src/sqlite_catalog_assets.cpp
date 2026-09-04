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
    if (request.collapse_stacks)
    {
        predicates.push_back(
            QStringLiteral("a.id NOT IN (SELECT m.asset_id FROM library_stack_member m "
                           "INNER JOIN library_stack s ON s.id = m.stack_id "
                           "WHERE m.asset_id <> s.pick_asset_id)"));
    }

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

Result<std::vector<std::string>>
SqliteCatalogRepository::list_version_asset_ids(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "SELECT id FROM asset WHERE source_asset_id = ? ORDER BY version_ordinal ASC, id ASC"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
        return map_sql_error(query, "list_version_asset_ids");
    std::vector<std::string> ids;
    while (query.next())
        ids.push_back(utf8_from_qstring(query.value(0).toString()));
    return ids;
}

Result<std::optional<AssetRecord>>
SqliteCatalogRepository::find_asset_by_uri(const std::string_view normalized_uri) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QString(kAssetSelect) +
                  QStringLiteral(" WHERE normalized_uri = ? AND version_ordinal = 0"));
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
        "created_unix_ms, rating, color_label, rejected, picked, version_ordinal, source_asset_id) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
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
    query.addBindValue(asset.review.picked ? 1 : 0);
    query.addBindValue(asset.version_ordinal);
    query.addBindValue(optional_string(asset.source_asset_id));
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
        "error_message = ?, rating = ?, color_label = ?, rejected = ?, picked = ? WHERE id = ?"));
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
    query.addBindValue(asset.review.picked ? 1 : 0);
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
    auto valid = validate_review_state(review);
    if (!valid)
    {
        return valid.error();
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "UPDATE asset SET rating = ?, color_label = ?, rejected = ?, picked = ? WHERE id = ?"));
    query.addBindValue(review.rating);
    query.addBindValue(qstring_from_utf8(color_label_name(review.color_label)));
    query.addBindValue(review.rejected ? 1 : 0);
    query.addBindValue(review.picked ? 1 : 0);
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
    const auto repair_stack = [&](const std::string_view id) -> Result<void>
    {
        QSqlQuery member(impl_->database);
        member.prepare(QStringLiteral(
            "SELECT m.stack_id, s.pick_asset_id, "
            "(SELECT COUNT(*) FROM library_stack_member c WHERE c.stack_id = m.stack_id) "
            "FROM library_stack_member m INNER JOIN library_stack s ON s.id = m.stack_id "
            "WHERE m.asset_id = ?"));
        member.addBindValue(qstring_from_utf8(id));
        if (!member.exec())
            return map_sql_error(member, "read_remove_stack");
        if (!member.next())
            return {};
        const auto stack_id = member.value(0).toString();
        const auto pick = utf8_from_qstring(member.value(1).toString());
        const auto count = member.value(2).toInt();
        if (count <= 2)
        {
            QSqlQuery drop(impl_->database);
            drop.prepare(QStringLiteral("DELETE FROM library_stack WHERE id = ?"));
            drop.addBindValue(stack_id);
            if (!drop.exec())
                return map_sql_error(drop, "dissolve_stack");
            return {};
        }
        if (pick == id)
        {
            QSqlQuery next_pick(impl_->database);
            next_pick.prepare(QStringLiteral(
                "SELECT asset_id FROM library_stack_member WHERE stack_id = ? AND asset_id <> ? "
                "ORDER BY position ASC, asset_id ASC LIMIT 1"));
            next_pick.addBindValue(stack_id);
            next_pick.addBindValue(qstring_from_utf8(id));
            if (!next_pick.exec() || !next_pick.next())
                return map_sql_error(next_pick, "reassign_stack_pick");
            QSqlQuery update(impl_->database);
            update.prepare(
                QStringLiteral("UPDATE library_stack SET pick_asset_id = ? WHERE id = ?"));
            update.addBindValue(next_pick.value(0));
            update.addBindValue(stack_id);
            if (!update.exec())
                return map_sql_error(update, "update_stack_pick");
        }
        return {};
    };
    QSqlQuery versions(impl_->database);
    versions.prepare(QStringLiteral("SELECT id FROM asset WHERE source_asset_id = ?"));
    versions.addBindValue(qstring_from_utf8(asset_id));
    if (!versions.exec())
        return impl_->abort_transaction(map_sql_error(versions, "list_asset_versions"));
    std::vector<std::string> version_ids;
    while (versions.next())
        version_ids.push_back(utf8_from_qstring(versions.value(0).toString()));
    for (const auto &version_id : version_ids)
    {
        auto repaired = repair_stack(version_id);
        if (!repaired)
            return impl_->abort_transaction(repaired.error());
        QSqlQuery drop_version(impl_->database);
        drop_version.prepare(QStringLiteral("DELETE FROM asset WHERE id = ?"));
        drop_version.addBindValue(qstring_from_utf8(version_id));
        if (!drop_version.exec())
            return impl_->abort_transaction(map_sql_error(drop_version, "remove_asset_version"));
    }
    auto repaired = repair_stack(asset_id);
    if (!repaired)
        return impl_->abort_transaction(repaired.error());
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

} // namespace ravo
