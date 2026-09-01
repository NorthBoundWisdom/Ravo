#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_internal.h"
#include "studio_command_ids.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QHash>
#include <QKeySequence>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <Qt>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/develop.h"
#include "studio_qt.h"

namespace ravo
{
using namespace command_internal;

namespace
{

enum class Condition
{
    kAlways,
    kCatalogOpen,
    kCatalogReady,
    kSelection,
    kReadySelection,
    kNonGrid,
    kSurveySelection,
    kDevelop,
    kDevelopSelection,
    kModifiedParameters,
    kCanUndo,
    kCanRedo,
    kCanPasteParameters,
    kCanPasteParametersToSelection,
    kCanDelete,
    kCatalogOperation,
};

using Validator = std::function<QString(const QVariant &)>;
using Handler = std::function<void(const QVariant &, const QString &)>;

struct CommandSpec
{
    QString id;
    Condition condition = Condition::kAlways;
    Validator validator;
    Handler handler;
};

struct State
{
    bool enabled = true;
    QString reason;
};

QString no_argument(const QVariant &argument)
{
    return argument.isValid() && !argument.isNull() ?
               QStringLiteral("This command takes no argument.") :
               QString{};
}

QString non_empty_string(const QVariant &argument)
{
    return argument.metaType().id() != QMetaType::QString ||
                   argument.toString().trimmed().isEmpty() ?
               QStringLiteral("A non-empty string is required.") :
               QString{};
}

QString map_argument(const QVariant &argument)
{
    return argument.canConvert<QVariantMap>() ? QString{} :
                                                QStringLiteral("An object argument is required.");
}

QString list_argument(const QVariant &argument)
{
    if (!argument.canConvert<QVariantList>() && !argument.canConvert<QStringList>())
        return QStringLiteral("A non-empty list of paths is required.");
    const auto values = argument.toList().isEmpty() ? QVariantList{} : argument.toList();
    if (!values.isEmpty())
    {
        for (const auto &value : values)
            if (value.metaType().id() != QMetaType::QString || value.toString().trimmed().isEmpty())
                return QStringLiteral("Every import path must be a non-empty string.");
        return {};
    }
    const auto strings = argument.toStringList();
    if (strings.isEmpty())
        return QStringLiteral("A non-empty list of paths is required.");
    for (const auto &value : strings)
        if (value.trimmed().isEmpty())
            return QStringLiteral("Every import path must be a non-empty string.");
    return {};
}

bool numeric_argument(const QVariant &argument)
{
    switch (argument.metaType().id())
    {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

QString finite_number(const QVariant &argument, const QString &name)
{
    return numeric_argument(argument) && std::isfinite(argument.toDouble()) ?
               QString{} :
               tr_command(QString::fromUtf8(
                              QT_TRANSLATE_NOOP("StudioCommands", "%1 must be a finite number.")))
                   .arg(name);
}

QString required_fields(const QVariant &argument, const QStringList &fields)
{
    const auto map_error = map_argument(argument);
    if (!map_error.isEmpty())
        return map_error;
    const auto values = argument.toMap();
    for (const auto &field : fields)
        if (!values.contains(field))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Missing command argument field: %1.")))
                .arg(field);
    return {};
}

QString preset_identity_argument(const QVariant &argument)
{
    const auto error = required_fields(argument, {QStringLiteral("path"), QStringLiteral("name")});
    if (!error.isEmpty())
        return error;
    const auto values = argument.toMap();
    static const QSet<QString> allowed{QStringLiteral("path"), QStringLiteral("name")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Unknown command argument field: %1.")))
                .arg(it.key());
    }
    const auto path = values.value(QStringLiteral("path"));
    const auto name = values.value(QStringLiteral("name"));
    return path.metaType().id() == QMetaType::QString && !path.toString().trimmed().isEmpty() &&
                   name.metaType().id() == QMetaType::QString &&
                   !name.toString().trimmed().isEmpty() ?
               QString{} :
               QStringLiteral("Preset path and name must be non-empty strings.");
}

QString one_of(const QVariant &argument, const QSet<QString> &values, const QString &name)
{
    return argument.metaType().id() == QMetaType::QString && values.contains(argument.toString()) ?
               QString{} :
               tr_command(
                   QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Unknown %1 value.")))
                   .arg(name);
}

QStringList strings_from(const QVariant &argument)
{
    auto result = argument.toStringList();
    if (!result.isEmpty())
        return result;
    for (const auto &value : argument.toList())
        result.push_back(value.toString());
    return result;
}

QString develop_parameter_fields_argument(const QVariant &argument)
{
    const bool variant_list = argument.metaType().id() == QMetaType::QVariantList;
    const bool string_list = argument.metaType().id() == QMetaType::QStringList;
    if (!variant_list && !string_list)
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    const QVariantList fields = variant_list ? argument.toList() : QVariantList{};
    const QStringList strings = string_list ? argument.toStringList() : strings_from(argument);
    if (strings.isEmpty() ||
        strings.size() > static_cast<qsizetype>(develop_selectable_field_names().size()))
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    if (variant_list && std::any_of(fields.cbegin(), fields.cend(), [](const QVariant &field)
                                    { return field.metaType().id() != QMetaType::QString; }))
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    QSet<QString> unique;
    for (const auto &field : strings)
    {
        if (field.isEmpty() || !is_develop_selectable_field(utf8_from_qstring(field)) ||
            unique.contains(field))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                "StudioCommands", "Parameter fields contain an unsupported or duplicate value.")));
        unique.insert(field);
    }
    return {};
}

QString preset_save_argument(const QVariant &argument)
{
    const auto error =
        required_fields(argument, {QStringLiteral("name"), QStringLiteral("fields")});
    if (!error.isEmpty())
        return error;
    const auto values = argument.toMap();
    static const QSet<QString> allowed{QStringLiteral("name"), QStringLiteral("fields")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Unknown command argument field: %1.")))
                .arg(it.key());
    }
    const auto name = values.value(QStringLiteral("name"));
    if (name.metaType().id() != QMetaType::QString || name.toString().trimmed().isEmpty())
        return tr_command(QString::fromUtf8(
            QT_TRANSLATE_NOOP("StudioCommands", "Preset name must be a non-empty string.")));

    return develop_parameter_fields_argument(values.value(QStringLiteral("fields")));
}

QVariantMap accepted()
{
    return {{QStringLiteral("accepted"), true},
            {QStringLiteral("code"), QStringLiteral("accepted")},
            {QStringLiteral("message"), QString{}}};
}

QVariantMap rejected(const QString &code, const QString &message)
{
    return {{QStringLiteral("accepted"), false},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}};
}

template <typename Range>
QSet<QString> string_set(const Range &values)
{
    QSet<QString> result;
    for (const auto &value : values)
        result.insert(value);
    return result;
}

} // namespace

struct StudioCommandController::Impl
{
    QHash<QString, CommandSpec> commands;
    QVector<ActionSpec> actions = builtin_actions();
    qulonglong confirmation_revision = 0;
    QString pending_confirmation_command;
    QString pending_confirmation_token;
    std::vector<std::string> pending_confirmation_assets;
    QVariant pending_confirmation_argument;
};

