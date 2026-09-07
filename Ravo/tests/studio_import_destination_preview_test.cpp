#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>
#include <gtest/gtest.h>

#include "interactive_perf_report.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;
using interactive_perf_report::CaseMeta;
using interactive_perf_report::emit_case;
using interactive_perf_report::recorded_samples_from_env;
using interactive_perf_report::warmups_from_env;
namespace
{

struct ImportKeyboardHarness
{
    QQmlEngine engine;
    QQuickWindow window;
    QQuickItem *root = nullptr;
    QQuickItem *grid = nullptr;
    ImportCandidateListModel *model = nullptr;

    [[nodiscard]] bool load(ImportCandidateListModel *candidates)
    {
        model = candidates;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(
                                             RAVO_IMPORT_CANDIDATE_KEYBOARD_HARNESS_QML)));
        if (component.isError())
        {
            for (const auto &error : component.errors())
                ADD_FAILURE() << error.toString().toStdString();
            return false;
        }
        auto *object = component.create();
        root = qobject_cast<QQuickItem *>(object);
        if (!root)
        {
            delete object;
            return false;
        }
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(800, 600));
        root->setProperty("importCandidates", QVariant::fromValue(model));
        grid = root->findChild<QQuickItem *>(QStringLiteral("importCandidateKeyboardGrid"));
        if (!grid)
            return false;
        window.resize(800, 600);
        window.show();
        QGuiApplication::processEvents();
        QMetaObject::invokeMethod(root, "focusCandidateGrid", Qt::DirectConnection);
        QGuiApplication::processEvents();
        return grid->hasActiveFocus();
    }

    void key(const int key, const Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        ASSERT_NE(grid, nullptr);
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers);
        QCoreApplication::sendEvent(grid, &press);
        QCoreApplication::sendEvent(grid, &release);
        QGuiApplication::processEvents();
    }

    [[nodiscard]] int currentIndex() const
    {
        return grid ? grid->property("currentIndex").toInt() : -1;
    }

    [[nodiscard]] int selectionAnchor() const
    {
        return root ? root->property("selectionAnchor").toInt() : -1;
    }

    [[nodiscard]] qreal contentY() const
    {
        return grid ? grid->property("contentY").toReal() : 0;
    }

    [[nodiscard]] int columnCount() const
    {
        QVariant columns;
        QMetaObject::invokeMethod(root, "keyboardColumnCount", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariant, columns));
        return std::max(1, columns.toInt());
    }

    [[nodiscard]] int pageStep() const
    {
        QVariant step;
        QMetaObject::invokeMethod(root, "keyboardPageStep", Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariant, step));
        return std::max(1, step.toInt());
    }
};

[[nodiscard]] std::vector<ImportCandidate> make_candidates(const int count,
                                                           const int duplicate_row = -1)
{
    std::vector<ImportCandidate> candidates(static_cast<std::size_t>(count));
    for (int row = 0; row < count; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path =
            "/candidate-" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].display_name =
            "candidate-" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].size_bytes =
            static_cast<std::uint64_t>((row + 1) * 10);
    }
    if (duplicate_row >= 0 && duplicate_row < count)
    {
        candidates[static_cast<std::size_t>(duplicate_row)].duplicate = true;
        candidates[static_cast<std::size_t>(duplicate_row)].duplicate_reason = "catalog_content";
    }
    return candidates;
}

