#include "ravo/adapters/sqlite_catalog.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

#include "catalog_sql_internal.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{

constexpr std::string_view kSupportMarker = ".ravo/";
constexpr const char *kKnownSupportPrefixes[] = {"derived/", "external-editor/", "sidecars/"};

constexpr const char *kAssetRecoveryUpdateTrigger =
    "CREATE TRIGGER asset_recovery_update AFTER UPDATE OF normalized_uri, media_type, size_bytes, "
    "mtime_unix_ms, content_fingerprint, width, height, import_state, error_code, error_message, "
    "created_unix_ms, rating, color_label, rejected, picked ON asset BEGIN "
    "  UPDATE asset_recovery_state SET generation = generation + 1 WHERE asset_id = NEW.id; "
    "END";

using sqlite_internal::map_sql_error;
using sqlite_internal::next_connection_name;
using sqlite_internal::qstring_from_utf8;
using sqlite_internal::utf8_from_qstring;

[[nodiscard]] TaskError uri_error(const ErrorCode code, std::string message, std::string reason,
                                  std::string path = {}, std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] bool known_support_suffix(const std::string_view suffix) noexcept
{
    for (const char *prefix : kKnownSupportPrefixes)
    {
        const std::string_view known(prefix);
        if (suffix.size() >= known.size() && suffix.compare(0, known.size(), known) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] Result<std::string>
rewrite_path_or_uri(const std::string_view value, const std::string_view destination_support_root)
{
    if (value.empty())
        return std::string{};

    const auto marker = value.find(kSupportMarker);
    if (marker == std::string_view::npos)
        return std::string(value);

    const auto suffix = value.substr(marker + kSupportMarker.size());
    if (!known_support_suffix(suffix))
    {
        return uri_error(ErrorCode::kValidation,
                         "Support-rooted URI points outside known catalog support roots",
                         "restore_support_uri_outside_known_roots", std::string(value));
    }

    if (value.starts_with("file:"))
    {
        auto location = normalize_local_input(value);
        if (!location)
            return location.error();
        const auto path_marker = location.value().path.find(kSupportMarker);
        if (path_marker == std::string::npos)
        {
            return uri_error(ErrorCode::kValidation,
                             "Support-rooted URI could not be resolved to a path prefix",
                             "restore_support_uri_unresolved", std::string(value));
        }
        const auto path_suffix = location.value().path.substr(path_marker + kSupportMarker.size());
        if (!known_support_suffix(path_suffix))
        {
            return uri_error(ErrorCode::kValidation,
                             "Support-rooted URI points outside known catalog support roots",
                             "restore_support_uri_outside_known_roots", std::string(value));
        }
        const std::string rewritten_path =
            std::string(destination_support_root) + "/" + std::string(path_suffix);
        auto rewritten = normalize_local_input(rewritten_path);
        if (!rewritten)
            return rewritten.error();
        return rewritten.value().uri;
    }

    return std::string(destination_support_root) + "/" + std::string(suffix);
}

} // namespace

Result<std::size_t>
sqlite_rewrite_support_rooted_uris(const std::string_view catalog_path,
                                   const std::string_view destination_support_root,
                                   const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (catalog_path.empty() || destination_support_root.empty())
    {
        return uri_error(ErrorCode::kInvalidArgument, "Catalog URI rewrite paths must not be empty",
                         "invalid_restore_uri_rewrite_request");
    }

    const QString connection = next_connection_name();
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(qstring_from_utf8(catalog_path));
    if (!database.open())
    {
        const auto detail = utf8_from_qstring(database.lastError().text());
        QSqlDatabase::removeDatabase(connection);
        return uri_error(ErrorCode::kIo, "Unable to open staged catalog for URI rewrite",
                         "restore_uri_catalog_open_failed", std::string(catalog_path), detail);
    }

    auto fail = [&](TaskError error) -> Result<std::size_t>
    {
        database.close();
        QSqlDatabase::removeDatabase(connection);
        return error;
    };

    if (!database.transaction())
    {
        return fail(uri_error(ErrorCode::kIo, "Unable to start URI rewrite transaction",
                              "restore_uri_transaction_failed", std::string(catalog_path),
                              utf8_from_qstring(database.lastError().text())));
    }

    QSqlQuery drop(database);
    if (!drop.exec(QStringLiteral("DROP TRIGGER IF EXISTS asset_recovery_update")))
    {
        database.rollback();
        return fail(map_sql_error(drop, "restore_uri_drop_trigger"));
    }

    std::size_t rewritten = 0U;

    QSqlQuery folders(database);
    if (!folders.exec(QStringLiteral("SELECT id, uri FROM catalog_folder")))
    {
        database.rollback();
        return fail(map_sql_error(folders, "restore_uri_folder_read"));
    }
    while (folders.next())
    {
        active = cancellation.check();
        if (!active)
        {
            database.rollback();
            return fail(active.error());
        }
        const auto id = utf8_from_qstring(folders.value(0).toString());
        const auto uri = utf8_from_qstring(folders.value(1).toString());
        auto next = rewrite_path_or_uri(uri, destination_support_root);
        if (!next)
        {
            database.rollback();
            return fail(next.error());
        }
        if (next.value() == uri)
            continue;
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE catalog_folder SET uri = ? WHERE id = ?"));
        update.addBindValue(qstring_from_utf8(next.value()));
        update.addBindValue(qstring_from_utf8(id));
        if (!update.exec())
        {
            database.rollback();
            return fail(map_sql_error(update, "restore_uri_folder_update"));
        }
        ++rewritten;
    }

    QSqlQuery assets(database);
    if (!assets.exec(
            QStringLiteral("SELECT id, normalized_uri, folder_uri, display_name FROM asset")))
    {
        database.rollback();
        return fail(map_sql_error(assets, "restore_uri_asset_read"));
    }
    while (assets.next())
    {
        active = cancellation.check();
        if (!active)
        {
            database.rollback();
            return fail(active.error());
        }
        const auto id = utf8_from_qstring(assets.value(0).toString());
        const auto normalized = utf8_from_qstring(assets.value(1).toString());
        const auto folder = utf8_from_qstring(assets.value(2).toString());
        auto next_uri = rewrite_path_or_uri(normalized, destination_support_root);
        if (!next_uri)
        {
            database.rollback();
            return fail(next_uri.error());
        }
        auto next_folder = rewrite_path_or_uri(folder, destination_support_root);
        if (!next_folder)
        {
            database.rollback();
            return fail(next_folder.error());
        }
        if (next_uri.value() == normalized && next_folder.value() == folder)
            continue;

        std::string display = utf8_from_qstring(assets.value(3).toString());
        if (next_uri.value() != normalized)
            display = uri_display_name(next_uri.value());

        QSqlQuery update(database);
        update.prepare(QStringLiteral(
            "UPDATE asset SET normalized_uri = ?, folder_uri = ?, display_name = ? WHERE id = ?"));
        update.addBindValue(qstring_from_utf8(next_uri.value()));
        update.addBindValue(qstring_from_utf8(next_folder.value()));
        update.addBindValue(qstring_from_utf8(display));
        update.addBindValue(qstring_from_utf8(id));
        if (!update.exec())
        {
            database.rollback();
            return fail(map_sql_error(update, "restore_uri_asset_update"));
        }
        ++rewritten;
    }

    QSqlQuery recreate(database);
    if (!recreate.exec(QString::fromUtf8(kAssetRecoveryUpdateTrigger)))
    {
        database.rollback();
        return fail(map_sql_error(recreate, "restore_uri_recreate_trigger"));
    }

    if (!database.commit())
    {
        return fail(uri_error(ErrorCode::kIo, "Unable to commit URI rewrite",
                              "restore_uri_commit_failed", std::string(catalog_path),
                              utf8_from_qstring(database.lastError().text())));
    }

    database.close();
    QSqlDatabase::removeDatabase(connection);
    return rewritten;
}

} // namespace ravo
