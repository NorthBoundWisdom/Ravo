#include "ravo/desktop/studio_import_preferences.h"

#include <string>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>
#include "studio_qt.h"

namespace ravo
{
namespace
{
constexpr auto kDestination = "desktop/import/lastDestination";
constexpr auto kSource = "desktop/import/lastSource";
constexpr auto kOrganization = "desktop/import/lastOrganization";
bool valid_organization(const QString &value)
{
    return value == QLatin1String("single") || value == QLatin1String("hierarchy") ||
           value == QLatin1String("date") || value == QLatin1String("month");
}
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
    if (kind == "source" && (!QFileInfo(path).isDir() || !QFileInfo(path).isReadable()))
    {
        settings.remove(QLatin1String(key));
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return settings_error();
        return make_error(ErrorCode::kValidation,
                          "Saved import source is unavailable; choose a source folder.",
                          {{"reason", "unavailable_import_source_preference"}});
    }
    return QDir::cleanPath(path);
}

Result<void> remember_value(const char *key, const QString &value)
{
    QSettings settings;
    const auto previous = settings.value(QLatin1String(key));
    settings.setValue(QLatin1String(key), value);
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
Result<void> remember_path(const char *key, const std::string &kind, const QString &path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path))
        return make_error(ErrorCode::kValidation, "Import " + kind + " must be an absolute path",
                          {{"reason", "invalid_import_" + kind + "_preference"}});
    return remember_value(key, QDir::cleanPath(path));
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

Result<QString> StudioImportPreferences::loadLastOrganization() const
{
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return settings_error();
    if (!settings.contains(QLatin1String(kOrganization)))
        return QStringLiteral("single");
    const auto stored = settings.value(QLatin1String(kOrganization));
    if (stored.metaType().id() == QMetaType::QString && valid_organization(stored.toString()))
        return stored.toString();
    settings.remove(QLatin1String(kOrganization));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return settings_error();
    return make_error(ErrorCode::kValidation, "Invalid saved import organization was removed",
                      {{"reason", "invalid_import_organization_preference"}});
}

Result<void> StudioImportPreferences::rememberOrganization(const QString &organization) const
{
    if (!valid_organization(organization))
        return make_error(ErrorCode::kValidation, "Invalid import organization",
                          {{"reason", "invalid_import_organization_preference"}});
    return remember_value(kOrganization, organization);
}
} // namespace ravo
