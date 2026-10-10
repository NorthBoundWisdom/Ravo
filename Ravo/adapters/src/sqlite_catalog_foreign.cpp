#include "ravo/adapters/sqlite_catalog.h"
#include "catalog_sql_internal.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QSqlError>
#include <filesystem>

namespace ravo
{
using namespace sqlite_internal;

Result<ForeignCatalogArchive>
SqliteCatalogRepository::archive_foreign_catalog(const std::string_view source_path,
                                                 const std::string_view expected_sha256,
                                                 const CancellationToken &cancellation)
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto checked = cancellation.check();
    if (!checked)
        return checked.error();
    if (expected_sha256.size() != 64)
        return make_error(ErrorCode::kInvalidArgument, "Invalid source hash");
    QFile source(qstring_from_utf8(source_path));
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > 2000000000)
        return make_error(ErrorCode::kIo, "Cannot open bounded foreign catalog source");
    ForeignCatalogArchive archive{std::string(expected_sha256), std::string(source_path),
                                  std::string(expected_sha256),
                                  static_cast<std::uint64_t>(source.size())};
    if (!impl_->database.transaction())
        return make_error(ErrorCode::kIo, "Cannot start foreign archive transaction");
    QSqlQuery insert(impl_->database);
    insert.prepare(
        "INSERT INTO foreign_catalog_source(id,source_path,sha256,size_bytes) VALUES(?,?,?,?)");
    insert.addBindValue(qstring_from_utf8(archive.source_id));
    insert.addBindValue(qstring_from_utf8(source_path));
    insert.addBindValue(qstring_from_utf8(expected_sha256));
    insert.addBindValue(static_cast<qlonglong>(archive.size_bytes));
    if (!insert.exec())
        return impl_->abort_transaction(map_sql_error(insert, "foreign_archive_source"));
    QCryptographicHash digest(QCryptographicHash::Sha256);
    QSqlQuery chunk(impl_->database);
    chunk.prepare("INSERT INTO foreign_catalog_chunk(source_id,ordinal,bytes) VALUES(?,?,?)");
    std::uint64_t total = 0;
    int ordinal = 0;
    while (!source.atEnd())
    {
        checked = cancellation.check();
        if (!checked)
            return impl_->abort_transaction(checked.error());
        const auto bytes = source.read(1048576);
        total += static_cast<std::uint64_t>(bytes.size());
        if (source.error() != QFileDevice::NoError || total > archive.size_bytes || bytes.isEmpty())
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Cannot read stable foreign catalog"));
        digest.addData(bytes);
        chunk.bindValue(0, qstring_from_utf8(archive.source_id));
        chunk.bindValue(1, ordinal++);
        chunk.bindValue(2, bytes);
        if (!chunk.exec())
            return impl_->abort_transaction(map_sql_error(chunk, "foreign_archive_chunk"));
    }
    if (total != archive.size_bytes || digest.result().toHex().toStdString() != expected_sha256)
        return impl_->abort_transaction(make_error(ErrorCode::kConflict,
                                                   "Foreign source changed before preservation",
                                                   {{"reason", "lightroom_source_changed"}}));
    checked = cancellation.check();
    if (!checked)
        return impl_->abort_transaction(checked.error());
    QSqlQuery revision(impl_->database);
    if (!revision.exec("UPDATE schema_info SET revision=revision+1 WHERE id=1"))
        return impl_->abort_transaction(map_sql_error(revision, "foreign_archive_revision"));
    if (!impl_->database.commit())
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Cannot commit foreign archive"));
    return archive;
}

Result<std::vector<ForeignCatalogArchive>>
SqliteCatalogRepository::list_foreign_catalog_archives() const
{
    if (!impl_ || !impl_->database.isOpen())
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    QSqlQuery query(impl_->database);
    if (!query.exec(
            "SELECT id,source_path,sha256,size_bytes FROM foreign_catalog_source ORDER BY id"))
        return map_sql_error(query, "foreign_archive_list");
    std::vector<ForeignCatalogArchive> result;
    while (query.next())
        result.push_back({query.value(0).toString().toStdString(),
                          query.value(1).toString().toStdString(),
                          query.value(2).toString().toStdString(), query.value(3).toULongLong()});
    if (query.lastError().isValid())
        return map_sql_error(query, "foreign_archive_list");
    return result;
}

Result<void>
SqliteCatalogRepository::export_foreign_catalog_archive(const std::string_view source_id,
                                                        const std::string_view output_path,
                                                        const CancellationToken &cancellation) const
{
    auto archives = list_foreign_catalog_archives();
    if (!archives)
        return archives.error();
    const ForeignCatalogArchive *archive = nullptr;
    for (const auto &candidate : archives.value())
        if (candidate.source_id == source_id)
            archive = &candidate;
    if (!archive)
        return make_error(ErrorCode::kNotFound, "Foreign source does not exist");
    auto checked = cancellation.check();
    if (!checked)
        return checked.error();
    const QString output = qstring_from_utf8(output_path);
    if (QFileInfo::exists(output))
        return make_error(ErrorCode::kConflict, "Output already exists");
    QTemporaryFile temporary(QFileInfo(output).absolutePath() + "/.ravo-foreign-XXXXXX");
    if (!temporary.open())
        return make_error(ErrorCode::kIo, "Cannot create foreign source output");
    QSqlQuery query(impl_->database);
    query.setForwardOnly(true);
    query.prepare(
        "SELECT ordinal,bytes FROM foreign_catalog_chunk WHERE source_id=? ORDER BY ordinal");
    query.addBindValue(qstring_from_utf8(source_id));
    if (!query.exec())
        return map_sql_error(query, "foreign_archive_export");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    int ordinal = 0;
    std::uint64_t total = 0;
    while (query.next())
    {
        checked = cancellation.check();
        if (!checked)
            return checked.error();
        const auto bytes = query.value(1).toByteArray();
        total += static_cast<std::uint64_t>(bytes.size());
        if (query.value(0).toInt() != ordinal++ || bytes.isEmpty() || bytes.size() > 1048576 ||
            total > archive->size_bytes)
            return make_error(ErrorCode::kValidation, "Corrupt foreign source archive");
        digest.addData(bytes);
        if (temporary.write(bytes) != bytes.size())
            return make_error(ErrorCode::kIo, "Cannot write foreign source output");
    }
    if (query.lastError().isValid())
        return map_sql_error(query, "foreign_archive_export");
    if (total != archive->size_bytes || digest.result().toHex().toStdString() != archive->sha256)
        return make_error(ErrorCode::kValidation, "Foreign source archive hash mismatch");
    if (!temporary.flush())
        return make_error(ErrorCode::kIo, "Cannot flush foreign source output");
    checked = cancellation.check();
    if (!checked)
        return checked.error();
    temporary.close();
    // A hard-link create is a single no-replace publication. The temporary owner
    // removes only its staging name; unsupported filesystems fail explicitly.
    std::error_code error;
    std::filesystem::create_hard_link(std::filesystem::path(temporary.fileName().toStdU16String()),
                                      std::filesystem::path(output.toStdU16String()), error);
    if (error)
        return make_error(error == std::errc::file_exists ?
                              ErrorCode::kConflict :
                              (error == std::errc::operation_not_supported ||
                                       error == std::errc::function_not_supported ?
                                   ErrorCode::kUnsupported :
                                   ErrorCode::kIo),
                          "Cannot publish foreign source output", {{"detail", error.message()}});
    return {};
}
} // namespace ravo