StudioCommandController::StudioCommandController(StudioPresenter &presenter, QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , impl_(std::make_unique<Impl>())
{
    const auto add =
        [this](const char *id, const Condition condition, Validator validator, Handler handler)
    {
        const auto key = QLatin1String(id);
        if (impl_->commands.contains(key))
            qFatal("Duplicate Studio command: %s", id);
        impl_->commands.insert(key, {key, condition, std::move(validator), std::move(handler)});
    };
    const auto present = [this](const char *id, const QVariant &argument)
    { emit presentationCommandRequested(QLatin1String(id), argument); };
    const auto request_confirmation = [this](const char *request_id, const char *confirmed_id)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets = presenter_.selected_asset_ids();
        impl_->pending_confirmation_argument.clear();
        emit presentationCommandRequested(QLatin1String(request_id),
                                          impl_->pending_confirmation_token);
    };
    const auto confirmation_validator = [this](const char *confirmed_id, const QVariant &argument)
    {
        if (argument.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != argument.toString())
            return QStringLiteral("A current confirmation token is required.");
        if (impl_->pending_confirmation_assets != presenter_.selected_asset_ids())
            return QStringLiteral("The photo selection changed after confirmation was requested.");
        return QString{};
    };
    const auto request_preset_confirmation =
        [this](const char *request_id, const char *confirmed_id, const QVariant &argument)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument = argument;
        auto presentation = argument.toMap();
        presentation.insert(QStringLiteral("token"), impl_->pending_confirmation_token);
        emit presentationCommandRequested(QLatin1String(request_id), presentation);
    };
    const auto preset_confirmation_validator =
        [this](const char *confirmed_id, const QVariant &argument)
    {
        const auto field_error =
            required_fields(argument, {QStringLiteral("token"), QStringLiteral("path")});
        if (!field_error.isEmpty())
            return field_error;
        const auto fields = argument.toMap();
        static const QSet<QString> allowed{QStringLiteral("token"), QStringLiteral("path")};
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        {
            if (!allowed.contains(it.key()))
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                      "StudioCommands", "Unknown command argument field: %1.")))
                    .arg(it.key());
        }
        const auto token = fields.value(QStringLiteral("token"));
        const auto path = fields.value(QStringLiteral("path"));
        if (token.metaType().id() != QMetaType::QString ||
            path.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != token.toString())
            return QStringLiteral("A current confirmation token is required.");
        const auto pending = impl_->pending_confirmation_argument.toMap();
        if (pending.value(QStringLiteral("path")).toString() != path.toString())
            return QStringLiteral("The preset changed after confirmation was requested.");
        return QString{};
    };
    const auto clear_confirmation = [this]()
    {
        impl_->pending_confirmation_command.clear();
        impl_->pending_confirmation_token.clear();
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument.clear();
    };

    add(command::kLibraryCreate, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryCreate, argument); });
    add(command::kLibraryCreatePath, Condition::kAlways, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.createCatalogFromPath(argument.toString()); });
    add(command::kLibraryOpen, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryOpen, argument); });
    add(command::kLibraryOpenPath, Condition::kAlways, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.openCatalogFromPath(argument.toString()); });
    add(command::kLibraryImportFiles, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryImportFiles, argument); });
    add(command::kLibraryImportPaths, Condition::kCatalogReady, list_argument,
        [this](const QVariant &argument, const QString &)
        { presenter_.importFilePaths(strings_from(argument)); });
    add(command::kLibraryImportFolder, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryImportFolder, argument); });
    add(command::kLibraryImportFolderPath, Condition::kCatalogReady, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.importFolderFromPath(argument.toString()); });
    add(command::kLibraryExport, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryExport, argument); });
    add(
        command::kLibraryExportWrite, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            const auto fields =
                required_fields(argument, {QStringLiteral("path"), QStringLiteral("format"),
                                           QStringLiteral("options")});
            if (!fields.isEmpty())
                return fields;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("path"), QStringLiteral("format"),
                                               QStringLiteral("options")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            {
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            }
            static const QSet<QString> formats{QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("tiff"), QStringLiteral("original")};
            const auto format_error =
                one_of(values.value(QStringLiteral("format")), formats, QStringLiteral("format"));
            if (!format_error.isEmpty())
                return format_error;
            const auto path = values.value(QStringLiteral("path"));
            if (path.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export path must be a string.")));
            if (path.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export path must not be empty.")));
            const auto options = values.value(QStringLiteral("options"));
            if (options.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export options must be an object.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.exportSelectedToPath(fields.value(QStringLiteral("path")).toString(),
                                            fields.value(QStringLiteral("format")).toString(),
                                            fields.value(QStringLiteral("options")).toMap());
        });
    add(
        command::kEditSetText, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Develop text control name must not be empty.")));
            return fields.value(QStringLiteral("value")).metaType().id() == QMetaType::QString ?
                       QString{} :
                       tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                           "StudioCommands", "Develop text value must be text.")));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setDevelopText(fields.value(QStringLiteral("name")).toString(),
                                      fields.value(QStringLiteral("value")).toString());
        });
    add(
        command::kLibraryExportBatchWrite, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            const auto fields = required_fields(
                argument, {QStringLiteral("directory"), QStringLiteral("filenameTemplate"),
                           QStringLiteral("format"), QStringLiteral("options")});
            if (!fields.isEmpty())
                return fields;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("directory"),
                                               QStringLiteral("filenameTemplate"),
                                               QStringLiteral("format"), QStringLiteral("options")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            {
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            }
            static const QSet<QString> formats{QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("tiff"), QStringLiteral("original")};
            const auto format_error =
                one_of(values.value(QStringLiteral("format")), formats, QStringLiteral("format"));
            if (!format_error.isEmpty())
                return format_error;
            const auto directory = values.value(QStringLiteral("directory"));
            if (directory.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must be a string.")));
            if (directory.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must not be empty.")));
            const auto filename_template = values.value(QStringLiteral("filenameTemplate"));
            if (filename_template.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Export filename template must be a string.")));
            if (filename_template.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Export filename template must not be empty.")));
            const auto options = values.value(QStringLiteral("options"));
            if (options.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export options must be an object.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.exportSelectedToDirectory(
                fields.value(QStringLiteral("directory")).toString(),
                fields.value(QStringLiteral("filenameTemplate")).toString(),
                fields.value(QStringLiteral("format")).toString(),
                fields.value(QStringLiteral("options")).toMap());
        });
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
            static const QSet<QString> allowed{QStringLiteral("directory"),
                                               QStringLiteral("intervalMinutes"),
                                               QStringLiteral("retentionCount")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            if (values.value(QStringLiteral("directory")).metaType().id() != QMetaType::QString ||
                values.value(QStringLiteral("directory")).toString().trimmed().isEmpty())
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
                values.value(QStringLiteral("retentionCount")).toInt(), true);
        });
    add(command::kLibraryBackupScheduleDisable, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.disableBackupSchedule(); });
    add(command::kLibraryBackupScheduleRun, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.runScheduledBackupNow(); });
    add(command::kLibraryPreviewRebuildSelected, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rebuildSelectedPreviews(); });
    add(command::kLibraryPreviewRebuildAll, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rebuildAllPreviews(); });
    add(command::kLibraryCancelOperation, Condition::kCatalogOperation, no_argument,
        [this](const QVariant &, const QString &) { presenter_.cancelCatalogOperation(); });
    add(command::kStyleSave, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleSave, argument); });
    add(command::kStyleSavePath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.saveStyleToPath(argument.toString()); });
    add(command::kStyleApply, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleApply, argument); });
    add(command::kStyleApplyPath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.applyStyleFromPath(argument.toString()); });
    add(command::kPresetImport, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetImport, argument); });
    add(command::kPresetImportPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.importPresetFromPath(argument.toString()); });
    add(command::kPresetApplyPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.applyStyleFromPath(argument.toString()); });
    add(command::kPresetSave, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetSave, argument); });
    add(command::kPresetSaveSelected, Condition::kDevelopSelection, preset_save_argument,
        [this](const QVariant &argument, const QString &)
        {
            const auto values = argument.toMap();
            QVariantList fields = values.value(QStringLiteral("fields")).toList();
            if (fields.isEmpty())
            {
                for (const auto &field : values.value(QStringLiteral("fields")).toStringList())
                    fields.push_back(field);
            }
            presenter_.savePreset(values.value(QStringLiteral("name")).toString(), fields);
        });
    add(command::kPresetCopyInfo, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.copyPresetDebugInfo(argument.toString()); });
    add(command::kPresetRename, Condition::kCatalogOpen, preset_identity_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetRename, argument); });
    add(command::kPresetRenamePath, Condition::kCatalogOpen, preset_identity_argument,
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.renamePreset(fields.value(QStringLiteral("path")).toString(),
                                    fields.value(QStringLiteral("name")).toString());
        });
    add(command::kPresetRequestDelete, Condition::kCatalogOpen, preset_identity_argument,
        [request_preset_confirmation](const QVariant &argument, const QString &)
        {
            request_preset_confirmation(command::kPresetRequestDelete, command::kPresetDelete,
                                        argument);
        });
    add(
        command::kPresetDelete, Condition::kCatalogOpen,
        [preset_confirmation_validator](const QVariant &argument)
        { return preset_confirmation_validator(command::kPresetDelete, argument); },
        [this, clear_confirmation](const QVariant &argument, const QString &)
        {
            const QString path = argument.toMap().value(QStringLiteral("path")).toString();
            clear_confirmation();
            presenter_.deletePreset(path);
        });
    add(
        command::kLibrarySetTagFilter, Condition::kCatalogOpen, [](const QVariant &)
        { return QString{}; }, [this](const QVariant &argument, const QString &)
        { presenter_.setTagFilter(argument.toString()); });
    add(
        command::kLibrarySetRatingFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("mode"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            static const QSet<QString> modes{QStringLiteral("any"), QStringLiteral("min"),
                                             QStringLiteral("exact")};
            if (!modes.contains(fields.value(QStringLiteral("mode")).toString()))
                return QStringLiteral("Unknown rating filter mode.");
            const auto value = fields.value(QStringLiteral("value"));
            const double number = value.toDouble();
            return numeric_argument(value) && std::isfinite(number) &&
                           std::floor(number) == number && number >= 0.0 && number <= 5.0 ?
                       QString{} :
                       QStringLiteral("Rating filter value must be an integer from 0 to 5.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setRatingFilter(fields.value(QStringLiteral("mode")).toString(),
                                       fields.value(QStringLiteral("value")).toInt());
        });
    add(
        command::kLibraryToggleColorFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("red"), QStringLiteral("yellow"),
                                              QStringLiteral("green"), QStringLiteral("blue"),
                                              QStringLiteral("purple")};
            return one_of(argument, values, QStringLiteral("color filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.toggleColorFilter(argument.toString()); });
    add(
        command::kLibrarySetRejectFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("include"), QStringLiteral("exclude"),
                                              QStringLiteral("only")};
            return one_of(argument, values, QStringLiteral("reject filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setRejectFilter(argument.toString()); });
    add(
        command::kLibrarySetTextFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::QString ?
                       QString{} :
                       QStringLiteral("Library text filter must be a string.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setFilterText(argument.toString()); });
    add(
        command::kLibrarySetMediaFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("any"), QStringLiteral("raw"),
                                              QStringLiteral("jpeg"), QStringLiteral("png"),
                                              QStringLiteral("tiff")};
            return one_of(argument, values, QStringLiteral("media filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setMediaFilter(argument.toString()); });
    add(
        command::kLibrarySetEditFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("any"), QStringLiteral("edited"),
                                              QStringLiteral("unedited")};
            return one_of(argument, values, QStringLiteral("edit filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setEditFilter(argument.toString()); });
    add(
        command::kLibrarySetSort, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("field"), QStringLiteral("direction")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            static const QSet<QString> sort_fields{
                QStringLiteral("imported"), QStringLiteral("captured"), QStringLiteral("name"),
                QStringLiteral("rating"), QStringLiteral("size")};
            static const QSet<QString> directions{QStringLiteral("asc"), QStringLiteral("desc")};
            if (!sort_fields.contains(fields.value(QStringLiteral("field")).toString()))
                return QStringLiteral("Unknown library sort field.");
            return directions.contains(fields.value(QStringLiteral("direction")).toString()) ?
                       QString{} :
                       QStringLiteral("Unknown library sort direction.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setSort(fields.value(QStringLiteral("field")).toString(),
                               fields.value(QStringLiteral("direction")).toString());
        });
    add(command::kLibraryClearFilters, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.clearFilters(); });
    add(
        command::kLibrarySelectFolder, Condition::kCatalogOpen, [](const QVariant &)
        { return QString{}; }, [this](const QVariant &argument, const QString &)
        { presenter_.selectFolder(argument.toString()); });
    add(command::kLibrarySelectLastImport, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.selectLastImport(); });
    add(command::kLibrarySelectSet, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.selectLibrarySet(argument.toString()); });
    add(command::kLibraryCreateManualSet, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.createManualLibrarySet(argument.toString()); });
    add(command::kLibraryCreateSmartSet, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.createSmartLibrarySet(argument.toString()); });
    add(
        command::kLibraryRenameSet, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("setId"), QStringLiteral("name")});
            if (!error.isEmpty())
                return error;
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.renameLibrarySet(fields.value(QStringLiteral("setId")).toString(),
                                        fields.value(QStringLiteral("name")).toString());
        });
    add(command::kLibraryDeleteSet, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.deleteLibrarySet(argument.toString()); });
    add(command::kLibraryAddSelectionToSet, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.addSelectionToLibrarySet(argument.toString()); });
    add(command::kLibraryRemoveSelectionFromSet, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.removeSelectionFromLibrarySet(argument.toString()); });
    add(command::kLibraryToggleStackCollapse, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setCollapseStacks(!presenter_.collapseStacks()); });
    add(command::kLibraryFolderRelink, Condition::kCatalogReady, non_empty_string,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryFolderRelink, argument); });
    add(
        command::kLibraryFolderRelinkPath, Condition::kCatalogReady,
        [](const QVariant &argument)
        {
            const auto error = required_fields(
                argument, {QStringLiteral("folderId"), QStringLiteral("directory")});
            if (!error.isEmpty())
                return error;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("folderId"),
                                               QStringLiteral("directory")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            if (values.value(QStringLiteral("folderId")).metaType().id() != QMetaType::QString ||
                values.value(QStringLiteral("folderId")).toString().trimmed().isEmpty() ||
                values.value(QStringLiteral("directory")).metaType().id() != QMetaType::QString ||
                values.value(QStringLiteral("directory")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Folder identity and replacement path must not be empty.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto values = argument.toMap();
            presenter_.relinkFolder(values.value(QStringLiteral("folderId")).toString(),
                                    values.value(QStringLiteral("directory")).toString());
        });

    add(
        command::kPhotoSelect, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (argument.metaType().id() == QMetaType::QString)
                return argument.toString().trimmed().isEmpty() ?
                           QStringLiteral("An asset ID is required.") :
                           QString{};
            const auto error = required_fields(argument, {QStringLiteral("id")});
            return !error.isEmpty() || argument.toMap()
                                           .value(QStringLiteral("id"))
                                           .toString()
                                           .trimmed()
                                           .isEmpty() ?
                       QStringLiteral("An asset ID is required.") :
                       QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            QString asset_id = argument.toString();
            QString mode = QStringLiteral("single");
            const auto fields = argument.toMap();
            if (!fields.isEmpty())
            {
                asset_id = fields.value(QStringLiteral("id")).toString();
                if (fields.contains(QStringLiteral("modifiers")))
                {
                    const auto modifiers = Qt::KeyboardModifiers(static_cast<unsigned int>(
                        fields.value(QStringLiteral("modifiers")).toInt()));
                    if (modifiers.testFlag(Qt::ShiftModifier))
                        mode = QStringLiteral("range");
                    else if (modifiers.testFlag(Qt::ControlModifier) ||
                             modifiers.testFlag(Qt::MetaModifier))
                        mode = QStringLiteral("toggle");
                }
                else
                    mode =
                        fields.value(QStringLiteral("mode"), QStringLiteral("single")).toString();
            }
            if (mode == QLatin1String("range"))
                presenter_.selectAssetRange(asset_id);
            else if (mode == QLatin1String("toggle"))
                presenter_.toggleAssetSelected(asset_id);
            else
                presenter_.selectAsset(asset_id);
        });
    add(
        command::kPhotoSetRating, Condition::kSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 && value <= 5.0 ?
                       QString{} :
                       QStringLiteral("Rating must be an integer between 0 and 5.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setRating(argument.toInt()); });
    add(
        command::kPhotoSetColor, Condition::kSelection,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("none"),   QStringLiteral("red"),
                                              QStringLiteral("yellow"), QStringLiteral("green"),
                                              QStringLiteral("blue"),   QStringLiteral("purple")};
            return values.contains(argument.toString()) ? QString{} :
                                                          QStringLiteral("Unknown color label.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setColorLabel(argument.toString()); });
    add(
        command::kPhotoSetTags, Condition::kSelection, [](const QVariant &) { return QString{}; },
        [this](const QVariant &argument, const QString &)
        { presenter_.setAssetTags(argument.toString()); });
    add(
        command::kPhotoSetMetadata, Condition::kSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            static const QSet<QString> names{QStringLiteral("title"), QStringLiteral("description"),
                                             QStringLiteral("creator"),
                                             QStringLiteral("copyright")};
            return names.contains(argument.toMap().value(QStringLiteral("name")).toString()) ?
                       QString{} :
                       QStringLiteral("Unknown writable metadata field.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setMetadataField(fields.value(QStringLiteral("name")).toString(),
                                        fields.value(QStringLiteral("value")).toString());
        });
    add(command::kPhotoRefreshMetadata, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshSelectedMetadata(); });
    add(
        command::kPhotoCreateSnapshot, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::QString)
                return QString{};
            return QStringLiteral("Snapshot label must be a string.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.createSnapshot(argument.toString()); });
    add(
        command::kPhotoRenameSnapshot, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("id"), QStringLiteral("label")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            const auto id = fields.value(QStringLiteral("id"));
            const double number = id.toDouble();
            if (!(numeric_argument(id) && std::isfinite(number) && std::floor(number) == number &&
                  number >= 0.0))
                return QStringLiteral("A non-negative integer history ID is required.");
            if (fields.value(QStringLiteral("label")).metaType().id() != QMetaType::QString)
                return QStringLiteral("Snapshot label must be a string.");
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.renameSnapshot(fields.value(QStringLiteral("id")).toInt(),
                                      fields.value(QStringLiteral("label")).toString());
        });
    add(
        command::kPhotoRestoreHistory, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 ?
                       QString{} :
                       QStringLiteral("A non-negative integer history ID is required.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.restoreHistory(argument.toInt()); });
    add(command::kPhotoToggleReject, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleRejected(); });
    add(command::kPhotoCopyInfo, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.copySelectedPhotoDebugInfo(); });
    add(command::kPhotoCopyParameters, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.copySelectedPhotoParametersDebugInfo(); });
    add(command::kPhotoRequestRemove, Condition::kSelection, no_argument,
        [request_confirmation](const QVariant &, const QString &)
        { request_confirmation(command::kPhotoRequestRemove, command::kPhotoRemove); });
    add(
        command::kPhotoRemove, Condition::kSelection,
        [confirmation_validator](const QVariant &argument)
        { return confirmation_validator(command::kPhotoRemove, argument); },
        [this, clear_confirmation](const QVariant &, const QString &)
        {
            clear_confirmation();
            presenter_.remove_selected_from_catalog();
        });
    add(command::kPhotoRequestDelete, Condition::kCanDelete, no_argument,
        [request_confirmation](const QVariant &, const QString &)
        { request_confirmation(command::kPhotoRequestDelete, command::kPhotoDelete); });
    add(
        command::kPhotoDelete, Condition::kCanDelete,
        [confirmation_validator](const QVariant &argument)
        { return confirmation_validator(command::kPhotoDelete, argument); },
        [this, clear_confirmation](const QVariant &, const QString &)
        {
            clear_confirmation();
            presenter_.remove_selected_from_disk();
        });
    const auto navigation_validator = [](const QVariant &argument)
    {
        return !argument.isValid() || argument.isNull() ||
                       argument.toString() == QLatin1String("range") ?
                   QString{} :
                   QStringLiteral("Navigation argument must be 'range'.");
    };
    add(command::kPhotoPrevious, Condition::kSelection, navigation_validator,
        [this](const QVariant &argument, const QString &)
        {
            if (argument.toString() != QLatin1String("range"))
                presenter_.selectPrevious();
            else if (const int row = presenter_.selectedIndex(); row > 0)
                presenter_.selectAssetRange(presenter_.assets()->assetIdAt(row - 1));
        });
    add(command::kPhotoNext, Condition::kSelection, navigation_validator,
        [this](const QVariant &argument, const QString &)
        {
            if (argument.toString() != QLatin1String("range"))
                presenter_.selectNext();
            else if (const int row = presenter_.selectedIndex();
                     row >= 0 && row + 1 < presenter_.assets()->rowCount())
                presenter_.selectAssetRange(presenter_.assets()->assetIdAt(row + 1));
        });

    add(command::kPhotoCreateVersion, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.createAssetVersion(); });
    add(
        command::kPhotoStackSelection, Condition::kReadySelection,
        [this](const QVariant &)
        {
            return presenter_.selectedCount() >= 2 ?
                       QString{} :
                       QStringLiteral("Select at least two photos to stack.");
        },
        [this](const QVariant &, const QString &) { presenter_.stackSelection(); });
    add(command::kPhotoUnstack, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.unstackSelection(); });
    add(command::kPhotoSetStackPick, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.setSelectedStackPick(); });
    add(command::kViewGrid, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.returnToGrid(); });
    add(command::kViewLoupe, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openLoupe(); });
    add(command::kViewDevelop, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openDevelop(); });
    add(command::kViewSurvey, Condition::kSurveySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openSurvey(); });
    add(command::kViewFit, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("fit")); });
    add(command::kViewFill, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("fill")); });
    add(command::kViewActual, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("actual")); });
    add(command::kViewToggleActualSize, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleActualSize(); });
    add(
        command::kViewSetZoomMode, Condition::kNonGrid,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("fit"), QStringLiteral("fill"),
                                              QStringLiteral("actual")};
            return one_of(argument, values, QStringLiteral("zoom mode"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setZoomMode(argument.toString()); });
    add(
        command::kViewAdjustZoom, Condition::kNonGrid, [](const QVariant &argument)
        { return finite_number(argument, QStringLiteral("Zoom delta")); },
        [this](const QVariant &argument, const QString &)
        { presenter_.adjustZoom(argument.toInt()); });
    add(
        command::kViewSetThumbnailSize, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 120.0 && value <= 320.0 ?
                       QString{} :
                       QStringLiteral("Thumbnail size must be an integer between 120 and 320.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setThumbnailSize(argument.toInt()); });
    add(command::kViewTogglePhotoInfo, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &)
        {
            photo_info_visible_ = !photo_info_visible_;
            emit photoInfoVisibleChanged();
            refresh();
        });
    add(
        command::kViewSetScopeMode, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{
                QStringLiteral("histogram"), QStringLiteral("waveform"), QStringLiteral("parade"),
                QStringLiteral("vectorscope"), QStringLiteral("split")};
            return one_of(argument, values, QStringLiteral("scope mode"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setScopeMode(argument.toString()); });

    add(command::kEditUndo, Condition::kCanUndo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.undoEdit(); });
    add(command::kEditRedo, Condition::kCanRedo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.redoEdit(); });
    add(command::kEditCopyParameters, Condition::kModifiedParameters, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kEditCopyParameters, argument); });
    add(command::kEditCopyParametersSelected, Condition::kModifiedParameters,
        develop_parameter_fields_argument,
        [this](const QVariant &argument, const QString &)
        {
            QVariantList fields = argument.toList();
            if (fields.isEmpty())
            {
                for (const auto &field : argument.toStringList())
                    fields.push_back(field);
            }
            presenter_.copyParametersSelected(fields);
        });
    add(command::kEditPasteParameters, Condition::kCanPasteParameters, no_argument,
        [this](const QVariant &, const QString &) { presenter_.pasteParameters(); });
    add(command::kEditPasteParametersToSelection, Condition::kCanPasteParametersToSelection,
        no_argument,
        [this](const QVariant &, const QString &) { presenter_.pasteParametersToSelection(); });
    add(command::kEditResetAll, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.resetAllEdits(); });
    add(command::kEditResetSection, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.resetSection(argument.toString()); });
    add(
        command::kEditSetSectionEnabled, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("section"), QStringLiteral("enabled")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("section")).toString().trimmed().isEmpty())
                return QStringLiteral("Develop section name must not be empty.");
            const auto enabled = fields.value(QStringLiteral("enabled"));
            return enabled.metaType().id() == QMetaType::Bool || enabled.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("Develop section enabled must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setSectionEffectEnabled(fields.value(QStringLiteral("section")).toString(),
                                               fields.value(QStringLiteral("enabled")).toBool());
        });
    add(command::kEditResetControl, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.resetControl(argument.toString()); });
    add(
        command::kEditSetNumber, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                return QStringLiteral("Develop control name must not be empty.");
            return finite_number(fields.value(QStringLiteral("value")),
                                 QStringLiteral("Develop value"));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const auto name = fields.value(QStringLiteral("name")).toString();
            const double value = fields.value(QStringLiteral("value")).toDouble();
            if (!std::isfinite(value))
            {
                presenter_.setError(tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Develop value must be finite."))));
                return;
            }
            if (fields.value(QStringLiteral("live")).toBool())
                presenter_.previewDevelopNumber(name, value);
            else
                presenter_.setDevelopNumber(name, value);
        });
    add(
        command::kEditSetNumbers, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error = required_fields(argument, {QStringLiteral("fields")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap().value(QStringLiteral("fields")).toMap();
            if (fields.isEmpty())
                return QStringLiteral("Develop fields must not be empty.");
            for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
            {
                if (it.key().trimmed().isEmpty())
                    return QStringLiteral("Develop control name must not be empty.");
                const auto number_error =
                    finite_number(it.value(), QStringLiteral("Develop value"));
                if (!number_error.isEmpty())
                    return number_error;
            }
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto payload = argument.toMap();
            const auto fields = payload.value(QStringLiteral("fields")).toMap();
            if (payload.value(QStringLiteral("live")).toBool())
                presenter_.previewDevelopNumbers(fields);
            else
                presenter_.setDevelopNumbers(fields);
        });
    add(
        command::kEditPickWhiteBalance, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("x"), QStringLiteral("y")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            const auto x_error =
                finite_number(fields.value(QStringLiteral("x")), QStringLiteral("White-balance X"));
            if (!x_error.isEmpty())
                return x_error;
            return finite_number(fields.value(QStringLiteral("y")),
                                 QStringLiteral("White-balance Y"));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.pickWhiteBalance(fields.value(QStringLiteral("x")).toDouble(),
                                        fields.value(QStringLiteral("y")).toDouble());
        });
    add(
        command::kEditSetWhiteBalancePick, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::Bool || argument.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("White-balance pick state must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setWhiteBalancePickActive(argument.toBool()); });
    add(
        command::kEditPlaceMask, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("x"), QStringLiteral("y")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            const auto x_error =
                finite_number(fields.value(QStringLiteral("x")), QStringLiteral("Mask place X"));
            if (!x_error.isEmpty())
                return x_error;
            return finite_number(fields.value(QStringLiteral("y")), QStringLiteral("Mask place Y"));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.placeMask(fields.value(QStringLiteral("x")).toDouble(),
                                 fields.value(QStringLiteral("y")).toDouble());
        });
    add(
        command::kEditSetMaskPlace, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::Bool || argument.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("Mask place state must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setMaskPlaceActive(argument.toBool()); });
    add(
        command::kEditSetToneCurve, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error = required_fields(argument, {QStringLiteral("points")});
            if (!error.isEmpty())
                return error;
            return argument.toMap().value(QStringLiteral("points")).canConvert<QVariantList>() ?
                       QString{} :
                       QStringLiteral("Tone curve points must be a list.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const auto family = fields.value(QStringLiteral("family")).toString();
            const int channel = fields.value(QStringLiteral("channel"), 0).toInt();
            const auto points = fields.value(QStringLiteral("points")).toList();
            if (fields.value(QStringLiteral("live")).toBool())
            {
                if (family.isEmpty())
                    presenter_.previewToneCurve(points);
                else
                    presenter_.previewCurvePoints(family, channel, points);
            }
            else if (family.isEmpty())
            {
                presenter_.setToneCurve(points);
            }
            else
            {
                presenter_.setCurvePoints(family, channel, points);
            }
        });
    add(
        command::kEditAddRetouchRegion, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            return required_fields(
                argument,
                {QStringLiteral("mode"), QStringLiteral("centerX"), QStringLiteral("centerY"),
                 QStringLiteral("radius"), QStringLiteral("feather"), QStringLiteral("opacity"),
                 QStringLiteral("sourceX"), QStringLiteral("sourceY"), QStringLiteral("blurType"),
                 QStringLiteral("blurRadius"), QStringLiteral("fillMode"), QStringLiteral("fillR"),
                 QStringLiteral("fillG"), QStringLiteral("fillB"),
                 QStringLiteral("fillBrightness")});
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.addRetouchRegion(argument.toMap()); });
    add(
        command::kEditRemoveRetouchRegion, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 &&
                           value <= static_cast<double>(std::numeric_limits<int>::max()) ?
                       QString{} :
                       QStringLiteral("Retouch region index must be a non-negative integer.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.removeRetouchRegion(argument.toInt()); });
    add(
        command::kEditSetCrop, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const QStringList names{QStringLiteral("x"), QStringLiteral("y"),
                                    QStringLiteral("width"), QStringLiteral("height")};
            const auto error = required_fields(argument, names);
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            for (const auto &name : names)
            {
                const auto number_error = finite_number(fields.value(name), name);
                if (!number_error.isEmpty())
                    return number_error;
            }
            return fields.value(QStringLiteral("width")).toDouble() > 0.0 &&
                           fields.value(QStringLiteral("height")).toDouble() > 0.0 ?
                       QString{} :
                       QStringLiteral("Crop width and height must be positive.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const double x = fields.value(QStringLiteral("x")).toDouble();
            const double y = fields.value(QStringLiteral("y")).toDouble();
            const double width = fields.value(QStringLiteral("width")).toDouble();
            const double height = fields.value(QStringLiteral("height")).toDouble();
            if (fields.value(QStringLiteral("live")).toBool())
                presenter_.previewCropRect(x, y, width, height);
            else
                presenter_.setCropRect(x, y, width, height);
        });
    add(command::kEditSetCropAspect, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.setCropAspect(argument.toString()); });
    add(
        command::kEditAutoPerspective, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const QString mode = argument.toString();
            return mode == QLatin1String("vertical") || mode == QLatin1String("horizontal") ||
                           mode == QLatin1String("full") ?
                       QString{} :
                       QStringLiteral("Perspective mode must be vertical, horizontal, or full.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.autoPerspective(argument.toString()); });
    add(command::kEditRotateLeft, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rotateLeft(); });
    add(command::kEditRotateRight, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rotateRight(); });
    add(command::kEditFlipHorizontal, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.flipHorizontal(); });
    add(command::kEditFlipVertical, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.flipVertical(); });
    add(
        command::kEditCropTool, Condition::kSelection,
        [](const QVariant &argument)
        {
            return !argument.isValid() || argument.isNull() || argument.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("Crop state must be boolean.");
        },
        [this](const QVariant &argument, const QString &source)
        {
            presenter_.openDevelop();
            if (argument.isValid())
            {
                presenter_.setCropToolActive(argument.toBool());
                return;
            }
            // R always enters crop. Menu/button still toggle so Done can exit.
            presenter_.setCropToolActive(
                source == QLatin1String("keyboard") ? true : !presenter_.cropToolActive());
        });
    add(command::kEditBeforeAfter, Condition::kDevelop, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleBeforeAfter(); });
    add(command::kEditComparison, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleComparison(); });

    add(command::kWindowSettings, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowSettings, argument); });
    add(command::kWindowAssistant, Condition::kAlways, no_argument,
        [this](const QVariant &, const QString &) { setAssistantOpen(!assistant_open_); });
    add(command::kWindowClose, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowClose, argument); });
    add(command::kWindowQuit, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowQuit, argument); });
    add(command::kWindowAbout, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowAbout, argument); });
    add(command::kWindowPalette, Condition::kAlways, no_argument,
        [this](const QVariant &, const QString &) { setPaletteOpen(true); });
    add(command::kWindowDismiss, Condition::kAlways, no_argument,
        [this, present](const QVariant &argument, const QString &)
        {
            if (settings_open_)
                present(command::kWindowDismiss, argument);
            else if (assistant_open_)
                setAssistantOpen(false);
            else if (presenter_.catalogOpen())
                presenter_.returnToGrid();
        });

    const auto errors = validateBuiltinDefinitions();
    if (!errors.isEmpty())
        qFatal("Studio command registry validation failed: %s",
               errors.join(QLatin1String("; ")).toUtf8().constData());
    if (string_set(impl_->commands.keys()) != string_set(command_ids()))
        qFatal("Studio command handlers do not cover the builtin command registry");

    const auto changed = [this]() { refresh(); };
    connect(&presenter_, &StudioPresenter::catalogChanged, this, changed);
    connect(&presenter_, &StudioPresenter::busyChanged, this, changed);
    connect(&presenter_, &StudioPresenter::libraryWorkChanged, this, changed);
    connect(&presenter_, &StudioPresenter::selectionChanged, this, changed);
    connect(&presenter_, &StudioPresenter::browseModeChanged, this, changed);
    connect(&presenter_, &StudioPresenter::zoomChanged, this, changed);
    connect(&presenter_, &StudioPresenter::editChanged, this, changed);
    connect(&presenter_, &StudioPresenter::copiedParametersChanged, this, changed);
}

