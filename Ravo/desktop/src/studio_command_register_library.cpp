#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerLibraryCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_confirmation = helpers.request_confirmation;
    const auto &confirmation_validator = helpers.confirmation_validator;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;

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
    add(command::kLibraryPreviewRebuildSelected, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rebuildSelectedPreviews(); });
    add(command::kLibraryPreviewRebuildAll, Condition::kCatalogReady, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rebuildAllPreviews(); });
    add(command::kLibraryCancelOperation, Condition::kCatalogOperation, no_argument,
        [this](const QVariant &, const QString &) { presenter_.cancelCatalogOperation(); });
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
        command::kLibrarySetCameraFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("make"), QStringLiteral("model")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            return fields.value(QStringLiteral("make")).metaType().id() == QMetaType::QString &&
                           fields.value(QStringLiteral("model")).metaType().id() ==
                               QMetaType::QString ?
                       QString{} :
                       QStringLiteral("Camera facet make and model must be strings.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setCameraFacetFilter(fields.value(QStringLiteral("make")).toString(),
                                            fields.value(QStringLiteral("model")).toString());
        });
    add(
        command::kLibrarySetLensFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::QString ?
                       QString{} :
                       QStringLiteral("Lens facet must be a string focal length.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setLensFacetFilter(argument.toString()); });
    add(
        command::kLibrarySetLensNameFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("make"), QStringLiteral("model")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            for (const auto &key : {QStringLiteral("make"), QStringLiteral("model")})
            {
                if (fields.value(key).metaType().id() != QMetaType::QString)
                    return QStringLiteral("Lens-name facet make and model must be strings.");
            }
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setLensNameFacetFilter(fields.value(QStringLiteral("make")).toString(),
                                              fields.value(QStringLiteral("model")).toString());
        });
    add(
        command::kLibrarySetCaptureDateFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::QString ?
                       QString{} :
                       QStringLiteral("Capture-date facet must be a YYYY:MM:DD string.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setCaptureDateFacetFilter(argument.toString()); });
    add(
        command::kLibrarySetLocationFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error = required_fields(
                argument, {QStringLiteral("country"), QStringLiteral("province_state"),
                           QStringLiteral("city"), QStringLiteral("sublocation")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            for (const auto &key : {QStringLiteral("country"), QStringLiteral("province_state"),
                                    QStringLiteral("city"), QStringLiteral("sublocation")})
            {
                if (fields.value(key).metaType().id() != QMetaType::QString)
                    return QStringLiteral("Location facet fields must be strings.");
            }
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setLocationFacetFilter(
                fields.value(QStringLiteral("country")).toString(),
                fields.value(QStringLiteral("province_state")).toString(),
                fields.value(QStringLiteral("city")).toString(),
                fields.value(QStringLiteral("sublocation")).toString());
        });
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
    add(command::kLibraryRevealFolder, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.revealFolderInFileManager(argument.toString()); });
    add(command::kLibraryRequestRemoveFolder, Condition::kCatalogReady, non_empty_string,
        [request_preset_confirmation](const QVariant &argument, const QString &)
        {
            request_preset_confirmation(command::kLibraryRequestRemoveFolder,
                                        command::kLibraryRemoveFolder,
                                        QVariantMap{{QStringLiteral("path"), argument.toString()}});
        });
    add(
        command::kLibraryRemoveFolder, Condition::kCatalogReady,
        [preset_confirmation_validator](const QVariant &argument)
        { return preset_confirmation_validator(command::kLibraryRemoveFolder, argument); },
        [this, clear_confirmation](const QVariant &argument, const QString &)
        {
            clear_confirmation();
            presenter_.removeFolderFromCatalog(
                argument.toMap().value(QStringLiteral("path")).toString());
        });
}

} // namespace ravo
