#include <QFile>
#include <QPointer>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_keyboard_harness.h"
#include "studio_test_support.h"

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
    QFile file(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
               QStringLiteral("/Ravo/desktop/qml/chrome/ImportPhotoGrid.qml"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto source = QString::fromUtf8(file.readAll());

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
    EXPECT_FALSE(source.contains(QStringLiteral("importCandidates.highlightAll")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("highlightRange(root.selectionAnchor, bounded, additive)")));
    EXPECT_TRUE(source.contains(QStringLiteral("applyCheck(candidateGrid.currentIndex)")));
    EXPECT_TRUE(source.contains(QStringLiteral("candidateGrid.currentIndex = index")));
    EXPECT_TRUE(source.contains(QStringLiteral("Accessible.description")));

    QFile harness_file(QString::fromUtf8(RAVO_IMPORT_CANDIDATE_KEYBOARD_HARNESS_QML));
    ASSERT_TRUE(harness_file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto harness = QString::fromUtf8(harness_file.readAll());
    EXPECT_TRUE(harness.contains(QStringLiteral("Keys.priority: Keys.BeforeItem")));
    EXPECT_TRUE(harness.contains(QStringLiteral("function moveKeyboardFocus")));
    EXPECT_TRUE(harness.contains(QStringLiteral("event.key === Qt.Key_PageDown")));
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

} // namespace ravo