StudioCommandController::~StudioCommandController() = default;

namespace
{
State resolve_state(const StudioPresenter &presenter, const Condition condition,
                    const bool settings_open)
{
    if (settings_open && condition != Condition::kAlways)
        return {false, tr_command(QStringLiteral("Close Settings to use this command."))};
    const bool catalog_open = presenter.catalogOpen();
    const bool selection = !presenter.selectedAssetId().isEmpty();
    const bool ready = catalog_open && !presenter.busy() && !presenter.importWorkActive();
    switch (condition)
    {
    case Condition::kAlways:
        return {};
    case Condition::kCatalogOpen:
        return catalog_open ? State{} :
                              State{false, tr_command(QStringLiteral("Open a library first."))};
    case Condition::kCatalogReady:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return ready ? State{} :
                       State{false, tr_command(QStringLiteral("Wait for library work to finish."))};
    case Condition::kSelection:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return selection ? State{} :
                           State{false, tr_command(QStringLiteral("Select a photo first."))};
    case Condition::kReadySelection:
        if (!ready)
            return {false, !catalog_open ?
                               tr_command(QStringLiteral("Open a library first.")) :
                               tr_command(QStringLiteral("Wait for library work to finish."))};
        return selection ? State{} :
                           State{false, tr_command(QStringLiteral("Select a photo first."))};
    case Condition::kNonGrid:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return presenter.browseMode() != QLatin1String("grid") &&
                       presenter.browseMode() != QLatin1String("survey") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open a photo first."))};
    case Condition::kSurveySelection:
        if (!ready)
            return {false, !catalog_open ?
                               tr_command(QStringLiteral("Open a library first.")) :
                               tr_command(QStringLiteral("Wait for library work to finish."))};
        return presenter.selectedCount() >= 2 ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Select two or four photos first."))};
    case Condition::kDevelop:
        return presenter.browseMode() == QLatin1String("develop") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open Edit first."))};
    case Condition::kDevelopSelection:
        if (!selection)
            return {false, tr_command(QStringLiteral("Select a photo first."))};
        return presenter.browseMode() == QLatin1String("develop") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open Edit first."))};
    case Condition::kModifiedParameters:
        if (!selection)
            return {false, tr_command(QStringLiteral("Select a photo first."))};
        return presenter.modifiedParameterChoices().isEmpty() ?
                   State{false, tr_command(QStringLiteral("No modified parameters to copy."))} :
                   State{};
    case Condition::kCanUndo:
        return presenter.browseMode() == QLatin1String("develop") && presenter.canUndo() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Nothing to undo."))};
    case Condition::kCanRedo:
        return presenter.browseMode() == QLatin1String("develop") && presenter.canRedo() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Nothing to redo."))};
    case Condition::kCanPasteParameters:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        if (!selection)
            return {false, tr_command(QStringLiteral("Select a photo first."))};
        return presenter.hasCopiedParameters() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Copy parameters first."))};
    case Condition::kCanPasteParametersToSelection:
        if (!ready)
            return {false, !catalog_open ?
                               tr_command(QStringLiteral("Open a library first.")) :
                               tr_command(QStringLiteral("Wait for library work to finish."))};
        if (presenter.catalogOperationActive())
            return {false, tr_command(QStringLiteral("Wait for library work to finish."))};
        if (presenter.selectedCount() < 2)
            return {false, tr_command(QStringLiteral("Select at least two photos first."))};
        return presenter.hasCopiedParameters() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Copy parameters first."))};
    case Condition::kCanDelete:
        return presenter.canDeleteFromDisk() ?
                   State{} :
                   State{false,
                         tr_command(QStringLiteral("The selected originals cannot be deleted."))};
    case Condition::kCatalogOperation:
        return presenter.catalogOperationActive() || presenter.importWorkActive() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("No catalog operation is running."))};
    }
    return {false, tr_command(QStringLiteral("Command unavailable in the current context."))};
}
} // namespace

