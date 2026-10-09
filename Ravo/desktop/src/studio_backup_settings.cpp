#include "ravo/desktop/studio_backup_settings.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "studio_command_ids.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{
QVariantMap fields(const QVariantMap &status)
{
    const CatalogBackupPolicy defaults;
    return {{"enabled", status.value("enabled", defaults.enabled)},
            {"directory", status.value("destination", QString{})},
            {"intervalMinutes",
             status.value("intervalMinutes", static_cast<int>(defaults.interval_minutes)).toInt()},
            {"retentionCount", status.value("retentionCount", defaults.retention_count).toInt()}};
}
} // namespace
StudioBackupSettings::StudioBackupSettings(StudioPresenter &presenter,
                                           StudioCommandController &commands, QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , commands_(commands)
{
    connect(&presenter_, &StudioPresenter::catalogChanged, this, &StudioBackupSettings::observe);
    connect(&presenter_, &StudioPresenter::libraryWorkChanged, this,
            &StudioBackupSettings::observe);
    connect(&presenter_, &StudioPresenter::errorChanged, this,
            [this]
            {
                if (saving_ && !presenter_.busy() && !presenter_.catalogOperationActive() &&
                    !presenter_.errorText().isEmpty())
                {
                    saving_ = false;
                    error_ = presenter_.errorText();
                    emit changed();
                }
            });
    connect(&commands_, &StudioCommandController::commandsChanged, this,
            &StudioBackupSettings::changed);
    observe();
}
bool StudioBackupSettings::enabled() const
{
    return values_.value("enabled").toBool();
}
QString StudioBackupSettings::directory() const
{
    return values_.value("directory").toString();
}
int StudioBackupSettings::intervalMinutes() const
{
    return values_.value("intervalMinutes").toInt();
}
int StudioBackupSettings::retentionCount() const
{
    return values_.value("retentionCount").toInt();
}
bool StudioBackupSettings::canApply() const
{
    return loaded_ && dirty() && !saving_ && !conflict_ &&
           commands_.action(QLatin1String(command::kLibraryBackupSchedule))
               .value("enabled")
               .toBool();
}
bool StudioBackupSettings::canEdit() const
{
    return loaded_ && !saving_ && !conflict_ &&
           commands_.action(QLatin1String(command::kLibraryBackupSchedule))
               .value("enabled")
               .toBool();
}
bool StudioBackupSettings::canRun() const
{
    return canEdit() && !dirty() && saved_.value("enabled").toBool() &&
           commands_.action(QLatin1String(command::kLibraryBackupScheduleRun))
               .value("enabled")
               .toBool();
}
QString StudioBackupSettings::disabledReason() const
{
    if (!loaded_)
        return tr("Open a catalog to configure automatic backups.");
    if (saving_)
        return tr("Saving backup settings…");
    if (conflict_)
        return error_;
    if (!dirty())
        return tr("No changes to save.");
    return commands_.action(QLatin1String(command::kLibraryBackupSchedule))
        .value("disabledReason")
        .toString();
}
QVariantMap StudioBackupSettings::limits() const
{
    return {{"intervalMin", static_cast<int>(kBackupScheduleIntervalMinutesMin)},
            {"intervalMax", static_cast<int>(kBackupScheduleIntervalMinutesMax)},
            {"retentionMin", kBackupRetentionCountMin},
            {"retentionMax", kBackupRetentionCountMax}};
}
void StudioBackupSettings::observe()
{
    const auto status = presenter_.backupScheduleStatus();
    const bool loaded = presenter_.catalogOpen() && status.value("loaded").toBool();
    const auto latest = fields(status);
    if (catalog_ != presenter_.catalogPath() || loaded_ != loaded)
    {
        ++generation_;
        folder_generation_.reset();
        catalog_ = presenter_.catalogPath();
        loaded_ = loaded;
        values_ = saved_ = latest;
        error_.clear();
        saving_ = conflict_ = false;
    }
    else if (latest == values_)
    {
        saved_ = latest;
        saving_ = false;
        conflict_ = false;
        error_.clear();
    }
    else if (latest != saved_)
    {
        if (dirty())
        {
            conflict_ = true;
            error_ = tr("Backup policy changed. Reload settings before saving.");
        }
        else
            values_ = latest;
        saved_ = latest;
    }
    emit changed();
}
void StudioBackupSettings::update(const QString &key, const QVariant &value)
{
    if (!canEdit() || values_.value(key) == value)
        return;
    values_.insert(key, value);
    if (!conflict_)
        error_.clear();
    emit changed();
}
void StudioBackupSettings::setEnabled(bool value)
{
    update("enabled", value);
}
void StudioBackupSettings::setDirectory(const QString &value)
{
    update("directory", value);
}
void StudioBackupSettings::setIntervalMinutes(int value)
{
    update("intervalMinutes", value);
}
void StudioBackupSettings::setRetentionCount(int value)
{
    update("retentionCount", value);
}
void StudioBackupSettings::reload()
{
    if (saving_)
        return;
    values_ = saved_;
    conflict_ = false;
    error_.clear();
    emit changed();
}
bool StudioBackupSettings::apply()
{
    if (!canApply() || catalog_ != presenter_.catalogPath())
        return false;
    const auto result =
        commands_.executeCommand(QLatin1String(command::kLibraryBackupSchedulePath), values_);
    if (!result.value("accepted").toBool())
        error_ = result.value("message").toString();
    else
    {
        saving_ = true;
        error_.clear();
    }
    emit changed();
    return result.value("accepted").toBool();
}
bool StudioBackupSettings::beginDirectorySelection()
{
    if (!canEdit())
        return false;
    folder_generation_ = generation_;
    return true;
}
bool StudioBackupSettings::acceptDirectory(const QString &directory)
{
    const bool current = folder_generation_ && *folder_generation_ == generation_ && canEdit();
    folder_generation_.reset();
    if (!current)
        return false;
    setDirectory(directory);
    return true;
}
bool StudioBackupSettings::runNow()
{
    if (!canRun())
        return false;
    return commands_.executeCommand(QLatin1String(command::kLibraryBackupScheduleRun))
        .value("accepted")
        .toBool();
}
JsonValue StudioBackupSettings::jsonSnapshot() const
{
    return JsonValue::Object{
        {"schema", "ravo.studio.backup_settings/v1"},
        {"catalog", utf8_from_qstring(catalog_)},
        {"loaded", loaded_},
        {"enabled", enabled()},
        {"directory", utf8_from_qstring(directory())},
        {"interval_minutes", JsonValue::number(std::to_string(intervalMinutes()))},
        {"retention_count", JsonValue::number(std::to_string(retentionCount()))},
        {"dirty", dirty()},
        {"saving", saving_},
        {"conflict", conflict_},
        {"error", utf8_from_qstring(error_)}};
}
} // namespace ravo
