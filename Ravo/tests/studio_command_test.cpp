#include <gtest/gtest.h>

#include <QKeySequence>
#include <QCoreApplication>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-command-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

TEST(StudioCommands, BuiltinRegistryIsCompleteAndConflictFree)
{
    EXPECT_TRUE(StudioCommandController::validateBuiltinDefinitions().isEmpty());
}

TEST(StudioCommands, CommandPaletteUsesQtPortablePrimaryModifierPolicy)
{
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("macos")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("windows")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("linux")),
              QStringLiteral("Ctrl+Shift+P"));

#ifdef Q_OS_MACOS
    const auto native = QKeySequence::fromString(
                            StudioCommandController::paletteShortcutForPlatform(
                                QStringLiteral("macos")),
                            QKeySequence::PortableText)
                            .toString(QKeySequence::NativeText);
    EXPECT_TRUE(native.contains(QChar(0x2318))) << native.toStdString();
#endif
}

TEST(StudioCommands, FuzzySearchSupportsPrefixesSubsequencesAndMultipleTokens)
{
    const auto exact = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("show command"));
    const auto subsequence = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("scpal"));
    const auto missing = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("export raw"));

    EXPECT_GT(exact, subsequence);
    EXPECT_GE(subsequence, 0);
    EXPECT_EQ(missing, -1);
}

TEST(StudioCommands, FuzzySearchNormalizesCaseWidthAndDiacritics)
{
    EXPECT_GE(StudioCommandController::fuzzyScore(
                  QStringLiteral("Réglages"), QStringLiteral("Window"),
                  {QStringLiteral("preferences")}, QStringLiteral("studio.window.show_settings"),
                  QStringLiteral("REGLAGES")),
              0);
}

TEST(StudioCommands, ControllerRevalidatesStateAndRejectsInvalidDispatch)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();

    const auto import_action =
        controller.action(ids.value(QStringLiteral("libraryImportFiles")).toString());
    EXPECT_FALSE(import_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(import_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto invalid_path = controller.executeCommand(
        ids.value(QStringLiteral("libraryCreatePath")).toString(), QString{},
        QStringLiteral("control"));
    EXPECT_FALSE(invalid_path.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(invalid_path.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto unexpected_argument = controller.executeCommand(
        ids.value(QStringLiteral("windowCommandPalette")).toString(), 1,
        QStringLiteral("keyboard"));
    EXPECT_FALSE(unexpected_argument.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unexpected_argument.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto opened = controller.executeAction(
        ids.value(QStringLiteral("windowCommandPalette")).toString(),
        QStringLiteral("keyboard"));
    EXPECT_TRUE(opened.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.paletteOpen());

    controller.setPaletteOpen(false);
    bool found_palette_shortcut = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() !=
            ids.value(QStringLiteral("windowCommandPalette")).toString())
            continue;
        found_palette_shortcut = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(),
                  QStringLiteral("Ctrl+Shift+P"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    EXPECT_TRUE(found_palette_shortcut);
}

} // namespace
} // namespace ravo
