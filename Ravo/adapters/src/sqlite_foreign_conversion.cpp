#include "ravo/adapters/sqlite_catalog.h"
#include "catalog_sql_internal.h"

#include <QSqlError>
#include <exception>

namespace ravo
{
using namespace sqlite_internal;

Result<std::vector<std::string>> SqliteCatalogRepository::list_foreign_conversion_ids() const
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    QSqlQuery query(impl_->database);
    if (!query.exec("SELECT id FROM foreign_conversion ORDER BY id LIMIT 1001"))
        return map_sql_error(query, "list_foreign_conversions");
    std::vector<std::string> ids;
    while (query.next())
    {
        if (ids.size() >= 1000)
            return make_error(ErrorCode::kUnsupported, "Conversion listing exceeds bound");
        ids.push_back(query.value(0).toString().toStdString());
    }
    if (query.lastError().isValid())
        return map_sql_error(query, "list_foreign_conversions");
    return ids;
}

Result<std::optional<ForeignConversionJournal>>
SqliteCatalogRepository::load_foreign_conversion(const std::string_view id) const
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    QSqlQuery query(impl_->database);
    query.prepare("SELECT source_sha256,catalog_revision FROM foreign_conversion WHERE id=?");
    query.addBindValue(qstring_from_utf8(id));
    if (!query.exec())
        return map_sql_error(query, "load_foreign_conversion");
    if (!query.next())
    {
        if (query.lastError().isValid())
            return map_sql_error(query, "load_foreign_conversion");
        return std::optional<ForeignConversionJournal>{};
    }
    ForeignConversionJournal journal{std::string(id),
                                     query.value(0).toString().toStdString(),
                                     query.value(1).toLongLong(),
                                     {},
                                     {}};
    query.prepare(
        "SELECT foreign_id,phase,asset_id,complete,receipt_json FROM foreign_conversion_record "
        "WHERE conversion_id=? ORDER BY foreign_id");
    query.addBindValue(qstring_from_utf8(id));
    if (!query.exec())
        return map_sql_error(query, "load_foreign_conversion_records");
    std::size_t receipt_bytes = 0;
    while (query.next())
    {
        if (journal.records.size() >= 2000000)
            return make_error(ErrorCode::kValidation, "Foreign conversion journal exceeds bound");
        ForeignConversionCheckpoint record;
        record.foreign_id = query.value(0).toString().toStdString();
        record.phase = query.value(1).toString().toStdString();
        if (!query.value(2).isNull())
            record.asset_id = query.value(2).toString().toStdString();
        record.complete = query.value(3).toBool();
        record.receipt_json = query.value(4).toString().toStdString();
        receipt_bytes += record.receipt_json.size();
        if (receipt_bytes > 512000000 || record.receipt_json.size() > 16000000)
            return make_error(ErrorCode::kValidation, "Conversion receipts exceed reader bound");
        journal.records.push_back(std::move(record));
    }
    if (query.lastError().isValid())
        return map_sql_error(query, "load_foreign_conversion_records");
    query.prepare(
        "SELECT foreign_id,phase,target_id,revision FROM foreign_conversion_commit WHERE conversion_id=? ORDER BY revision");
    query.addBindValue(qstring_from_utf8(id));
    if (!query.exec())
        return map_sql_error(query, "load_foreign_commit_proofs");
    std::size_t proof_bytes = 0;
    while (query.next())
    {
        ForeignConversionJournal::Commit proof;
        proof.foreign_id = query.value(0).toString().toStdString();
        proof.phase = query.value(1).toString().toStdString();
        if (!query.value(2).isNull())
            proof.target_id = query.value(2).toString().toStdString();
        proof.revision = query.value(3).toLongLong();
        proof_bytes += proof.foreign_id.size() + proof.phase.size() +
                       proof.target_id.value_or("").size() + sizeof(proof);
        if (proof_bytes > 256000000 || journal.commits.size() >= 5000000)
            return make_error(ErrorCode::kValidation, "Conversion proofs exceed reader bound");
        journal.commits.push_back(std::move(proof));
    }
    if (query.lastError().isValid())
        return map_sql_error(query, "load_foreign_commit_proofs");
    return std::optional<ForeignConversionJournal>{std::move(journal)};
}

