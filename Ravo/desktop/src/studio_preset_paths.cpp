#include "studio_preset_paths.h"
#include <QUrl>
namespace ravo
{
QString local_file_path(QString path)
{
    path = path.trimmed();
    if (path.startsWith(QStringLiteral("file:")))
        path = QUrl(path).toLocalFile();
    return path;
}

QString preset_name_from_filename(const QFileInfo &info)
{
    QString name = info.fileName();
    const QString style_suffix = QStringLiteral(".rstyle.json");
    const QString xmp_suffix = QStringLiteral(".xmp");
    if (name.endsWith(style_suffix, Qt::CaseInsensitive))
        name.chop(style_suffix.size());
    else if (name.endsWith(xmp_suffix, Qt::CaseInsensitive))
        name.chop(xmp_suffix.size());
    return name;
}

QString canonical_or_absolute(const QFileInfo &info)
{
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
} // namespace ravo
