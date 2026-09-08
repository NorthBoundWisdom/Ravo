#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QKeySequence>
#include <QKeyEvent>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_keyboard_harness.h"
#include "studio_test_support.h"
#include "ravo/foundation/log.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "studio_import_production_window.h"
#include <QDir>
#include <QColor>
#include <QImage>
#include <QTemporaryDir>

namespace ravo
{
using namespace studio_test_support;
using studio_import_keyboard_harness::ImportKeyboardHarness;
using studio_import_keyboard_harness::make_candidates;
TEST(StudioImportKeyboard, HarnessLifetimeIsExplicit)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(8));

    QPointer<QQuickItem> root_watch;
    QPointer<QQuickItem> grid_watch;
    {
        ImportKeyboardHarness harness;
        ASSERT_TRUE(harness.load(&model));
        root_watch = harness.root.get();
        grid_watch = harness.grid;
        ASSERT_FALSE(root_watch.isNull());
        ASSERT_FALSE(grid_watch.isNull());

        ASSERT_TRUE(harness.load(&model));
        EXPECT_TRUE(root_watch.isNull());
        EXPECT_TRUE(grid_watch.isNull());
        root_watch = harness.root.get();
        grid_watch = harness.grid;
        ASSERT_FALSE(root_watch.isNull());
        ASSERT_FALSE(grid_watch.isNull());
        harness.reset();
        EXPECT_TRUE(root_watch.isNull());
        EXPECT_TRUE(grid_watch.isNull());
        EXPECT_EQ(harness.grid, nullptr);
        EXPECT_EQ(harness.root, nullptr);
    }
    EXPECT_TRUE(root_watch.isNull());
    EXPECT_TRUE(grid_watch.isNull());

    {
        ImportKeyboardHarness harness;
        ASSERT_TRUE(harness.load(&model));
        harness.reset();
        EXPECT_EQ(harness.root, nullptr);
        EXPECT_EQ(harness.grid, nullptr);
    }
}