QVariantMap StudioCommandController::action(const QString &action_id) const
{
    const auto found = std::find_if(impl_->actions.cbegin(), impl_->actions.cend(),
                                    [&action_id](const ActionSpec &candidate)
                                    { return candidate.id == action_id; });
    if (found == impl_->actions.cend())
        return {};
    const auto command_found = impl_->commands.constFind(found->command_id);
    if (command_found == impl_->commands.cend())
        return {};
    const auto state = resolve_state(presenter_, command_found->condition, settings_open_);
    QString title = tr_command(found->title);
    if (found->id == QLatin1String(command::kPhotoToggleReject) && presenter_.selectedRejected())
        title = tr_command(QStringLiteral("Unreject"));
    if (found->id == QLatin1String(command::kEditCropTool) && presenter_.cropToolActive())
        title = tr_command(QStringLiteral("Done Cropping"));

    bool checkable = false;
    bool checked = false;
    if (found->id == QLatin1String(command::kViewGrid) ||
        found->id == QLatin1String(command::kViewLoupe) ||
        found->id == QLatin1String(command::kViewDevelop) ||
        found->id == QLatin1String(command::kViewSurvey))
    {
        checkable = true;
        checked = (found->id == QLatin1String(command::kViewGrid) &&
                   presenter_.browseMode() == QLatin1String("grid")) ||
                  (found->id == QLatin1String(command::kViewLoupe) &&
                   presenter_.browseMode() == QLatin1String("loupe")) ||
                  (found->id == QLatin1String(command::kViewDevelop) &&
                   presenter_.browseMode() == QLatin1String("develop")) ||
                  (found->id == QLatin1String(command::kViewSurvey) &&
                   presenter_.browseMode() == QLatin1String("survey"));
    }
    else if (found->id == QLatin1String(command::kLibraryToggleStackCollapse))
    {
        checkable = true;
        checked = presenter_.collapseStacks();
    }
    else if (found->id == QLatin1String(command::kViewFit) ||
             found->id == QLatin1String(command::kViewFill) ||
             found->id == QLatin1String(command::kViewActual))
    {
        checkable = true;
        checked = (found->id == QLatin1String(command::kViewFit) &&
                   presenter_.zoomMode() == QLatin1String("fit")) ||
                  (found->id == QLatin1String(command::kViewFill) &&
                   presenter_.zoomMode() == QLatin1String("fill")) ||
                  (found->id == QLatin1String(command::kViewActual) &&
                   presenter_.zoomMode() == QLatin1String("actual"));
    }
    else if (found->command_id == QLatin1String(command::kPhotoSetRating))
    {
        checkable = true;
        checked = found->argument.toInt() == presenter_.selectedRating();
    }
    else if (found->command_id == QLatin1String(command::kPhotoSetColor))
    {
        checkable = true;
        checked = found->argument.toString() == presenter_.selectedColorLabel();
    }
    else if (found->id == QLatin1String(command::kPhotoToggleReject))
    {
        checkable = true;
        checked = presenter_.selectedRejected();
    }
    else if (found->id == QLatin1String(command::kEditCropTool))
    {
        checkable = true;
        checked = presenter_.cropToolActive();
    }
    else if (found->id == QLatin1String(command::kEditBeforeAfter))
    {
        checkable = true;
        checked = presenter_.beforeAfter();
    }
    else if (found->id == QLatin1String(command::kEditComparison))
    {
        checkable = true;
        checked = presenter_.comparisonActive();
    }
    else if (found->id == QLatin1String(command::kWindowAssistant))
    {
        checkable = true;
        checked = assistant_open_;
    }
    else if (found->id == QLatin1String(command::kViewTogglePhotoInfo))
    {
        checkable = true;
        checked = photo_info_visible_;
    }
    const QString shortcut =
        found->shortcuts.isEmpty() ? QString{} : native_key(found->shortcuts.front().sequence);
    return {{QStringLiteral("actionId"), found->id},
            {QStringLiteral("commandId"), found->command_id},
            {QStringLiteral("title"), title},
            {QStringLiteral("category"), tr_command(found->category)},
            {QStringLiteral("keywords"), found->keywords},
            {QStringLiteral("menuPath"), found->menu_path},
            {QStringLiteral("enabled"), state.enabled},
            {QStringLiteral("disabledReason"), state.reason},
            {QStringLiteral("checkable"), checkable},
            {QStringLiteral("checked"), checked},
            {QStringLiteral("executing"), false},
            {QStringLiteral("shortcutText"), shortcut}};
}

