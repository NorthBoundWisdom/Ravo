#include "ravo/adapters/lightroom_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/adapters/xmp_adjacent_metadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>
#include <zlib.h>

#include <map>
#include <set>
#include <tuple>
#include <algorithm>

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

Result<std::string> settings_text(const QVariant &value,
                                  const std::uint32_t expanded_limit = 1048576)
{
    QByteArray bytes = value.toByteArray();
    constexpr qsizetype limit = 16000000;
    if (bytes.size() > limit)
        return source_error("Catalog text exceeds 16 MB");
    if (bytes.size() > 6 && bytes[0] == '\0' && static_cast<unsigned char>(bytes[4]) == 0x78)
    {
        const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData());
        const std::uint32_t expected = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                                       (std::uint32_t(p[2]) << 8) | p[3];
        if (expected > expanded_limit)
            return source_error("Compressed catalog text exceeds its expanded bound");
        QByteArray expanded(static_cast<qsizetype>(expected), '\0');
        uLongf size = expected;
        uLong compressed_size = static_cast<uLong>(bytes.size() - 4);
        if (uncompress2(reinterpret_cast<Bytef *>(expanded.data()), &size,
                        reinterpret_cast<const Bytef *>(bytes.constData() + 4),
                        &compressed_size) != Z_OK ||
            size != expected || compressed_size != static_cast<uLong>(bytes.size() - 4))
            return source_error("Invalid compressed catalog text");
        bytes = std::move(expanded);
    }
    if (bytes.startsWith("\xef\xbb\xbf"))
        bytes.remove(0, 3);
    const auto text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes)
        return source_error("Invalid UTF-8 in catalog text");
    return bytes.toStdString();
}

