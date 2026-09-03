#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_controller_detail.h"
#include "studio_command_ids.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include <QKeySequence>
#include <QSet>
#include <QVector>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/develop.h"
#include "studio_qt.h"

namespace ravo
{
using namespace command_internal;
using namespace command_controller_detail;

namespace command_controller_detail
{
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
} // namespace command_controller_detail

namespace command_controller_detail
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
    case Condition::kLoadedPhotos:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return presenter.visibleCount() > 0 ?
                   State{} :
                   State{false, tr_command(QStringLiteral("No photos to select."))};
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
} // namespace command_controller_detail

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