QVariantList StudioCommandController::menuEntries(const QString &path) const
{
    QVector<const ActionSpec *> selected;
    for (const auto &definition : impl_->actions)
        if (definition.menu_path == path)
            selected.push_back(&definition);
    std::sort(selected.begin(), selected.end(),
              [](const auto *left, const auto *right)
              {
                  return left->order < right->order ||
                         (left->order == right->order && left->id < right->id);
              });
    QVariantList result;
    for (const auto *definition : selected)
        result.push_back(action(definition->id));
    return result;
}

QVariantList StudioCommandController::paletteEntries() const
{
    struct Ranked
    {
        QVariantMap value;
        int score = 0;
        int order = 0;
        QString id;
    };
    QVector<Ranked> ranked;
    for (const auto &definition : impl_->actions)
    {
        if (!definition.palette_visible)
            continue;
        const auto value = action(definition.id);
        const int score = fuzzyScore(value.value(QStringLiteral("title")).toString(),
                                     value.value(QStringLiteral("category")).toString(),
                                     definition.keywords, definition.id, palette_query_);
        if (score >= 0)
            ranked.push_back({value, score, definition.order, definition.id});
    }
    std::sort(ranked.begin(), ranked.end(),
              [this](const Ranked &left, const Ranked &right)
              {
                  if (!palette_query_.trimmed().isEmpty() && left.score != right.score)
                      return left.score > right.score;
                  const auto left_category =
                      left.value.value(QStringLiteral("category")).toString();
                  const auto right_category =
                      right.value.value(QStringLiteral("category")).toString();
                  if (left_category != right_category)
                      return left_category.localeAwareCompare(right_category) < 0;
                  if (left.order != right.order)
                      return left.order < right.order;
                  const auto left_title = left.value.value(QStringLiteral("title")).toString();
                  const auto right_title = right.value.value(QStringLiteral("title")).toString();
                  if (left_title != right_title)
                      return left_title.localeAwareCompare(right_title) < 0;
                  return left.id < right.id;
              });
    QVariantList result;
    for (const auto &entry : ranked)
        result.push_back(entry.value);
    return result;
}

