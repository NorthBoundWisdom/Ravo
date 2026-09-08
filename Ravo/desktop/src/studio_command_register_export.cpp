#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerExportCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_confirmation = helpers.request_confirmation;
    const auto &confirmation_validator = helpers.confirmation_validator;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;

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
}

} // namespace ravo
