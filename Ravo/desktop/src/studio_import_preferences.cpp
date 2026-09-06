#include "ravo/desktop/studio_import_preferences.h"

#include <string>
#include <QDir>
#include <QMetaType>
#include <QSettings>
#include "studio_qt.h"

namespace ravo
{
namespace
{
constexpr auto kDestination = "desktop/import/lastDestination";
constexpr auto kSource = "desktop/import/lastSource";
TaskError settings_error()
{
    return make_error(ErrorCode::kIo, "Unable to access import folder preferences",
                      {{"reason", "import_preferences_io_failed"}});
}
Result<QString> load_path(const char *key, const std::string &kind)
{
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return settings_error();
    if (!settings.contains(QLatin1String(key)))
        return QString{};
    const auto stored = settings.value(QLatin1String(key));
    const auto path = stored.toString();
    if (stored.metaType().id() != QMetaType::QString || path.isEmpty() ||
        path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
    {
        settings.remove(QLatin1String(key));
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return settings_error();
        return make_error(ErrorCode::kValidation, "Invalid saved import " + kind + " was removed",
                          {{"reason", "invalid_import_" + kind + "_preference"}});
    }
    return QDir::cleanPath(path);
}

Result<void> remember_path(const char *key, const std::string &kind, const QString &path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
        return make_error(ErrorCode::kValidation, "Import " + kind + " must be an absolute path",
                          {{"reason", "invalid_import_" + kind + "_preference"}});
    QSettings settings;
    const auto previous = settings.value(QLatin1String(key));
    settings.setValue(QLatin1String(key), QDir::cleanPath(path));
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // Also restore Qt's process-local settings cache after a failed atomic write.
        if (previous.isValid())
            settings.setValue(QLatin1String(key), previous);
        else
            settings.remove(QLatin1String(key));
        return settings_error();
    }
    return {};
}
} // namespace

Result<QString> StudioImportPreferences::loadLastSource() const
{
    return load_path(kSource, "source");
}

Result<void> StudioImportPreferences::rememberSource(const QString &path) const
{
    return remember_path(kSource, "source", path);
}

Result<QString> StudioImportPreferences::loadLastDestination() const
{
    return load_path(kDestination, "destination");
}

Result<void> StudioImportPreferences::rememberDestination(const QString &path) const
{
    return remember_path(kDestination, "destination", path);
}
} // namespace ravo
