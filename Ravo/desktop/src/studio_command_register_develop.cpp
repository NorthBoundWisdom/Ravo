#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"
#include <cmath>

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerDevelopCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;
    add(command::kVideoPlay, Condition::kVideo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.video()->play(); });
    add(command::kVideoPause, Condition::kVideo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.video()->pause(); });
    add(
        command::kVideoSeek, Condition::kVideo,
        [this](const QVariant &argument)
        {
            const auto valid = finite_number(argument, "position_ms");
            const double value = argument.toDouble();
            if (!valid.isEmpty())
                return valid;
            return value >= 0 && value <= static_cast<double>(presenter_.video()->duration()) &&
                           std::floor(value) == value ?
                       QString{} :
                       QStringLiteral("Video position is outside the duration.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.video()->seek(argument.toLongLong()); });
    add(
        command::kVideoVolume, Condition::kVideo,
        [](const QVariant &argument)
        {
            const auto valid = finite_number(argument, "volume");
            if (!valid.isEmpty())
                return valid;
            return argument.toDouble() >= 0 && argument.toDouble() <= 1 ?
                       QString{} :
                       QStringLiteral("Video volume must be between 0 and 1.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.video()->setVolume(argument.toDouble()); });
    add(
        command::kVideoMute, Condition::kVideo,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::Bool ?
                       QString{} :
                       QStringLiteral("Video mute state must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.video()->setMuted(argument.toBool()); });

    add(command::kStyleSave, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleSave, argument); });
    add(command::kStyleSavePath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.develop()->saveStyleToPath(argument.toString()); });
    add(command::kStyleApply, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleApply, argument); });
    add(command::kStyleApplyPath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.develop()->applyStyleFromPath(argument.toString()); });
    add(command::kPresetApplyPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.develop()->applyStyleFromPath(argument.toString()); });
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
            presenter_.develop()->savePreset(values.value(QStringLiteral("name")).toString(),
                                             fields);
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
            presenter_.develop()->renamePreset(fields.value(QStringLiteral("path")).toString(),
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
            presenter_.develop()->deletePreset(path);
        });
    add(command::kViewDevelop, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openDevelop(); });
}

} // namespace ravo