TEST(StudioImportKeyboard, BatchCheckSkipsDuplicates)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(4, 1));

    ASSERT_EQ(model.selectedCount(), 3);
    model.highlightRange(0, 2, false);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));

    model.applyCheck(2);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_FALSE(model.data(model.index(0, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_FALSE(model.data(model.index(2, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_TRUE(model.data(model.index(3, 0), ImportCandidateListModel::SelectedRole).toBool());

    model.highlightAll();
    model.applyCheck(3);
    EXPECT_EQ(model.selectedCount(), 0);
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_FALSE(model.highlighted(1));
}

TEST(StudioImportKeyboard, GridContractMatchesProductionKeys)
{
    ensure_qt_core();
    QFile grid_file(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML));
    ASSERT_TRUE(grid_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto source = QString::fromUtf8(grid_file.readAll());

    EXPECT_TRUE(source.contains(QStringLiteral("activeFocusOnTab: true")));
    EXPECT_TRUE(source.contains(QStringLiteral("Keys.priority: Keys.BeforeItem")));
    EXPECT_TRUE(source.contains(QStringLiteral("function moveKeyboardFocus")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Left")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Right")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Up")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Down")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Home")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_End")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_PageUp")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_PageDown")));
    EXPECT_TRUE(source.contains(QStringLiteral("event.key === Qt.Key_Space")));
    EXPECT_FALSE(source.contains(QStringLiteral("highlightAll")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("highlightRange(root.selectionAnchor, bounded, additive)")));
    EXPECT_TRUE(source.contains(QStringLiteral("applyCheck(candidateGrid.currentIndex)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Accessible.description")));

    QFile photo_file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                     QStringLiteral("/Ravo/desktop/qml/chrome/ImportPhotoGrid.qml"));
    ASSERT_TRUE(photo_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto photo = QString::fromUtf8(photo_file.readAll());
    EXPECT_TRUE(photo.contains(QStringLiteral("ImportCandidateGrid")));
    EXPECT_TRUE(photo.contains(QStringLiteral("importCandidateCheckBox")));
    EXPECT_TRUE(photo.contains(QStringLiteral("accessibleName")));
    EXPECT_TRUE(photo.contains(QStringLiteral("accessibleDescription")));
    EXPECT_FALSE(photo.contains(QStringLiteral("function moveKeyboardFocus")));
    EXPECT_FALSE(photo.contains(QStringLiteral("Keys.onPressed")));

    QFile harness_file(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_KEYBOARD_HARNESS_QML));
    ASSERT_TRUE(harness_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto harness = QString::fromUtf8(harness_file.readAll());
    EXPECT_TRUE(harness.contains(QStringLiteral("productionGridUrl")));
    EXPECT_FALSE(harness.contains(QStringLiteral("function moveKeyboardFocus")));
    EXPECT_FALSE(harness.contains(QStringLiteral("Keys.onPressed")));
    EXPECT_FALSE(harness.contains(QStringLiteral("event.key === Qt.Key_PageDown")));
    EXPECT_FALSE(harness.contains(QStringLiteral("highlightAll")));

    QFile page_file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                    QStringLiteral("/Ravo/desktop/qml/chrome/ImportPage.qml"));
    ASSERT_TRUE(page_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto page_source = QString::fromUtf8(page_file.readAll());
    EXPECT_TRUE(page_source.contains(QStringLiteral("candidateKeyboardHelp")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("selectionArea.width >= 720")));
}

TEST(StudioImportKeyboard, GridEventsDriveHighlightAndCheck)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(24, 5));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    ASSERT_EQ(harness.currentIndex(), 0);
    ASSERT_EQ(harness.selectionAnchor(), 0);
    const int columns = harness.columnCount();
    ASSERT_GE(columns, 1);
    const int page = harness.pageStep();
    ASSERT_GE(page, columns);

    harness.key(Qt::Key_Right);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_FALSE(model.highlighted(0));
    EXPECT_EQ(harness.selectionAnchor(), 1);

    harness.key(Qt::Key_Down);
    EXPECT_EQ(harness.currentIndex(), 1 + columns);
    EXPECT_EQ(harness.selectionAnchor(), 1 + columns);

    harness.key(Qt::Key_Home);
    EXPECT_EQ(harness.currentIndex(), 0);
    harness.key(Qt::Key_End);
    EXPECT_EQ(harness.currentIndex(), 23);

    harness.key(Qt::Key_Home);
    harness.key(Qt::Key_PageDown);
    EXPECT_EQ(harness.currentIndex(), std::min(23, page));
    harness.key(Qt::Key_PageUp);
    EXPECT_EQ(harness.currentIndex(), 0);

    harness.key(Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_EQ(harness.selectionAnchor(), 0);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));

    const auto control = Qt::ControlModifier | Qt::MetaModifier;
    harness.key(Qt::Key_Right, control);
    EXPECT_EQ(harness.currentIndex(), 2);
    EXPECT_EQ(harness.selectionAnchor(), 0);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_FALSE(model.highlighted(2));

    harness.key(Qt::Key_Right, control | Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 3);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));
    EXPECT_TRUE(model.highlighted(3));

    harness.key(Qt::Key_Home);
    harness.key(Qt::Key_Down, Qt::ShiftModifier);
    harness.key(Qt::Key_Down, Qt::ShiftModifier);
    EXPECT_GE(harness.currentIndex(), 5);
    EXPECT_FALSE(model.highlighted(5));
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(4));

    const int selected_before = model.selectedCount();
    harness.key(Qt::Key_Space);
    EXPECT_NE(model.selectedCount(), selected_before);
    EXPECT_FALSE(model.data(model.index(5, 0), ImportCandidateListModel::SelectedRole).toBool());

    model.setAllSelected(true);
    model.highlightExclusive(4);
    constexpr int kMaxFocusSteps = 48;
    int focus_steps = 0;
    while (harness.currentIndex() != 4)
    {
        if (++focus_steps > kMaxFocusSteps)
        {
            ADD_FAILURE() << "bounded focus navigation failed"
                          << " index=" << harness.currentIndex()
                          << " anchor=" << harness.selectionAnchor()
                          << " focus=" << harness.gridHasFocus();
            return;
        }
        if (harness.currentIndex() < 4)
            harness.key(Qt::Key_Right, control);
        else
            harness.key(Qt::Key_Left, control);
    }
    harness.key(Qt::Key_Right, control);
    ASSERT_EQ(harness.currentIndex(), 5);
    EXPECT_FALSE(model.highlighted(5));
    harness.key(Qt::Key_Space);
    EXPECT_FALSE(model.data(model.index(5, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_EQ(model.selectedCount(), 23);
}
TEST(StudioImportKeyboard, WindowModifierVariantsDriveHighlight)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(12));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    ASSERT_EQ(harness.currentIndex(), 0);
    // Establish exclusive highlight before additive/modifier moves.
    harness.key(Qt::Key_Home);
    ASSERT_TRUE(model.highlighted(0));
    ASSERT_EQ(harness.selectionAnchor(), 0);

    harness.key(Qt::Key_Right, Qt::ControlModifier);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(1));
    EXPECT_EQ(harness.selectionAnchor(), 0);

    harness.key(Qt::Key_Right, Qt::MetaModifier);
    EXPECT_EQ(harness.currentIndex(), 2);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(2));

    harness.key(Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 3);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));
    EXPECT_TRUE(model.highlighted(3));
    EXPECT_EQ(harness.selectionAnchor(), 0);

    harness.key(Qt::Key_Home);
    harness.key(Qt::Key_Right, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));

    harness.key(Qt::Key_Home);
    model.highlightExclusive(0);
    harness.key(Qt::Key_Right, Qt::MetaModifier | Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
}

