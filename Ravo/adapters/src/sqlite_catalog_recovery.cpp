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

} // namespace ravo