QVariantList StudioCommandController::shortcutEntries() const
{
    QVariantList result;
    for (const auto &definition : impl_->actions)
    {
        const auto state = action(definition.id);
        for (const auto &shortcut : definition.shortcuts)
        {
            const bool enabled = state.value(QStringLiteral("enabled")).toBool() &&
                                 !palette_open_ && !modal_open_ &&
                                 (!shortcut.requires_non_text_input || !text_input_active_);
            result.push_back(
                QVariantMap{{QStringLiteral("actionId"), definition.id},
                            {QStringLiteral("sequence"), shortcut.sequence},
                            {QStringLiteral("nativeText"), native_key(shortcut.sequence)},
                            {QStringLiteral("enabled"), enabled}});
        }
    }
    return result;
}

QVariantMap StudioCommandController::executeAction(const QString &action_id, const QString &source)
{
    const auto found = std::find_if(impl_->actions.cbegin(), impl_->actions.cend(),
                                    [&action_id](const ActionSpec &candidate)
                                    { return candidate.id == action_id; });
    if (found == impl_->actions.cend())
    {
        const auto message = tr_command(QStringLiteral("Unknown action: %1")).arg(action_id);
        presenter_.setError(message);
        emit dispatchRejected(action_id, QStringLiteral("unknown_action"), message);
        return rejected(QStringLiteral("unknown_action"), message);
    }
    const auto state = action(action_id);
    if (!state.value(QStringLiteral("enabled")).toBool())
    {
        const auto message = state.value(QStringLiteral("disabledReason")).toString();
        emit dispatchRejected(action_id, QStringLiteral("unavailable"), message);
        return rejected(QStringLiteral("unavailable"), message);
    }
    return executeCommandInternal(found->command_id,
                                  found->has_argument ? found->argument : QVariant{}, source);
}

