#include "ravo/adapters/sqlite_catalog.h"
#include "catalog_sql_internal.h"

#include <QSqlError>

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
    ForeignConversionJournal journal{
        std::string(id), query.value(0).toString().toStdString(), query.value(1).toLongLong(), {}};
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
    revision.addBindValue(expected_revision);
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
    insert.addBindValue(expected_revision + 1);
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
    revision.addBindValue(expected_revision);
    if (!revision.exec())
        return impl_->abort_transaction(map_sql_error(revision, "foreign_checkpoint_revision"));
    if (revision.numRowsAffected() != 1)
        return impl_->abort_transaction(
            make_error(ErrorCode::kConflict, "Catalog revision changed"));
    QSqlQuery parent(impl_->database);
    parent.prepare("UPDATE foreign_conversion SET catalog_revision=? WHERE id=?");
    parent.addBindValue(expected_revision + 1);
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
} // namespace ravo
