#include "ravo/adapters/lightroom_catalog.h"
#include "ravo/adapters/text_file.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>

#include <map>
#include <set>

namespace ravo
{
namespace
{
TaskError source_error(const std::string &detail)
{
    return make_error(ErrorCode::kUnsupported, "Unsupported or corrupt Lightroom catalog",
                      {{"reason", "unsupported_source_schema"}, {"detail", detail}});
}

struct Connection
{
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    ~Connection()
    {
        db.close();
        db = QSqlDatabase{};
        QSqlDatabase::removeDatabase(name);
    }
};

Result<void> check_closed(const QString &path)
{
    for (const auto *suffix : {"-wal", "-journal", ".lock"})
    {
        const QFileInfo journal(path + QString::fromLatin1(suffix));
        if (journal.exists() && (journal.size() != 0 || QString::fromLatin1(suffix) == ".lock"))
            return make_error(ErrorCode::kConflict, "Close Lightroom before importing its catalog",
                              {{"reason", "lightroom_catalog_active"}});
    }
    return {};
}

Result<std::vector<ForeignCatalogPhoto>> read_snapshot(const QString &path,
                                                       const CancellationToken &cancellation)
{
    Connection connection;
    connection.db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0"));
    connection.db.setDatabaseName(path);
    if (!connection.db.open())
        return source_error(connection.db.lastError().text().toStdString());
    QSqlQuery query(connection.db);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next() ||
        query.value(0).toString() != QStringLiteral("ok"))
        return source_error("SQLite integrity check failed");
    const auto tables = connection.db.tables();
    // LEFT JOIN keeps broken references visible: never silently lose a photo.
    if (!query.exec(QStringLiteral(
            "SELECT i.id_local,r.absolutePath,d.pathFromRoot,f.idx_filename,f.baseName,"
            "f.extension,i.rating,i.pick,i.colorLabels,i.masterImage "
            "FROM Adobe_images i LEFT JOIN AgLibraryFile f ON f.id_local=i.rootFile "
            "LEFT JOIN AgLibraryFolder d ON d.id_local=f.folder "
            "LEFT JOIN AgLibraryRootFolder r ON r.id_local=d.rootFolder ORDER BY i.id_local")))
        return source_error(query.lastError().text().toStdString());
    std::vector<ForeignCatalogPhoto> photos;
    std::map<std::string, std::size_t> indices;
    while (query.next())
    {
        auto checked = cancellation.check();
        if (!checked)
            return checked.error();
        if (photos.size() >= 1000000)
            return source_error("Catalog exceeds the one-million-photo reader limit");
        ForeignCatalogPhoto photo;
        photo.foreign_id = query.value(0).toString().toStdString();
        if (photo.foreign_id.empty() || indices.contains(photo.foreign_id))
            return source_error("Invalid or duplicate photo identity");
        if (query.value(1).isNull() || query.value(2).isNull())
            return source_error("Photo has a broken folder reference");
        QString filename = query.value(3).toString();
        if (filename.isEmpty())
            filename = query.value(4).toString() + '.' + query.value(5).toString();
        if (filename.isEmpty() || filename.contains('/') || filename.contains('\\') ||
            filename == "." || filename == "..")
            return source_error("Invalid photo filename");
        QString root = query.value(1).toString();
        QString relative = query.value(2).toString();
        root.replace('\\', '/');
        relative.replace('\\', '/');
        if (root.isEmpty() || QDir::isAbsolutePath(relative))
            return source_error("Invalid photo folder path");
        photo.original_path = QDir::cleanPath(root + '/' + relative + '/' + filename).toStdString();
        if (!QDir::isAbsolutePath(root))
            photo.skip_reason = "foreign_volume_unavailable";
        bool valid_rating = false;
        const int rating = query.value(6).isNull() ? 0 : query.value(6).toInt(&valid_rating);
        if ((!query.value(6).isNull() && !valid_rating) || rating < 0 || rating > 5)
            return source_error("Invalid photo rating");
        photo.rating = rating;
        const int pick = query.value(7).toInt();
        photo.rejected = pick < 0;
        if (pick > 0)
            photo.unsupported_adjusts.emplace_back("lightroom.pick_flag");
        const auto label = query.value(8).toString();
        const std::map<QString, ColorLabel> labels{{"red", ColorLabel::kRed},
                                                   {"yellow", ColorLabel::kYellow},
                                                   {"green", ColorLabel::kGreen},
                                                   {"blue", ColorLabel::kBlue},
                                                   {"purple", ColorLabel::kPurple}};
        if (auto found = labels.find(label.toLower()); found != labels.end())
            photo.color_label = found->second;
        else if (!label.isEmpty())
            photo.unsupported_adjusts.emplace_back("lightroom.custom_color_label:" +
                                                   label.toStdString());
        if (!query.value(9).isNull())
            photo.skip_reason = "lightroom_virtual_copy_unsupported";
        for (const auto &[table, field] : std::map<QString, std::string>{
                 {"Adobe_imageDevelopSettings", "lightroom.develop"},
                 {"Adobe_libraryImageDevelopHistoryStep", "lightroom.history"},
                 {"Adobe_libraryImageDevelopSnapshot", "lightroom.snapshots"},
                 {"AgLibraryCollection", "lightroom.collections"},
                 {"AgLibraryIPTC", "lightroom.iptc"}})
        {
            if (tables.contains(table))
                photo.unsupported_adjusts.push_back(field);
        }
        indices.emplace(photo.foreign_id, photos.size());
        photos.push_back(std::move(photo));
    }
    if (query.lastError().isValid())
        return source_error(query.lastError().text().toStdString());