QVariantMap StudioCommandController::executeCommand(const QString &command_id,
                                                    const QVariant &argument, const QString &source)
{
    return executeCommandInternal(command_id, argument, source);
}

Result<bool>
StudioCommandController::applyDevelopFields(const std::vector<StudioDevelopField> &fields)
{
    if (fields.empty() || fields.size() > 256U)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Live Develop mutation requires 1 through 256 fields");
    }
    if (modal_open_)
    {
        return make_error(ErrorCode::kConflict,
                          "Studio command is unavailable while a modal surface is open",
                          {{"reason", "command_unavailable"}});
    }
    const bool enter_develop = presenter_.browseMode() != QLatin1String("develop");
    const auto state = resolve_state(
        presenter_, enter_develop ? Condition::kReadySelection : Condition::kDevelopSelection,
        settings_open_);
    if (!state.enabled)
    {
        return make_error(ErrorCode::kConflict, state.reason.toUtf8().toStdString(),
                          {{"reason", "command_unavailable"}});
    }
    if (!presenter_.develop_loaded_ || presenter_.busy_ || presenter_.import_work_active_ ||
        presenter_.develop_job_in_flight_ || presenter_.pending_save_ ||
        presenter_.pending_preview_)
    {
        return make_error(ErrorCode::kConflict, "Studio Develop state is busy",
                          {{"reason", "busy"}});
    }

    DevelopParams next = presenter_.develop_;
    std::set<std::string, std::less<>> names;
    for (const auto &field : fields)
    {
        if (field.name.empty() || field.name.size() > 256U || !std::isfinite(field.value))
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Live Develop field name or value is invalid",
                              {{"name", field.name}});
        }
        if (!names.insert(field.name).second)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Live Develop field was specified more than once",
                              {{"name", field.name}});
        }
        auto applied = apply_develop_field_strict(next, field.name, field.value);
        if (!applied)
        {
            auto error = applied.error();
            error.context.insert_or_assign("reason", "invalid_develop_field");
            return error;
        }
    }
    if (enter_develop)
    {
        presenter_.openDevelop();
    }
    return presenter_.mutate_develop(std::move(next), StudioPresenter::DevelopEdit::Commit);
}

