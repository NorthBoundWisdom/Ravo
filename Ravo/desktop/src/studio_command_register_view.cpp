#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_registration.h"
#include "studio_command_ids.h"
#include "studio_command_controller_detail.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
using namespace command_controller_detail;
using command_internal::tr_command;

void StudioCommandController::registerViewCommands(const command_registration::Helpers &helpers)
{
    const auto &add = helpers.add;
    const auto &present = helpers.present;
    const auto &request_confirmation = helpers.request_confirmation;
    const auto &confirmation_validator = helpers.confirmation_validator;
    const auto &request_preset_confirmation = helpers.request_preset_confirmation;
    const auto &preset_confirmation_validator = helpers.preset_confirmation_validator;
    const auto &clear_confirmation = helpers.clear_confirmation;

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
    add(command::kPhotoSelectAll, Condition::kLoadedPhotos, no_argument,
        [this](const QVariant &, const QString &)
        {
            if (presenter_.importPageOpen())
                presenter_.importCandidates()->highlightAll();
            else
                presenter_.selectAllVisible();
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
            static const QSet<QString> names{
                QStringLiteral("title"),   QStringLiteral("description"),
                QStringLiteral("creator"), QStringLiteral("copyright"),
                QStringLiteral("country"), QStringLiteral("province_state"),
                QStringLiteral("city"),    QStringLiteral("sublocation")};
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
    add(command::kPhotoTogglePick, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.togglePicked(); });
    add(command::kPhotoToggleReject, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleRejected(); });
    add(command::kPhotoCullUnflag, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.applyCullReview(QStringLiteral("unflag"), QVariant{}, QString{}, true); });
    add(command::kPhotoCopyInfo, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.copySelectedPhotoDebugInfo(); });
    add(command::kPhotoCopyParameters, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.copySelectedPhotoParametersDebugInfo(); });
    add(command::kPhotoRevealInFileManager, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.revealSelectedPhotoInFileManager(); });
    add(command::kPhotoEditIn, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPhotoEditIn, argument); });
    add(
        command::kPhotoEditInPrepare, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("editorId")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "External editor id must not be empty.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.prepareExternalEditorWorkingCopy(argument.toMap()); });
    add(
        command::kPhotoEditInCheckReturned, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.checkExternalEditorReturned(
                fields.value(QStringLiteral("workingCopyId")).toString(),
                fields.value(QStringLiteral("returnedPath")).toString());
        });
    add(command::kPhotoEditInClearSession, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.clearExternalEditorSession(); });
    add(
        command::kPhotoEditInAbandon, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.abandonExternalEditorWorkingCopy(
                fields.value(QStringLiteral("workingCopyId")).toString());
        });
    add(
        command::kPhotoEditInReopen, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const bool open_after = fields.value(QStringLiteral("openAfterReopen"), true).toBool();
            presenter_.reopenExternalEditorWorkingCopy(
                fields.value(QStringLiteral("workingCopyId")).toString(), open_after,
                fields.value(QStringLiteral("applicationPath")).toString());
        });
    add(
        command::kPhotoEditInRefreshStatus, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.refreshExternalEditorWorkingCopyStatus(
                fields.value(QStringLiteral("workingCopyId")).toString());
        });
    add(command::kPhotoOfflineEdit, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPhotoOfflineEdit, argument); });
    add(command::kPhotoOfflineEditRefreshStatus, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshOfflineEditMediaStatus(); });
    add(command::kPhotoOfflineEditList, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshOfflineEditProxyList(); });
    add(
        command::kPhotoOfflineEditCreate, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.createOfflineEditProxy(fields.value(QStringLiteral("maxEdge")).toUInt());
        });
    add(
        command::kPhotoOfflineEditReconnect, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.reconnectOfflineEditProxy(
                fields.value(QStringLiteral("clearProxy")).toBool());
        });
    add(
        command::kPhotoOfflineEditDelete, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.deleteOfflineEditProxy(fields.value(QStringLiteral("force")).toBool());
        });
    add(
        command::kPhotoOfflineEditPin, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const bool pinned = fields.contains(QStringLiteral("pinned")) ?
                                    fields.value(QStringLiteral("pinned")).toBool() :
                                    true;
            presenter_.pinOfflineEditProxy(pinned);
        });
    add(
        command::kPhotoOfflineEditEvict, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.evictOfflineEditProxies(
                fields.value(QStringLiteral("maxTotalBytes")).toULongLong());
        });

    add(command::kPhotoAiProposal, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPhotoAiProposal, argument); });
    add(
        command::kPhotoAiPropose, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::UnknownType)
                return QString{};
            if (argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.createAiStubProposal(
                fields.value(QStringLiteral("kind")).toString(),
                fields.value(QStringLiteral("semanticLabel")).toString());
        });
    add(
        command::kPhotoAiProposalSelect, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required.")));
            if (argument.toMap().value(QStringLiteral("proposalId")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "A proposal id is required.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            presenter_.selectAiProposal(
                argument.toMap().value(QStringLiteral("proposalId")).toString());
        });
    add(command::kPhotoAiProposalRefresh, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshAiProposals(); });
    add(command::kPhotoAiProposalApply, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.applySelectedAiProposal(); });
    add(command::kPhotoAiProposalReject, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rejectSelectedAiProposal(); });
    add(command::kPhotoAiProposalCancel, Condition::kReadySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.cancelSelectedAiProposal(); });
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
    add(command::kViewSurvey, Condition::kSurveySelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openSurvey(); });
    add(command::kViewBurstCompare, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openBurstCompare(); });
    add(command::kViewBurstComparePrevious, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.stepBurstComparePrevious(); });
    add(command::kViewBurstCompareNext, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.stepBurstCompareNext(); });
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
    // clang-format off
    const auto validate_preview_xy =
        [](const QVariant &argument, const QString &x_label, const QString &y_label) -> QString {
        if (const auto e = required_fields(argument, {QStringLiteral("x"), QStringLiteral("y")}); !e.isEmpty())
            return e;
        const auto fields = argument.toMap();
        if (const auto e = finite_number(fields.value(QStringLiteral("x")), x_label); !e.isEmpty())
            return e;
        return finite_number(fields.value(QStringLiteral("y")), y_label);
    };
    const auto validate_bool = [](const QVariant &argument, const QString &label) -> QString {
        return (argument.metaType().id() == QMetaType::Bool || argument.canConvert<bool>()) ? QString{} : label;
    };
    add(command::kEditPickWhiteBalance, Condition::kDevelopSelection,
        [&validate_preview_xy](const QVariant &a) {
            return validate_preview_xy(a, QStringLiteral("White-balance X"), QStringLiteral("White-balance Y"));
        },
        [this](const QVariant &a, const QString &) {
            const auto fields = a.toMap();
            presenter_.pickWhiteBalance(fields.value(QStringLiteral("x")).toDouble(),
                                        fields.value(QStringLiteral("y")).toDouble());
        });
    add(command::kEditSetWhiteBalancePick, Condition::kDevelopSelection,
        [&validate_bool](const QVariant &a) {
            return validate_bool(a, QStringLiteral("White-balance pick state must be boolean."));
        },
        [this](const QVariant &a, const QString &) { presenter_.setWhiteBalancePickActive(a.toBool()); });
    add(command::kEditPlaceMask, Condition::kDevelopSelection,
        [&validate_preview_xy](const QVariant &a) {
            return validate_preview_xy(a, QStringLiteral("Mask place X"), QStringLiteral("Mask place Y"));
        },
        [this](const QVariant &a, const QString &) {
            const auto fields = a.toMap();
            presenter_.placeMask(fields.value(QStringLiteral("x")).toDouble(),
                                 fields.value(QStringLiteral("y")).toDouble());
        });
    add(command::kEditSetMaskPlace, Condition::kDevelopSelection,
        [&validate_bool](const QVariant &a) {
            return validate_bool(a, QStringLiteral("Mask place state must be boolean."));
        },
        [this](const QVariant &a, const QString &) { presenter_.setMaskPlaceActive(a.toBool()); });
    add(command::kEditAssistParametricMask, Condition::kDevelopSelection,
        [&validate_preview_xy](const QVariant &a) {
            return validate_preview_xy(a, QStringLiteral("Parametric assist X"),
                                       QStringLiteral("Parametric assist Y"));
        },
        [this](const QVariant &a, const QString &) {
            const auto fields = a.toMap();
            presenter_.assistParametricMask(fields.value(QStringLiteral("x")).toDouble(),
                                            fields.value(QStringLiteral("y")).toDouble());
        });
    add(command::kEditSetMaskParametricAssist, Condition::kDevelopSelection,
        [&validate_bool](const QVariant &a) {
            return validate_bool(a, QStringLiteral("Mask parametric assist state must be boolean."));
        },
        [this](const QVariant &a, const QString &) {
            presenter_.setMaskParametricAssistActive(a.toBool());
        });
    // clang-format on
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
            else if (presenter_.importPageOpen())
                presenter_.closeImportPage();
            else if (presenter_.catalogOpen())
                presenter_.returnToGrid();
        });
}

} // namespace ravo