TEST(StudioImportKeyboard, ProductionWindowRoutesGridKeys)
{
    ensure_qt_core();
    using ravo::studio_import_production_window::ImportCandidateGridWindow;
    ImportCandidateGridWindow host;
    ASSERT_TRUE(host.load(16, -1)) << "production ImportCandidateGrid failed to load";
    ASSERT_EQ(host.currentIndex(), 0);

    host.key(Qt::Key_Right);
    EXPECT_EQ(host.currentIndex(), 1);
    EXPECT_TRUE(host.model.highlighted(1));
    EXPECT_FALSE(host.model.highlighted(0));

    host.key(Qt::Key_Right, Qt::ControlModifier);
    EXPECT_EQ(host.currentIndex(), 2);
    EXPECT_TRUE(host.model.highlighted(1));
    EXPECT_FALSE(host.model.highlighted(2));

    host.key(Qt::Key_Right, Qt::MetaModifier);
    EXPECT_EQ(host.currentIndex(), 3);
    EXPECT_TRUE(host.model.highlighted(1));
    EXPECT_FALSE(host.model.highlighted(3));

    host.key(Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_EQ(host.currentIndex(), 4);
    EXPECT_TRUE(host.model.highlighted(1));
    EXPECT_TRUE(host.model.highlighted(4));

    const int before = host.model.selectedCount();
    host.key(Qt::Key_Space);
    EXPECT_NE(host.model.selectedCount(), before);
}

TEST(StudioImportKeyboard, SelectAllCommandOwnsImportGalleryAndTextContexts)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto action = QStringLiteral("studio.photo.select_all");

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();

    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(Qt::cyan);
    const QString one = directory.filePath(QStringLiteral("one.png"));
    const QString two = directory.filePath(QStringLiteral("two.png"));
    ASSERT_TRUE(image.save(one, "PNG"));
    image.fill(Qt::magenta);
    ASSERT_TRUE(image.save(two, "PNG"));
    presenter.importFilePaths({one, two});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();

    // Gallery context: one owner, one shortcut entry.
    int shortcuts = 0;
    for (const auto &value : controller.shortcutEntries())
        if (value.toMap().value(QStringLiteral("actionId")).toString() == action)
            ++shortcuts;
    EXPECT_EQ(shortcuts, 1);
    presenter.selectAsset(presenter.assets()->assetIdAt(0));
    ASSERT_EQ(presenter.selectedCount(), 1);
    EXPECT_TRUE(controller.executeAction(action, QStringLiteral("keyboard"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    EXPECT_EQ(presenter.selectedCount(), 2);

    // Import context: same command highlights candidates, does not grow Gallery selection.
    // Use fresh pixels so catalog duplicates from the Gallery setup do not zero selection.
    const QString source = directory.filePath(QStringLiteral("import-source"));
    ASSERT_TRUE(QDir().mkpath(source));
    QImage fresh(16, 16, QImage::Format_RGB888);
    fresh.fill(QColor(10, 200, 30));
    ASSERT_TRUE(fresh.save(source + QStringLiteral("/a.png"), "PNG"));
    fresh.fill(QColor(200, 30, 10));
    ASSERT_TRUE(fresh.save(source + QStringLiteral("/b.png"), "PNG"));
    ASSERT_TRUE(
        QFile::copy(source + QStringLiteral("/a.png"), source + QStringLiteral("/duplicate.png")));
    presenter.openImportPage();
    ASSERT_TRUE(presenter.importPageOpen());
    presenter.setImportMode(QStringLiteral("add"));
    presenter.setImportSourceRoot(source);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.importCandidates()->rowCount() > 0 &&
                   presenter.importScanTotal() > 0 && !presenter.importScanActive();
        },
        30000))
        << presenter.errorText().toStdString() << " scanTotal=" << presenter.importScanTotal()
        << " rows=" << presenter.importCandidates()->rowCount()
        << " ready=" << presenter.importReady();
    auto *import_model = presenter.importCandidates();
    ASSERT_GE(import_model->rowCount(), 2);
    const int gallery_selected = presenter.selectedCount();
    const auto import_select = controller.executeAction(action, QStringLiteral("keyboard"));
    EXPECT_TRUE(import_select.value(QStringLiteral("accepted")).toBool())
        << import_select.value(QStringLiteral("message")).toString().toStdString()
        << " scanTotal=" << presenter.importScanTotal();
    int highlighted = 0;
    for (int row = 0; row < import_model->rowCount(); ++row)
        if (import_model->highlighted(row))
            ++highlighted;
    EXPECT_GE(highlighted, 1);
    EXPECT_EQ(presenter.selectedCount(), gallery_selected);

    // Text input yields: command must not change Import highlight/check.
    import_model->highlightExclusive(0);
    const int checked = import_model->selectedCount();
    const bool was_highlighted_zero = import_model->highlighted(0);
    controller.setTextInputActive(true);
    for (const auto &value : controller.shortcutEntries())
        if (value.toMap().value(QStringLiteral("actionId")).toString() == action)
            EXPECT_FALSE(value.toMap().value(QStringLiteral("enabled")).toBool());
    // Shortcut entries yield to text input; menu execute may still be available.
    // The contract under test is that the keyboard owner does not fire while typing.
    EXPECT_EQ(import_model->highlighted(0), was_highlighted_zero);
    EXPECT_EQ(import_model->selectedCount(), checked);
    controller.setTextInputActive(false);

    // Production grid must not invent a second Ctrl+A owner.
    QFile grid_file(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML));
    ASSERT_TRUE(grid_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto grid_source = QString::fromUtf8(grid_file.readAll());
    EXPECT_FALSE(grid_source.contains(QStringLiteral("Key_A")));
    EXPECT_FALSE(grid_source.contains(QStringLiteral("SelectAll")));
}