Result<void> SqliteCatalogRepository::begin_foreign_conversion(const std::string_view id,
                                                               const std::string_view source,
                                                               const std::int64_t expected_revision)
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (id.size() != 64 || source.size() != 64)
        return make_error(ErrorCode::kInvalidArgument, "Invalid foreign conversion identity");
    if (!impl_->database.transaction())
        return make_error(ErrorCode::kIo, "Cannot start conversion journal transaction");
    QSqlQuery revision(impl_->database);
    revision.prepare("UPDATE schema_info SET revision=revision+1 WHERE id=1 AND revision=?");
    revision.addBindValue(static_cast<qlonglong>(expected_revision));
    if (!revision.exec())
        return impl_->abort_transaction(map_sql_error(revision, "foreign_conversion_revision"));
    if (revision.numRowsAffected() != 1)
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision changed"));
    QSqlQuery insert(impl_->database);
    insert.prepare(
        "INSERT INTO foreign_conversion(id,source_sha256,catalog_revision) VALUES(?,?,?)");
    insert.addBindValue(qstring_from_utf8(id));
    insert.addBindValue(qstring_from_utf8(source));
    insert.addBindValue(static_cast<qlonglong>(expected_revision + 1));
    if (!insert.exec())
        return impl_->abort_transaction(map_sql_error(insert, "begin_foreign_conversion"));
    if (!impl_->database.commit())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Cannot commit conversion journal"));
    return {};
}

Result<void> SqliteCatalogRepository::save_foreign_conversion_checkpoint(
    const std::string_view id, const ForeignConversionCheckpoint &record,
    const std::int64_t expected_revision)
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (record.foreign_id.empty() || record.receipt_json.size() > 16000000)
        return make_error(ErrorCode::kInvalidArgument, "Invalid conversion checkpoint");
    if (!impl_->database.transaction())
        return make_error(ErrorCode::kIo, "Cannot start checkpoint transaction");
    QSqlQuery revision(impl_->database);
    revision.prepare("UPDATE schema_info SET revision=revision+1 WHERE id=1 AND revision=?");
    revision.addBindValue(static_cast<qlonglong>(expected_revision));
    if (!revision.exec())
        return impl_->abort_transaction(map_sql_error(revision, "foreign_checkpoint_revision"));
    if (revision.numRowsAffected() != 1)
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision changed"));
    QSqlQuery parent(impl_->database);
    parent.prepare("UPDATE foreign_conversion SET catalog_revision=? WHERE id=?");
    parent.addBindValue(static_cast<qlonglong>(expected_revision + 1));
    parent.addBindValue(qstring_from_utf8(id));
    if (!parent.exec())
        return impl_->abort_transaction(map_sql_error(parent, "foreign_checkpoint_parent"));
    if (parent.numRowsAffected() != 1)
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Conversion journal not found"));
    QSqlQuery insert(impl_->database);
    insert.prepare(
        "INSERT INTO foreign_conversion_record VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(conversion_id,foreign_id) DO UPDATE SET phase=excluded.phase, "
        "asset_id=excluded.asset_id,complete=excluded.complete,receipt_json=excluded.receipt_json");
    insert.addBindValue(qstring_from_utf8(id));
    insert.addBindValue(qstring_from_utf8(record.foreign_id));
    insert.addBindValue(qstring_from_utf8(record.phase));
    insert.addBindValue(record.asset_id ? QVariant(qstring_from_utf8(*record.asset_id)) :
                                          QVariant{});
    insert.addBindValue(record.complete);
    insert.addBindValue(qstring_from_utf8(record.receipt_json));
    if (!insert.exec())
        return impl_->abort_transaction(map_sql_error(insert, "save_foreign_checkpoint"));
    if (!impl_->database.commit())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Cannot commit conversion checkpoint"));
    return {};
}

