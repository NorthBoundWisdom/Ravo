#include "studio_file_manager.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace ravo
{
namespace
{

[[nodiscard]] TaskError invalid_path_error(const QString &detail)
{
    return make_error(ErrorCode::kInvalidArgument, detail.toStdString());
}

} // namespace

Result<QString> local_file_path_from_asset_uri(const QString &uri)
{
    const QString trimmed = uri.trimmed();
    if (trimmed.isEmpty())
    {
        return invalid_path_error(QStringLiteral("The selected photo has no file URI."));
    }

    QString path;
    if (trimmed.startsWith(QStringLiteral("file:")))
    {
        path = QUrl(trimmed).toLocalFile();
    }
    else if (QFileInfo::exists(trimmed))
    {
        path = trimmed;
    }
    if (path.isEmpty())
    {
        return invalid_path_error(QStringLiteral("The selected photo has no local file path."));
    }
    return QFileInfo(path).absoluteFilePath();
}

Result<FileManagerRevealLaunch> file_manager_reveal_launch(const QString &local_path)
{
    const QFileInfo info(local_path);
    if (!info.exists() || !info.isFile())
    {
        return make_error(ErrorCode::kNotFound,
                          "The original file is missing and cannot be shown in the file manager.");
    }

    FileManagerRevealLaunch launch;
    launch.local_path = info.absoluteFilePath();
#if defined(Q_OS_MACOS)
    launch.program = QStringLiteral("open");
    launch.arguments = {QStringLiteral("-R"), launch.local_path};
#elif defined(Q_OS_WIN)
    launch.program = QStringLiteral("explorer");
    launch.arguments = {QStringLiteral("/select,") + QDir::toNativeSeparators(launch.local_path)};
#else
    launch.program = QStringLiteral("dbus-send");
    launch.arguments = {QStringLiteral("--session"),
                        QStringLiteral("--dest=org.freedesktop.FileManager1"),
                        QStringLiteral("--type=method_call"),
                        QStringLiteral("/org/freedesktop/FileManager1"),
                        QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                        QStringLiteral("array:string:") +
                            QUrl::fromLocalFile(launch.local_path).toString(),
                        QStringLiteral("string:")};
#endif
    return launch;
}

Result<FileManagerRevealLaunch> file_manager_open_directory_launch(const QString &local_path)
{
    const QFileInfo info(local_path);
    if (!info.exists() || !info.isDir())
    {
        return make_error(ErrorCode::kNotFound,
                          "The folder is missing and cannot be shown in the file manager.");
    }

    FileManagerRevealLaunch launch;
    launch.local_path = info.absoluteFilePath();
#if defined(Q_OS_MACOS)
    launch.program = QStringLiteral("open");
    launch.arguments = {launch.local_path};
#elif defined(Q_OS_WIN)
    launch.program = QStringLiteral("explorer");
    launch.arguments = {QDir::toNativeSeparators(launch.local_path)};
#else
    launch.program = QStringLiteral("xdg-open");
    launch.arguments = {launch.local_path};
#endif
    return launch;
}

bool start_file_manager_reveal(const FileManagerRevealLaunch &launch)
{
    if (launch.program.isEmpty())
    {
        return false;
    }
    if (QProcess::startDetached(launch.program, launch.arguments))
    {
        return true;
    }
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
    if (!launch.local_path.isEmpty())
    {
        return QProcess::startDetached(QStringLiteral("xdg-open"),
                                       {QFileInfo(launch.local_path).absolutePath()});
    }
#endif
    return false;
}

Result<FileManagerRevealLaunch> file_open_with_launch(const QString &local_path,
                                                      const QString &application_path)
{
    const QFileInfo info(local_path);
    if (!info.exists() || !info.isFile())
    {
        return make_error(ErrorCode::kNotFound,
                          "The working-copy file is missing and cannot be opened.");
    }

    FileManagerRevealLaunch launch;
    launch.local_path = info.absoluteFilePath();
    const QString app = application_path.trimmed();
#if defined(Q_OS_MACOS)
    launch.program = QStringLiteral("open");
    if (!app.isEmpty())
    {
        const QFileInfo app_info(app);
        if (app_info.isBundle() || app.endsWith(QStringLiteral(".app")))
            launch.arguments = {QStringLiteral("-a"), app, launch.local_path};
        else
            launch.arguments = {QStringLiteral("-a"), app, launch.local_path};
    }
    else
    {
        launch.arguments = {launch.local_path};
    }
#elif defined(Q_OS_WIN)
    if (!app.isEmpty())
    {
        launch.program = app;
        launch.arguments = {QDir::toNativeSeparators(launch.local_path)};
    }
    else
    {
        launch.program = QStringLiteral("cmd");
        launch.arguments = {QStringLiteral("/c"), QStringLiteral("start"), QStringLiteral(""),
                            QDir::toNativeSeparators(launch.local_path)};
    }
#else
    if (!app.isEmpty())
    {
        launch.program = app;
        launch.arguments = {launch.local_path};
    }
    else
    {
        launch.program = QStringLiteral("xdg-open");
        launch.arguments = {launch.local_path};
    }
#endif
    return launch;
}

bool start_file_open(const FileManagerRevealLaunch &launch)
{
    return start_file_manager_reveal(launch);
}

} // namespace ravo