TEST(StudioImportKeyboard, FocusSurvivesScanAndPageLifecycles)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    EXPECT_EQ(harness.currentIndex(), -1);
    EXPECT_EQ(harness.selectionAnchor(), -1);

    model.setCandidates(make_candidates(8));
    QGuiApplication::processEvents();
    EXPECT_EQ(harness.currentIndex(), 0);
    EXPECT_EQ(harness.selectionAnchor(), 0);
    harness.key(Qt::Key_Right);
    EXPECT_EQ(harness.currentIndex(), 1);

    // Shrink / empty / refill without leaving stale indices.
    model.setCandidates(make_candidates(3));
    QGuiApplication::processEvents();
    EXPECT_GE(harness.currentIndex(), -1);
    EXPECT_LT(harness.currentIndex(), 3);
    EXPECT_GE(harness.selectionAnchor(), -1);
    EXPECT_LT(harness.selectionAnchor(), 3);

    model.setCandidates({});
    QGuiApplication::processEvents();
    EXPECT_EQ(harness.currentIndex(), -1);
    EXPECT_EQ(harness.selectionAnchor(), -1);

    model.setCandidates(make_candidates(5));
    QGuiApplication::processEvents();
    EXPECT_EQ(harness.currentIndex(), 0);
    EXPECT_EQ(harness.selectionAnchor(), 0);

    // Same count, new source identity replacement.
    model.setCandidates(make_candidates(5));
    QGuiApplication::processEvents();
    EXPECT_EQ(harness.currentIndex(), 0);
    harness.key(Qt::Key_End);
    EXPECT_EQ(harness.currentIndex(), 4);

    // Close/reopen harness (page lifecycle).
    harness.reset();
    ASSERT_TRUE(harness.load(&model));
    EXPECT_EQ(harness.currentIndex(), 0);
    harness.key(Qt::Key_Right);
    EXPECT_EQ(harness.currentIndex(), 1);
}