Result<ForeignConversionJournal::Commit> SqliteCatalogRepository::run_foreign_conversion_stage(
    const std::string_view id, const ForeignConversionCheckpoint &record,
    const std::int64_t expected_revision, const std::function<void()> &action)
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    // TEMP objects belong to this connection only. Other clients cannot borrow
    // the provenance binding. The guard and proof run within each existing
    // business transaction, including rollback; no file I/O holds a transaction.
    if (!action)
        return make_error(ErrorCode::kInvalidArgument, "Foreign conversion action is empty");
    QSqlQuery head(impl_->database);
    head.prepare(
        "SELECT f.catalog_revision,s.revision FROM foreign_conversion f CROSS JOIN schema_info s WHERE f.id=? AND s.id=1");
    head.addBindValue(qstring_from_utf8(id));
    if (!head.exec())
        return map_sql_error(head, "foreign_stage_expected_revision");
    if (!head.next())
        return make_error(ErrorCode::kNotFound, "Conversion journal not found");
    if (head.value(0).toLongLong() != expected_revision ||
        head.value(1).toLongLong() != expected_revision)
        return make_error(ErrorCode::kConflict, "Destination changed outside this conversion",
                          {{"reason", "foreign_conversion_revision_conflict"}});
    head.finish();
    for (
        const char *statement :
        {"CREATE TEMP TABLE IF NOT EXISTS ravo_foreign_stage_context(id INTEGER PRIMARY KEY CHECK(id=1),conversion_id TEXT,foreign_id TEXT,phase TEXT,target_id TEXT,expected_revision INTEGER NOT NULL)",
         "CREATE TEMP TRIGGER IF NOT EXISTS ravo_foreign_revision_guard BEFORE UPDATE OF revision ON main.schema_info "
         "WHEN EXISTS(SELECT 1 FROM ravo_foreign_stage_context) BEGIN "
         "SELECT CASE WHEN OLD.revision IS NOT (SELECT expected_revision FROM ravo_foreign_stage_context) OR "
         "OLD.revision IS NOT (SELECT f.catalog_revision FROM foreign_conversion f JOIN ravo_foreign_stage_context c ON f.id=c.conversion_id) "
         "THEN RAISE(ABORT,'foreign_conversion_revision_conflict') END; END",
         "CREATE TEMP TRIGGER IF NOT EXISTS ravo_foreign_revision_proof AFTER UPDATE OF revision ON main.schema_info "
         "WHEN EXISTS(SELECT 1 FROM ravo_foreign_stage_context) BEGIN "
         "INSERT INTO foreign_conversion_commit SELECT conversion_id,NEW.revision,foreign_id,phase,target_id FROM ravo_foreign_stage_context; "
         "UPDATE foreign_conversion SET catalog_revision=NEW.revision WHERE id=(SELECT conversion_id FROM ravo_foreign_stage_context); "
         "UPDATE ravo_foreign_stage_context SET expected_revision=NEW.revision WHERE id=1; END",
         "CREATE TEMP TRIGGER IF NOT EXISTS ravo_foreign_asset_target AFTER INSERT ON main.asset "
         "WHEN EXISTS(SELECT 1 FROM ravo_foreign_stage_context) BEGIN UPDATE ravo_foreign_stage_context SET target_id=NEW.id WHERE id=1; END",
         "CREATE TEMP TRIGGER IF NOT EXISTS ravo_foreign_set_target AFTER INSERT ON main.library_set "
         "WHEN EXISTS(SELECT 1 FROM ravo_foreign_stage_context) BEGIN UPDATE ravo_foreign_stage_context SET target_id=NEW.id WHERE id=1; END"})
    {
        auto installed =
            impl_->exec(QString::fromUtf8(statement), "bind_foreign_commit_provenance");
        if (!installed)
            return installed.error();
    }
    QSqlQuery context(impl_->database);
    context.prepare("INSERT INTO ravo_foreign_stage_context VALUES(1,?,?,?,?,?)");
    context.addBindValue(qstring_from_utf8(id));
    context.addBindValue(qstring_from_utf8(record.foreign_id));
    context.addBindValue(qstring_from_utf8(record.phase));
    context.addBindValue(record.asset_id ? QVariant(qstring_from_utf8(*record.asset_id)) :
                                           QVariant{});
    context.addBindValue(static_cast<qlonglong>(expected_revision));
    if (!context.exec())
        return map_sql_error(context, "begin_foreign_commit_provenance");
    struct Binding
    {
        QSqlDatabase &database;
        bool active = true;
        ~Binding()
        {
            if (active)
            {
                QSqlQuery cleanup(database);
                if (!cleanup.exec("DELETE FROM ravo_foreign_stage_context"))
                    database.close(); // Fail closed: never tag an unrelated later write.
            }
        }
    } binding{impl_->database};
    try
    {
        action();
    }
    catch (const std::exception &error)
    {
        binding.active = false;
        impl_->database.close(); // Roll back any unowned unfinished transaction.
        return make_error(ErrorCode::kIo, "Foreign conversion action threw",
                          {{"detail", error.what()}});
    }
    catch (...)
    {
        binding.active = false;
        impl_->database.close();
        return make_error(ErrorCode::kIo, "Foreign conversion action threw an unknown exception");
    }
    ForeignConversionJournal::Commit confirmed{record.foreign_id, record.phase, record.asset_id, 0};
    if (!context.exec(
            "SELECT target_id,expected_revision FROM ravo_foreign_stage_context WHERE id=1") ||
        !context.next())
        return map_sql_error(context, "foreign_commit_target");
    if (!context.value(0).isNull())
        confirmed.target_id = context.value(0).toString().toStdString();
    confirmed.revision = context.value(1).toLongLong();
    if (!context.exec("DELETE FROM ravo_foreign_stage_context"))
    {
        auto error = map_sql_error(context, "end_foreign_commit_provenance");
        binding.active = false;
        impl_->database.close();
        return error;
    }
    binding.active = false;
    context.prepare(
        "SELECT f.catalog_revision,s.revision FROM foreign_conversion f CROSS JOIN schema_info s WHERE f.id=? AND s.id=1");
    context.addBindValue(qstring_from_utf8(id));
    if (!context.exec() || !context.next())
        return map_sql_error(context, "confirmed_foreign_revision");
    if (confirmed.revision != context.value(0).toLongLong() ||
        confirmed.revision != context.value(1).toLongLong())
        return make_error(ErrorCode::kConflict, "Destination changed outside this conversion",
                          {{"reason", "foreign_conversion_revision_conflict"}});
    return confirmed;
}
} // namespace ravo