    if (tables.contains("AgLibraryKeywordImage"))
    {
        struct Keyword
        {
            std::string name;
            std::string parent;
        };
        std::map<std::string, Keyword> keywords;
        if (!query.exec(QStringLiteral("SELECT id_local,name,parent FROM AgLibraryKeyword")))
            return source_error(query.lastError().text().toStdString());
        while (query.next())
        {
            auto checked = cancellation.check();
            if (!checked)
                return checked.error();
            if (keywords.size() >= 1000000)
                return source_error("Keyword limit exceeded");
            keywords.emplace(query.value(0).toString().toStdString(),
                             Keyword{query.value(1).toString().toStdString(),
                                     query.value(2).toString().toStdString()});
        }
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
        if (!query.exec(
                QStringLiteral("SELECT image,tag FROM AgLibraryKeywordImage ORDER BY image,tag")))
            return source_error(query.lastError().text().toStdString());
        std::size_t count = 0;
        while (query.next())
        {
            auto checked = cancellation.check();
            if (!checked)
                return checked.error();
            if (++count > 5000000)
                return source_error("Keyword assignment limit exceeded");
            const auto image = indices.find(query.value(0).toString().toStdString());
            if (image == indices.end())
                return source_error("Keyword references an unknown photo");
            auto id = query.value(1).toString().toStdString();
            std::set<std::string> visited;
            std::string tag;
            while (!id.empty() && id != "0")
            {
                if (!visited.insert(id).second || visited.size() > 128)
                    return source_error("Keyword hierarchy cycle or depth limit");
                const auto keyword = keywords.find(id);
                if (keyword == keywords.end())
                    return source_error("Broken keyword reference");
                if (keyword->second.name.find('|') != std::string::npos)
                    return source_error("Keyword contains the destination hierarchy separator");
                if (!keyword->second.name.empty())
                    tag = keyword->second.name + (tag.empty() ? "" : "|" + tag);
                id = keyword->second.parent;
            }
            if (!tag.empty())
                photos[image->second].keywords.push_back(std::move(tag));
        }
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
    }
    return photos;
}
} // namespace

Result<std::vector<ForeignCatalogPhoto>>
read_lightroom_catalog(const std::string_view path, const CancellationToken &cancellation)
{
    const QString source = QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
    auto checked = cancellation.check();
    if (!checked)
        return checked.error();
    auto closed = check_closed(source);
    if (!closed)
        return closed.error();
    const QFileInfo before(source);
    if (!before.isFile() || before.size() > 2000000000)
        return source_error("Missing catalog or catalog exceeds the 2 GB reader limit");
    auto hash = sha256_file_hex(path, cancellation);
    if (!hash)
        return hash.error();
    QTemporaryDir directory;
    if (!directory.isValid())
        return make_error(ErrorCode::kIo, "Cannot create Lightroom snapshot directory");
    const auto snapshot = directory.filePath(QStringLiteral("source.lrcat"));
    QFile input(source);
    QFile output(snapshot);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return make_error(ErrorCode::kIo, "Cannot open Lightroom snapshot files");
    while (!input.atEnd())
    {
        checked = cancellation.check();
        if (!checked)
            return checked.error();
        const auto bytes = input.read(1024 * 1024);
        if (output.pos() + bytes.size() > 2000000000)
            return source_error("Catalog grew beyond the 2 GB reader limit");
        if (input.error() != QFileDevice::NoError || output.write(bytes) != bytes.size())
            return make_error(ErrorCode::kIo, "Cannot copy Lightroom snapshot");
    }
    if (!output.flush())
        return make_error(ErrorCode::kIo, "Cannot flush Lightroom snapshot");
    output.close();
    input.close();
    auto copied = sha256_file_hex(snapshot.toStdString(), cancellation);
    if (!copied)
        return copied.error();
    auto after = sha256_file_hex(path, cancellation);
    if (!after)
        return after.error();
    closed = check_closed(source);
    if (!closed)
        return closed.error();
    if (hash.value() != copied.value() || hash.value() != after.value())
        return make_error(ErrorCode::kConflict, "Lightroom catalog changed during snapshot",
                          {{"reason", "lightroom_source_changed"}});
    return read_snapshot(snapshot, cancellation);
}
} // namespace ravo