TEST(StudioImportKeyboard, LockedGridRejectsKeyboardAndKeepsSelection)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    model.setCandidates(make_candidates(6, 2));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    harness.key(Qt::Key_Right);
    ASSERT_EQ(harness.currentIndex(), 1);
    const int selected = model.selectedCount();
    const bool highlighted1 = model.highlighted(1);

    harness.root->setProperty("importWorkActive", true);
    QGuiApplication::processEvents();
    harness.key(Qt::Key_Right);
    EXPECT_EQ(harness.currentIndex(), 1);
    EXPECT_EQ(model.selectedCount(), selected);
    EXPECT_EQ(model.highlighted(1), highlighted1);
    harness.key(Qt::Key_Space);
    EXPECT_EQ(model.selectedCount(), selected);

    harness.root->setProperty("importWorkActive", false);
    QGuiApplication::processEvents();
    harness.key(Qt::Key_Right);
    EXPECT_EQ(harness.currentIndex(), 2);
}

TEST(StudioImportKeyboard, LateModelUpdateDoesNotStealTextFocusContract)
{
    ensure_qt_core();
    // Structural contract: ImportPage disables the grid while locked, and the
    // production grid only consumes Keys when interactionLocked is false.
    QFile page(QString::fromUtf8(RAVO_STUDIO_IMPORT_PAGE_QML));
    ASSERT_TRUE(page.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto page_source = QString::fromUtf8(page.readAll());
    EXPECT_TRUE(page_source.contains(QStringLiteral("readonly property bool locked")));
    EXPECT_TRUE(page_source.contains(
        QStringLiteral("importWorkActive || presenter.importPreflightActive")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("enabled: !root.locked")));

    QFile grid(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML));
    ASSERT_TRUE(grid.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto grid_source = QString::fromUtf8(grid.readAll());
    EXPECT_TRUE(grid_source.contains(QStringLiteral("if (root.interactionLocked)")));
    EXPECT_TRUE(grid_source.contains(QStringLiteral("return;")));
    // Passive focus must not be stolen by passive count/visible publication.
    EXPECT_TRUE(grid_source.contains(QStringLiteral("onVisibleChanged")));
    EXPECT_FALSE(grid_source.contains(QStringLiteral(
        "onVisibleChanged: if (visible) {\n            root.initializeKeyboardFocus();\n            forceActiveFocus();")));
    EXPECT_TRUE(grid_source.contains(QStringLiteral("function focusGrid")));
}

TEST(StudioImportKeyboard, FirstCandidatesDoNotStealFilenameTemplateFocus)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    QQmlEngine engine;
    QQuickWindow window;
    const QString host_path = QString::fromUtf8(RAVO_IMPORT_FOCUS_PUBLICATION_HOST_QML);
    QQmlComponent component(&engine, QUrl::fromLocalFile(host_path));
    if (component.isError())
    {
        for (const auto &error : component.errors())
            ADD_FAILURE() << error.toString().toStdString();
        GTEST_SKIP() << "UNTESTED: focus publication host failed to load";
    }
    auto *object = component.create();
    auto *raw_root = qobject_cast<QQuickItem *>(object);
    ASSERT_NE(raw_root, nullptr);
    studio_import_production_window::ScopedQuickItem root{raw_root};
    root->setParentItem(window.contentItem());
    root->setSize(QSizeF(900, 600));
    root->setProperty("productionGridUrl",
                      QUrl::fromLocalFile(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML)));
    root->setProperty("importCandidates", QVariant::fromValue(&model));
    window.resize(900, 600);
    window.show();
    window.requestActivate();
    QGuiApplication::processEvents();

    auto *field = root->findChild<QQuickItem *>(QStringLiteral("importFilenameTemplate"));
    ASSERT_NE(field, nullptr);
    QMetaObject::invokeMethod(root.get(), "focusFilenameTemplate", Qt::DirectConnection);
    QGuiApplication::processEvents();
    ASSERT_TRUE(field->hasActiveFocus());
    const auto selected_text = field->property("selectedText").toString();
    EXPECT_FALSE(selected_text.isEmpty());

    model.setCandidates(make_candidates(12));
    QGuiApplication::processEvents();
    EXPECT_TRUE(field->hasActiveFocus()) << "passive candidate publication stole text focus";
    EXPECT_EQ(field->property("selectedText").toString(), selected_text);

    QFile page(QString::fromUtf8(RAVO_STUDIO_IMPORT_PAGE_QML));
    ASSERT_TRUE(page.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto page_source = QString::fromUtf8(page.readAll());
    EXPECT_TRUE(page_source.contains(QStringLiteral("ImportDestinationPanel")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("ImportPhotoGrid")));
}