void expect_candidate_keyboard_batch_check_skips_duplicates()
{
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

void expect_candidate_grid_keyboard_contract()
{
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

void expect_candidate_grid_keyboard_events()
{
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

    // Ctrl/Cmd-only moves focus and preserves the existing highlight set.
    const auto control = Qt::ControlModifier | Qt::MetaModifier;
    harness.key(Qt::Key_Right, control);
    EXPECT_EQ(harness.currentIndex(), 2);
    EXPECT_EQ(harness.selectionAnchor(), 0);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_FALSE(model.highlighted(2));

    // Ctrl/Cmd+Shift adds the new range without clearing the previous highlight.
    harness.key(Qt::Key_Right, control | Qt::ShiftModifier);
    EXPECT_EQ(harness.currentIndex(), 3);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));
    EXPECT_TRUE(model.highlighted(3));

    // Range across the duplicate row keeps the duplicate read-only.
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

    // Space on a focused duplicate must not check it.
    model.setAllSelected(true);
    model.highlightExclusive(4);
    while (harness.currentIndex() != 4)
    {
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

void expect_large_candidate_focus_scroll_budgets()
{
    // Enforceable harness ceilings + PERF-01-style observation only — not a PERF-02 admit.
    constexpr int kCandidateCount = 1200;
    constexpr std::int64_t kFocusMoveCeilingUs = 50'000;   // 50 ms / move
    constexpr std::int64_t kPageScrollCeilingUs = 100'000; // 100 ms / page step

    ImportCandidateListModel model;
    model.setCandidates(make_candidates(kCandidateCount));
    ImportKeyboardHarness harness;
    ASSERT_TRUE(harness.load(&model));
    ASSERT_EQ(harness.currentIndex(), 0);

    const std::size_t warmups = warmups_from_env(1U);
    const std::size_t recorded = recorded_samples_from_env(6U);
    std::vector<std::int64_t> arrow_samples;
    std::vector<std::int64_t> page_samples;
    arrow_samples.reserve(recorded);
    page_samples.reserve(recorded);

    for (std::size_t i = 0; i < warmups + recorded; ++i)
    {
        harness.key(Qt::Key_Home);
        const auto before_y = harness.contentY();
        const auto start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_Down);
        harness.key(Qt::Key_Right);
        const auto arrow_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        const auto page_start = std::chrono::steady_clock::now();
        harness.key(Qt::Key_PageDown);
        const auto page_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - page_start)
                                 .count();
        if (i >= warmups)
        {
            arrow_samples.push_back(arrow_us);
            page_samples.push_back(page_us);
        }
        EXPECT_GE(harness.currentIndex(), 0);
        static_cast<void>(before_y);
    }

    CaseMeta arrow_meta;
    arrow_meta.case_id = "import_candidate_keyboard_focus_move";
    arrow_meta.path = "import_candidate_grid";
    arrow_meta.unit = "us";
    arrow_meta.cache_state = "warm";
    arrow_meta.source_kind = "synthetic_candidates";
    arrow_meta.file_count = static_cast<std::size_t>(kCandidateCount);
    arrow_meta.warmups = warmups;
    arrow_meta.recorded_samples = recorded;
    emit_case(arrow_meta, arrow_samples);

    CaseMeta page_meta = arrow_meta;
    page_meta.case_id = "import_candidate_keyboard_page_scroll";
    emit_case(page_meta, page_samples);

    const auto arrow_summary = interactive_perf_report::summarize(arrow_samples);
    const auto page_summary = interactive_perf_report::summarize(page_samples);
    EXPECT_LE(arrow_summary.p90, kFocusMoveCeilingUs)
        << "import focus move p90=" << arrow_summary.p90;
    EXPECT_LE(page_summary.p90, kPageScrollCeilingUs)
        << "import page scroll p90=" << page_summary.p90;
    EXPECT_GT(harness.contentY(), 0);
}

} // namespace

TEST(StudioImportWorkspace, DestinationPreviewTracksSelectionAndOrganizationWithoutCreatingFolders)
{
    ensure_qt_core();
    expect_candidate_keyboard_batch_check_skips_duplicates();
    expect_candidate_grid_keyboard_contract();
    expect_candidate_grid_keyboard_events();
    expect_large_candidate_focus_scroll_budgets();

    init_logging("ravo-import-preview-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source + "/nested"));
    ASSERT_TRUE(QDir().mkpath(destination));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(source + "/a.png"));
    image.fill(Qt::blue);
    ASSERT_TRUE(image.save(source + "/nested/b.png"));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    presenter.setImportOrganization(QStringLiteral("hierarchy"));
    const auto ready = [&]
    {
        return !presenter.importScanActive() && !presenter.importDestinationPreviewActive() &&
               !presenter.importDestinationPreview().empty();
    };
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    ASSERT_TRUE(presenter.importDestinationPreviewError().isEmpty());
    auto folders = presenter.importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.front().toMap().value("photoCount").toUInt(), 2U);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 1U);
    EXPECT_TRUE(folders.back().toMap().value("willCreate").toBool());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.importCandidates()->setAllSelected(false);
    EXPECT_TRUE(presenter.importDestinationPreview().empty());
    presenter.importCandidates()->setAllSelected(true);
    presenter.setImportOrganization(QStringLiteral("date"));
    presenter.setImportOrganization(QStringLiteral("month"));
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    folders = presenter.importDestinationPreview();
    ASSERT_EQ(folders.size(), 3);
    EXPECT_EQ(folders.back().toMap().value("depth").toInt(), 2);
    EXPECT_EQ(folders.back().toMap().value("photoCount").toUInt(), 2U);
    presenter.setImportOrganization(QStringLiteral("hierarchy"));
    presenter.closeImportPage();
    EXPECT_FALSE(presenter.importDestinationPreviewActive());
    EXPECT_TRUE(presenter.importDestinationPreview().empty());
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.openImportPage();
    presenter.setImportDestination(destination);
    ASSERT_TRUE(wait_until(ready)) << presenter.importDestinationPreviewError().toStdString();
    presenter.startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
        30000));
    EXPECT_EQ(presenter.lastImportCount(), 2U);
    EXPECT_TRUE(QFile::exists(destination + "/source/a.png"));
    EXPECT_TRUE(QFile::exists(destination + "/source/nested/b.png"));
    EXPECT_TRUE(QFile::exists(source + "/a.png"));
}
} // namespace ravo
