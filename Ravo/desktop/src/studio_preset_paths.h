#pragma once
#include <QFileInfo>
#include <QString>
namespace ravo
{
QString local_file_path(QString path);
QString preset_name_from_filename(const QFileInfo &info);
QString canonical_or_absolute(const QFileInfo &info);
} // namespace ravo