void StudioCommandController::cancelPendingConfirmation(const QString &token)
{
    if (token.isEmpty() || impl_->pending_confirmation_token != token)
        return;
    impl_->pending_confirmation_command.clear();
    impl_->pending_confirmation_token.clear();
    impl_->pending_confirmation_assets.clear();
    impl_->pending_confirmation_argument.clear();
}

QVariantMap StudioCommandController::executeCommandInternal(const QString &command_id,
                                                            const QVariant &argument,
                                                            const QString &source)
{
    static const QSet<QString> sources{QStringLiteral("menu"), QStringLiteral("keyboard"),
                                       QStringLiteral("palette"), QStringLiteral("control"),
                                       QStringLiteral("system")};
    const auto found = impl_->commands.constFind(command_id);
    if (found == impl_->commands.cend())
    {
        const auto message = tr_command(QStringLiteral("Unknown command: %1")).arg(command_id);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("unknown_command"), message);
        return rejected(QStringLiteral("unknown_command"), message);
    }
    if (!sources.contains(source))
    {
        const auto message = tr_command(QStringLiteral("Unknown command source: %1")).arg(source);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("invalid_source"), message);
        return rejected(QStringLiteral("invalid_source"), message);
    }
    const auto state = resolve_state(presenter_, found->condition, settings_open_);
    if (!state.enabled)
    {
        emit dispatchRejected(command_id, QStringLiteral("unavailable"), state.reason);
        return rejected(QStringLiteral("unavailable"), state.reason);
    }
    const auto validation_error = found->validator ? found->validator(argument) : QString{};
    if (!validation_error.isEmpty())
    {
        const auto message = tr_command(validation_error);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("invalid_argument"), message);
        return rejected(QStringLiteral("invalid_argument"), message);
    }
    found->handler(argument, source);
    return accepted();
}

} // namespace ravo
