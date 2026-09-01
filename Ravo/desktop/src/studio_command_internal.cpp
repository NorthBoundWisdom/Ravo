#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_internal.h"

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

namespace ravo::command_internal
{

QString tr_command(const QString &source)
{
    const auto bytes = source.toUtf8();
    return QCoreApplication::translate("StudioCommands", bytes.constData());
}

[[maybe_unused]] const char *const kStudioCommandTranslationSources[] = {
    QT_TRANSLATE_NOOP("StudioCommands", "This command takes no argument."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-empty string is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-empty list of paths is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Every import path must be a non-empty string."),
    QT_TRANSLATE_NOOP("StudioCommands", "%1 must be a finite number."),
    QT_TRANSLATE_NOOP("StudioCommands", "Missing command argument field: %1."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown %1 value."),
    QT_TRANSLATE_NOOP("StudioCommands", "A current confirmation token is required."),
    QT_TRANSLATE_NOOP("StudioCommands",
                      "The photo selection changed after confirmation was requested."),
    QT_TRANSLATE_NOOP("StudioCommands", "Preset path and name must be non-empty strings."),
    QT_TRANSLATE_NOOP("StudioCommands", "The preset changed after confirmation was requested."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export path must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export filename template must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown rating filter mode."),
    QT_TRANSLATE_NOOP("StudioCommands", "Rating filter value must be an integer from 0 to 5."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown library sort field."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown library sort direction."),
    QT_TRANSLATE_NOOP("StudioCommands", "An asset ID is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Rating must be an integer between 0 and 5."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown color label."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown writable metadata field."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-negative integer history ID is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Snapshot label must be a string."),
    QT_TRANSLATE_NOOP("StudioCommands", "Navigation argument must be 'range'."),
    QT_TRANSLATE_NOOP("StudioCommands", "Thumbnail size must be an integer between 120 and 320."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop control name must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop fields must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "White-balance pick state must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop section name must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop section enabled must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop value must be finite."),
    QT_TRANSLATE_NOOP("StudioCommands", "Tone curve points must be a list."),
    QT_TRANSLATE_NOOP("StudioCommands", "Crop width and height must be positive."),
    QT_TRANSLATE_NOOP("StudioCommands", "Crop state must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Close Settings to use this command."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open a library first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Wait for library work to finish."),
    QT_TRANSLATE_NOOP("StudioCommands", "Select a photo first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open a photo first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open Edit first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Nothing to undo."),
    QT_TRANSLATE_NOOP("StudioCommands", "Nothing to redo."),
    QT_TRANSLATE_NOOP("StudioCommands", "No modified parameters to copy."),
    QT_TRANSLATE_NOOP("StudioCommands", "Copy parameters first."),
    QT_TRANSLATE_NOOP("StudioCommands", "The selected originals cannot be deleted."),
    QT_TRANSLATE_NOOP("StudioCommands", "No catalog operation is running."),
    QT_TRANSLATE_NOOP("StudioCommands", "Command unavailable in the current context."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unreject"),
    QT_TRANSLATE_NOOP("StudioCommands", "Done Cropping"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown action: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command source: %1")};

QString normalize_search(const QString &value)
{
    const auto decomposed = value.normalized(QString::NormalizationForm_KD).toCaseFolded();
    QString result;
    result.reserve(decomposed.size());
    for (const auto character : decomposed)
    {
        const auto category = character.category();
        if (category != QChar::Mark_NonSpacing && category != QChar::Mark_SpacingCombining &&
            category != QChar::Mark_Enclosing)
            result.push_back(character);
    }
    return result;
}

int token_score(const QString &source, const QString &raw_token)
{
    const auto haystack = normalize_search(source);
    const auto token = normalize_search(raw_token);
    if (token.isEmpty())
        return 0;
    if (haystack == token)
        return 4000;
    for (qsizetype index = 0; index + token.size() <= haystack.size(); ++index)
    {
        const bool boundary = index == 0 || !haystack.at(index - 1).isLetterOrNumber();
        if (boundary && haystack.mid(index, token.size()) == token)
            return 3000 - static_cast<int>(index);
    }
    const qsizetype contiguous = haystack.indexOf(token);
    if (contiguous >= 0)
        return 2000 - static_cast<int>(contiguous);
    qsizetype cursor = 0;
    qsizetype first = -1;
    qsizetype last = -1;
    for (const auto character : token)
    {
        const qsizetype found = haystack.indexOf(character, cursor);
        if (found < 0)
            return -1;
        if (first < 0)
            first = found;
        last = found;
        cursor = found + 1;
    }
    const qsizetype gaps = std::max<qsizetype>(0, last - first + 1 - token.size());
    return 1000 - static_cast<int>(first) - static_cast<int>(gaps) * 4;
}


} // namespace ravo::command_internal