TEST(StudioImportKeyboard, FilenameTemplateSelectAllDoesNotMutateCandidates)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto action = QStringLiteral("studio.photo.select_all");

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));

    const QString source = directory.filePath(QStringLiteral("import-source"));
    ASSERT_TRUE(QDir().mkpath(source));
    QImage fresh(16, 16, QImage::Format_RGB888);
    fresh.fill(QColor(10, 200, 30));
    ASSERT_TRUE(fresh.save(source + QStringLiteral("/a.png"), "PNG"));
    fresh.fill(QColor(200, 30, 10));
    ASSERT_TRUE(fresh.save(source + QStringLiteral("/b.png"), "PNG"));
    presenter.openImportPage();
    presenter.setImportMode(QStringLiteral("add"));
    presenter.setImportSourceRoot(source);
    ASSERT_TRUE(wait_until(
        [&]
        { return presenter.importCandidates()->rowCount() > 0 && !presenter.importScanActive(); },
        30000));
    auto *import_model = presenter.importCandidates();
    import_model->highlightExclusive(0);
    const int checked = import_model->selectedCount();
    const int highlighted = [&]
    {
        int count = 0;
        for (int row = 0; row < import_model->rowCount(); ++row)
            count += import_model->highlighted(row);
        return count;
    }();
    const int gallery = presenter.selectedCount();

    // Product-shaped composition: text field + grid + Main.qml textInputActive Binding.
    // Do not manually setTextInputActive; the host Binding owns that product path.
    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(RAVO_STUDIO_QML_IMPORT_ROOT));
    engine.addImportPath(QString::fromUtf8(RAVO_GEOCONTROLS_QML_IMPORT_ROOT));
    engine.addImportPath(QString::fromUtf8(RAVO_GEOCONTROLS_APPSHELL_QML_IMPORT_ROOT));
    QQuickWindow window;
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QString::fromUtf8(RAVO_IMPORT_SELECT_ALL_TEXT_HOST_QML)));
    ASSERT_FALSE(component.isError()) << [&]
    {
        QStringList errors;
        for (const auto &error : component.errors())
            errors.push_back(error.toString());
        return errors.join(QLatin1Char('\n')).toStdString();
    }();
    auto *object = component.create();
    auto *raw_root = qobject_cast<QQuickItem *>(object);
    ASSERT_NE(raw_root, nullptr);
    studio_import_production_window::ScopedQuickItem root{raw_root};
    root->setParentItem(window.contentItem());
    root->setSize(QSizeF(900, 600));
    root->setProperty("productionGridUrl",
                      QUrl::fromLocalFile(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_GRID_QML)));
    root->setProperty("importCandidates", QVariant::fromValue(import_model));
    root->setProperty("commandController", QVariant::fromValue(&controller));
    window.resize(900, 600);
    window.show();
    window.requestActivate();
    QGuiApplication::processEvents();
    auto *field = root->findChild<QQuickItem *>(QStringLiteral("importFilenameTemplate"));
    ASSERT_NE(field, nullptr);
    const QString template_text = field->property("text").toString();
    ASSERT_FALSE(template_text.isEmpty());

    QMetaObject::invokeMethod(root.get(), "focusFilenameTemplateUnselected", Qt::DirectConnection);
    QGuiApplication::processEvents();
    ASSERT_TRUE(field->hasActiveFocus() || (window.activeFocusItem() != nullptr &&
                                            window.activeFocusItem()->parentItem() == field));
    ASSERT_TRUE(field->property("selectedText").toString().isEmpty())
        << "Select All precondition requires an empty text selection";
    ASSERT_TRUE(controller.textInputActive())
        << "product textInputActive Binding must follow focused text without manual override";

    int select_all_shortcuts = 0;
    for (const auto &value : controller.shortcutEntries())
    {
        if (value.toMap().value(QStringLiteral("actionId")).toString() != action)
            continue;
        ++select_all_shortcuts;
        EXPECT_FALSE(value.toMap().value(QStringLiteral("enabled")).toBool());
    }
    EXPECT_EQ(select_all_shortcuts, 1);

    {
        const QKeySequence select_all(QKeySequence::SelectAll);
        for (int index = 0; index < select_all.count(); ++index)
        {
            const QKeyCombination combo = select_all[static_cast<uint>(index)];
            QKeyEvent press(QEvent::KeyPress, combo.key(), combo.keyboardModifiers());
            QKeyEvent release(QEvent::KeyRelease, combo.key(), combo.keyboardModifiers());
            QCoreApplication::sendEvent(&window, &press);
            QCoreApplication::sendEvent(&window, &release);
        }
        QGuiApplication::processEvents();
    }
    EXPECT_EQ(field->property("selectedText").toString(), template_text);
    EXPECT_EQ(import_model->selectedCount(), checked);
    int highlighted_after = 0;
    for (int row = 0; row < import_model->rowCount(); ++row)
        highlighted_after += import_model->highlighted(row);
    EXPECT_EQ(highlighted_after, highlighted);
    EXPECT_EQ(presenter.selectedCount(), gallery);

    // Grid focus: the same command owner must accept exactly once and highlight candidates.
    QMetaObject::invokeMethod(root.get(), "focusCandidateGrid", Qt::DirectConnection);
    QGuiApplication::processEvents();
    ASSERT_FALSE(controller.textInputActive());
    import_model->highlightExclusive(0);
    const auto grid_select = controller.executeAction(action, QStringLiteral("keyboard"));
    EXPECT_TRUE(grid_select.value(QStringLiteral("accepted")).toBool())
        << grid_select.value(QStringLiteral("message")).toString().toStdString();
    int highlighted_grid = 0;
    for (int row = 0; row < import_model->rowCount(); ++row)
        highlighted_grid += import_model->highlighted(row) ? 1 : 0;
    EXPECT_GE(highlighted_grid, 1);
    EXPECT_EQ(presenter.selectedCount(), gallery);

    // Production ImportPage / DestinationPanel / StudioActions / GeoControls contracts.
    // Instantiating those QML graphs needs GeoControls qrc prefer paths owned by ravo_studio;
    // this command-test binary keeps source + filesystem membership as the hard gate.
    QFile page_file(QString::fromUtf8(RAVO_STUDIO_IMPORT_PAGE_QML));
    ASSERT_TRUE(page_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto page_source = QString::fromUtf8(page_file.readAll());
    EXPECT_TRUE(page_source.contains(QStringLiteral("import GeoControls 1.0")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("ImportDestinationPanel")));
    EXPECT_TRUE(page_source.contains(QStringLiteral("ImportPhotoGrid")));

    QFile destination_file(QFileInfo(QString::fromUtf8(RAVO_STUDIO_IMPORT_PAGE_QML))
                               .dir()
                               .filePath(QStringLiteral("ImportDestinationPanel.qml")));
    ASSERT_TRUE(destination_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto destination_source = QString::fromUtf8(destination_file.readAll());
    EXPECT_TRUE(destination_source.contains(QStringLiteral("CustomTextField")));
    EXPECT_TRUE(
        destination_source.contains(QStringLiteral("objectName: \"importFilenameTemplate\"")));
    ASSERT_TRUE(
        QFileInfo::exists(QDir(QString::fromUtf8(RAVO_GEOCONTROLS_QML_IMPORT_ROOT))
                              .filePath(QStringLiteral("GeoControls/CustomTextField.qml"))));

    QFile actions_file(QString::fromUtf8(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto actions_source = QString::fromUtf8(actions_file.readAll());
    EXPECT_TRUE(actions_source.contains(QStringLiteral("required property var controller")));
    EXPECT_TRUE(actions_source.contains(QStringLiteral("executeAction")));
    EXPECT_EQ(controller.ids().value(QStringLiteral("photoSelectAll")).toString(), action);

    QFile main_file(QFileInfo(QString::fromUtf8(RAVO_STUDIO_IMPORT_PAGE_QML))
                        .dir()
                        .filePath(QStringLiteral("../Main.qml")));
    if (!main_file.exists())
        main_file.setFileName(QDir(QString::fromUtf8(RAVO_REPOSITORY_ROOT))
                                  .filePath(QStringLiteral("Ravo/desktop/qml/Main.qml")));
    ASSERT_TRUE(main_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto main_source = QString::fromUtf8(main_file.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("property: \"textInputActive\"")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("window.textInputActive")));
}

TEST(StudioImportKeyboard, ProductionWindowKeysUseQTestWindowEntry)
{
    ensure_qt_core();
    studio_import_production_window::ImportCandidateGridWindow host;
    if (!host.load(16))
        GTEST_SKIP() << "UNTESTED: production ImportCandidateGrid window failed to activate";
    const int before = host.currentIndex();
    host.key(Qt::Key_Right);
    EXPECT_NE(host.currentIndex(), before);
}

} // namespace ravo
