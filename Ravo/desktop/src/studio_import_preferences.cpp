#include "ravo/desktop/studio_import_preferences.h"

#include <QDir>
#include <QMetaType>
#include <QSettings>
#include "studio_qt.h"

namespace ravo
{
namespace
{
constexpr auto kDestination = "desktop/import/lastDestination";
TaskError settings_error()
{
    return make_error(ErrorCode::kIo, "Unable to save the last import destination",
                      {{"reason", "import_preferences_io_failed"}});
}
} // namespace

Result<QString> StudioImportPreferences::loadLastDestination() const
{
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return settings_error();
    if (!settings.contains(QLatin1String(kDestination)))
        return QString{};
    const auto stored = settings.value(QLatin1String(kDestination));
    const auto path = stored.toString();
    if (stored.metaType().id() != QMetaType::QString || path.isEmpty() ||
        path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
    {
        settings.remove(QLatin1String(kDestination));
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return settings_error();
        return make_error(ErrorCode::kValidation, "Invalid saved import destination was removed",
                          {{"reason", "invalid_import_destination_preference"}});
    }
    return QDir::cleanPath(path);
}

Result<void> StudioImportPreferences::rememberDestination(const QString &path) const
{
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
        return make_error(ErrorCode::kValidation, "Import destination must be an absolute path",
                          {{"reason", "invalid_import_destination_preference"}});
    QSettings settings;
    const auto previous = settings.value(QLatin1String(kDestination));
    settings.setValue(QLatin1String(kDestination), QDir::cleanPath(path));
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // Also restore Qt's process-local settings cache after a failed atomic write.
        if (previous.isValid())
            settings.setValue(QLatin1String(kDestination), previous);
        else
            settings.remove(QLatin1String(kDestination));
        return settings_error();
    }
    return {};
}
} // namespace ravo
