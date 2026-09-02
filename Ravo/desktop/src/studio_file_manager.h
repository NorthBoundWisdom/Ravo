#pragma once

#include <QString>
#include <QStringList>

#include "ravo/foundation/error.h"

namespace ravo
{

struct FileManagerRevealLaunch
{
    QString program;
    QStringList arguments;
    QString local_path;
};

[[nodiscard]] Result<QString> local_file_path_from_asset_uri(const QString &uri);
[[nodiscard]] Result<FileManagerRevealLaunch> file_manager_reveal_launch(const QString &local_path);
[[nodiscard]] Result<FileManagerRevealLaunch>
file_manager_open_directory_launch(const QString &local_path);
[[nodiscard]] bool start_file_manager_reveal(const FileManagerRevealLaunch &launch);

} // namespace ravo
