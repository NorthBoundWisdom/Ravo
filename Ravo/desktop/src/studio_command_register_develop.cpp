#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerDevelopCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_confirmation = helpers.request_confirmation;
    const auto &confirmation_validator = helpers.confirmation_validator;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;

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
    add(command::kViewDevelop, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openDevelop(); });
}

} // namespace ravo
