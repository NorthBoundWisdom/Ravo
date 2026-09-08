#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerImportCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_confirmation = helpers.request_confirmation;
    const auto &confirmation_validator = helpers.confirmation_validator;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;

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
    add(command::kPresetImport, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetImport, argument); });
    add(command::kPresetImportPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.importPresetFromPath(argument.toString()); });
    add(command::kLibrarySelectLastImport, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.selectLastImport(); });
}

} // namespace ravo
