#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerRecoveryCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;

    add(command::kLibraryRecoveryStatus, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshRecoveryStatus(); });
    add(command::kLibraryRecoverySync, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.synchronizeRecovery(); });
    add(command::kLibraryBackupCreate, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryBackupCreate, argument); });
    add(command::kLibraryBackupCreatePath, Condition::kCatalogReady, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.createBackupAtPath(argument.toString()); });
    add(command::kLibraryBackupVerify, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryBackupVerify, argument); });
    add(command::kLibraryBackupVerifyPath, Condition::kAlways, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.verifyBackupAtPath(argument.toString()); });
    add(command::kLibraryBackupRestore, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryBackupRestore, argument); });
    add(
        command::kLibraryBackupRestorePaths, Condition::kAlways,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("backup"), QStringLiteral("catalog")});
            if (!error.isEmpty())
                return error;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("backup"), QStringLiteral("catalog")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            if (values.value(QStringLiteral("backup")).metaType().id() != QMetaType::QString ||
                values.value(QStringLiteral("backup")).toString().trimmed().isEmpty() ||
                values.value(QStringLiteral("catalog")).metaType().id() != QMetaType::QString ||
                values.value(QStringLiteral("catalog")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Backup and restored catalog paths must not be empty.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto values = argument.toMap();
            presenter_.restoreBackupToPath(values.value(QStringLiteral("backup")).toString(),
                                           values.value(QStringLiteral("catalog")).toString());
        });
    add(command::kLibraryBackupSchedule, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryBackupSchedule, argument); });
    add(
        command::kLibraryBackupSchedulePath, Condition::kCatalogReady,
        [](const QVariant &argument)
        {
            const auto error = required_fields(argument, {QStringLiteral("directory"),
                                                          QStringLiteral("intervalMinutes"),
                                                          QStringLiteral("retentionCount")});
            if (!error.isEmpty())
                return error;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{
                QStringLiteral("directory"), QStringLiteral("intervalMinutes"),
                QStringLiteral("retentionCount"), QStringLiteral("enabled")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            if (values.contains(QStringLiteral("enabled")) &&
                values.value(QStringLiteral("enabled")).metaType().id() != QMetaType::Bool)
                return QStringLiteral("Backup enabled state must be boolean.");
            if (values.value(QStringLiteral("directory")).metaType().id() != QMetaType::QString ||
                (values.value(QStringLiteral("enabled"), true).toBool() &&
                 values.value(QStringLiteral("directory")).toString().trimmed().isEmpty()))
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Backup directory must not be empty.")));
            const auto interval = values.value(QStringLiteral("intervalMinutes"));
            const auto retention = values.value(QStringLiteral("retentionCount"));
            if (!numeric_argument(interval) ||
                std::floor(interval.toDouble()) != interval.toDouble() ||
                interval.toLongLong() < kBackupScheduleIntervalMinutesMin ||
                interval.toLongLong() > kBackupScheduleIntervalMinutesMax)
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Backup interval is outside the supported range.")));
            if (!numeric_argument(retention) ||
                std::floor(retention.toDouble()) != retention.toDouble() ||
                retention.toInt() < kBackupRetentionCountMin ||
                retention.toInt() > kBackupRetentionCountMax)
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Backup retention count is outside the supported range.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto values = argument.toMap();
            presenter_.configureBackupSchedule(
                values.value(QStringLiteral("directory")).toString(),
                values.value(QStringLiteral("intervalMinutes")).toInt(),
                values.value(QStringLiteral("retentionCount")).toInt(),
                values.value(QStringLiteral("enabled"), true).toBool());
        });
    add(command::kLibraryBackupScheduleDisable, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.disableBackupSchedule(); });
    add(command::kLibraryBackupScheduleRun, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.runScheduledBackupNow(); });
}

} // namespace ravo