Result<ForeignCatalogSnapshot> read_snapshot(const QString &path,
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
    const auto image_columns = connection.db.record("Adobe_images");
    const auto copy_name_column = image_columns.contains("copyName") ? "i.copyName" : "''";
    if (!query.exec(
            QStringLiteral(
                "SELECT i.id_local,r.absolutePath,d.pathFromRoot,f.idx_filename,f.baseName,"
                "f.extension,i.rating,i.pick,i.colorLabels,i.masterImage,%1 "
                "FROM Adobe_images i LEFT JOIN AgLibraryFile f ON f.id_local=i.rootFile "
                "LEFT JOIN AgLibraryFolder d ON d.id_local=f.folder "
                "LEFT JOIN AgLibraryRootFolder r ON r.id_local=d.rootFolder ORDER BY i.id_local")
                .arg(copy_name_column)))
        return source_error(query.lastError().text().toStdString());
    ForeignCatalogSnapshot result;
    auto &photos = result.photos;
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
        if (root.isEmpty() || QDir::isAbsolutePath(relative) || relative.split('/').contains(".."))
            return source_error("Invalid photo folder path");
        photo.original_path = QDir::cleanPath(root + '/' + relative + '/' + filename).toStdString();
        if (!QDir::isAbsolutePath(root))
            photo.skip_reason = "foreign_volume_unavailable";
        bool valid_rating = false;
        const int rating = query.value(6).isNull() ? 0 : query.value(6).toInt(&valid_rating);
        if ((!query.value(6).isNull() && !valid_rating) || rating < 0 || rating > 5)
            return source_error("Invalid photo rating");
        photo.rating = rating;
        bool valid_pick = false;
        const int pick = query.value(7).isNull() ? 0 : query.value(7).toInt(&valid_pick);
        if ((!query.value(7).isNull() && !valid_pick) || pick < -1 || pick > 1)
            return source_error("Invalid photo flag");
        photo.rejected = pick < 0;
        photo.picked = pick > 0;
        const auto label = query.value(8).toString();
        photo.color_label = ColorLabel::kNone;
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
            photo.master_id = query.value(9).toString().toStdString();
        photo.copy_name = query.value(10).toString().toStdString();
        if (!photo.copy_name.empty())
            photo.unsupported_adjusts.push_back("lightroom.copy_name:" + photo.copy_name);
        indices.emplace(photo.foreign_id, photos.size());
        photos.push_back(std::move(photo));
    }
    if (query.lastError().isValid())
        return source_error(query.lastError().text().toStdString());

    for (const auto &photo : photos)
        if (photo.master_id &&
            (!indices.contains(*photo.master_id) || photos[indices.at(*photo.master_id)].master_id))
            return source_error("Virtual copy references an invalid master");

    std::size_t settings_bytes = 0;
    for (const auto &[table, snapshot, history] :
         {std::tuple{"Adobe_imageDevelopSettings", false, false},
          std::tuple{"Adobe_libraryImageDevelopHistoryStep", false, true},
          std::tuple{"Adobe_libraryImageDevelopSnapshot", true, false}})
    {
        if (!tables.contains(QString::fromLatin1(table)))
            continue;
        const QString sql =
            history ?
                "SELECT image,text,name FROM Adobe_libraryImageDevelopHistoryStep ORDER BY image,dateCreated,id_local" :
                (snapshot ?
                     "SELECT image,text,name FROM Adobe_libraryImageDevelopSnapshot ORDER BY image,id_local" :
                     "SELECT image,text,'' FROM Adobe_imageDevelopSettings ORDER BY image");
        if (!query.exec(sql))
            return source_error(query.lastError().text().toStdString());
        std::set<std::string> current_ids;
        while (query.next())
        {
            auto checked = cancellation.check();
            if (!checked)
                return checked.error();
            const auto found = indices.find(query.value(0).toString().toStdString());
            if (found == indices.end())
                return source_error("Develop record references an unknown photo");
            if (!history && !snapshot && !current_ids.insert(found->first).second)
                return source_error("Multiple current Develop records for one photo");
            if (query.value(1).isNull())
                continue;
            auto text = settings_text(query.value(1), history || snapshot ? 1048576 : 16000000);
            if (!text)
            {
                auto error = text.error();
                error.context.emplace("foreign_id", found->first);
                error.context.emplace("field", std::string(table) + ".text");
                return error;
            }
            settings_bytes += text.value().size();
            if (settings_bytes > 512000000)
                return source_error("Expanded Develop data exceeds 512 MB");
            auto &photo = photos[found->second];
            if (!history && !snapshot)
            {
                if (photo.develop_settings)
                    return source_error("Multiple current Develop records for one photo");
                if (text.value().empty())
                    continue;
                photo.develop_settings = std::move(text).value();
            }
            else
            {
                if (photo.develop_states.size() >= 100000)
                    return source_error("Too many Develop states for one photo");
                photo.develop_states.push_back(
                    {query.value(2).toString().toStdString(), std::move(text).value(), snapshot});
            }
        }
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
    }

    if (tables.contains("Adobe_AdditionalMetadata"))
    {
        if (!query.exec("SELECT image,xmp FROM Adobe_AdditionalMetadata ORDER BY image"))
            return source_error(query.lastError().text().toStdString());
        std::set<std::string> seen;
        while (query.next())
        {
            auto checked = cancellation.check();
            if (!checked)
                return checked.error();
            const auto id = query.value(0).toString().toStdString();
            const auto found = indices.find(id);
            if (found == indices.end() || !seen.insert(id).second)
                return source_error("Invalid metadata photo reference");
            auto decoded = settings_text(query.value(1), 16000000);
            if (!decoded)
            {
                auto error = decoded.error();
                error.context.emplace("foreign_id", id);
                error.context.emplace("field", "Adobe_AdditionalMetadata.xmp");
                return error;
            }
            const auto &text = decoded.value();
            const auto bytes = query.value(1).toByteArray();
            settings_bytes += text.size();
            if (settings_bytes > 512000000)
                return source_error("Metadata exceeds reader bounds");
            if (text.empty())
                continue;
            auto parsed = parse_xmp_adjacent_metadata(text);
            if (!parsed.parse_ok)
            {
                auto error = source_error("Invalid catalog XMP metadata: " +
                                          parsed.parse_reason.value_or("unknown"));
                error.context.emplace("foreign_id", id);
                error.context.emplace("packet_prefix_hex", bytes.left(16).toHex().toStdString());
                error.context.emplace("packet_size", std::to_string(bytes.size()));
                return error;
            }
            photos[found->second].metadata = std::move(parsed.metadata.writable);
        }
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
    }

    if (tables.contains("AgLibraryCollection"))
    {
        if (!query.exec(
                "SELECT id_local,name,parent,creationId FROM AgLibraryCollection ORDER BY id_local"))
            return source_error(query.lastError().text().toStdString());
        std::map<std::string, std::size_t> collections;
        while (query.next())
        {
            auto checked = cancellation.check();
            if (!checked)
                return checked.error();
            ForeignCatalogCollection item;
            item.foreign_id = query.value(0).toString().toStdString();
            item.name = query.value(1).toString().toStdString();
            if (!query.value(2).isNull())
                item.parent_id = query.value(2).toString().toStdString();
            item.creation_id = query.value(3).toString().toStdString();
            if (collections.size() >= 1000000 || item.foreign_id.empty() ||
                !collections.emplace(item.foreign_id, result.collections.size()).second)
                return source_error("Invalid collection identity or limit");
            result.collections.push_back(std::move(item));
        }
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
        if (tables.contains("AgLibraryCollectionImage"))
        {
            if (!query.exec(
                    "SELECT collection,image FROM AgLibraryCollectionImage ORDER BY collection,positionInCollection,image"))
                return source_error(query.lastError().text().toStdString());
            std::size_t count = 0;
            while (query.next())
            {
                auto checked = cancellation.check();
                if (!checked)
                    return checked.error();
                const auto collection = collections.find(query.value(0).toString().toStdString());
                const auto photo = query.value(1).toString().toStdString();
                if (++count > 5000000 || collection == collections.end() ||
                    !indices.contains(photo))
                    return source_error("Invalid collection membership or limit");
                result.collections[collection->second].photo_ids.push_back(photo);
            }
            if (query.lastError().isValid())
                return source_error(query.lastError().text().toStdString());
        }
        for (const auto &collection : result.collections)
        {
            std::set<std::string> seen;
            auto parent = collection.parent_id;
            while (parent && *parent != "0")
            {
                if (!seen.insert(*parent).second || seen.size() > 128 ||
                    !collections.contains(*parent))
                    return source_error("Invalid collection hierarchy");
                parent = result.collections[collections.at(*parent)].parent_id;
            }
        }
    }

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
            const auto id = query.value(0).toString().toStdString();
            if (id.empty() || !keywords
                                   .emplace(id, Keyword{query.value(1).toString().toStdString(),
                                                        query.value(2).toString().toStdString()})
                                   .second)
                return source_error("Invalid keyword identity");
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
    const std::set<QString> interpreted_tables{"Adobe_images",
                                               "AgLibraryFile",
                                               "AgLibraryFolder",
                                               "AgLibraryRootFolder",
                                               "AgLibraryKeyword",
                                               "AgLibraryKeywordImage",
                                               "Adobe_imageDevelopSettings",
                                               "Adobe_libraryImageDevelopHistoryStep",
                                               "Adobe_libraryImageDevelopSnapshot",
                                               "Adobe_AdditionalMetadata",
                                               "AgLibraryCollection",
                                               "AgLibraryCollectionImage"};
    for (const auto &table : tables)
    {
        if (interpreted_tables.contains(table) || table.startsWith("sqlite_"))
            continue;
        auto checked = cancellation.check();
        if (!checked)
            return checked.error();
        QString identifier = table;
        identifier.replace('"', "\"\"");
        if (!query.exec("SELECT 1 FROM \"" + identifier + "\" LIMIT 1"))
            return source_error(query.lastError().text().toStdString());
        if (query.next())
            result.archived_only_tables.push_back(table.toStdString());
        if (query.lastError().isValid())
            return source_error(query.lastError().text().toStdString());
    }
    std::sort(result.archived_only_tables.begin(), result.archived_only_tables.end());
    return result;
}
} // namespace

Result<ForeignCatalogSnapshot> read_lightroom_catalog(const std::string_view path,
                                                      const CancellationToken &cancellation)
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
    auto result = read_snapshot(snapshot, cancellation);
    if (result)
        result.value().source_sha256 = hash.value();
    return result;
}
} // namespace ravo
