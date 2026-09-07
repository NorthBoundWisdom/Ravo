#include <atomic>
#include <functional>
#include <chrono>
#include <future>
#include <thread>

#include <QGuiApplication>
#include <QTemporaryDir>
#include <QImage>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

namespace
{
StudioImportThumbnailController::Host make_host(ImportCandidateListModel *model,
                                                std::uint64_t *scan_generation = nullptr)
{
    return StudioImportThumbnailController::Host{
        model,
        nullptr,
        [] { return true; },
        [] { return false; },
        [] { return false; },
        [scan_generation] { return scan_generation ? *scan_generation : 1ULL; },
        {},
    };
}

bool wait_for(const std::function<bool()> &pred, int timeout_ms = 5000)
{
    return wait_until(pred, timeout_ms);
}
} // namespace

TEST(StudioImportThumbnailScheduler, ViewportDemandCapsPendingAndPrefersNewViewport)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(100000);
    for (int row = 0; row < 100000; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));

    StudioImportThumbnailController controller(make_host(&model));

    std::vector<int> start_rows(300);
    for (int i = 0; i < 300; ++i)
        start_rows[static_cast<std::size_t>(i)] = i;
    controller.setViewportDemand(start_rows, 2, 0);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
    EXPECT_EQ(controller.pendingHardCap(), 256U);

    std::vector<int> end_rows;
    for (int row = 99900; row < 100000; ++row)
        end_rows.push_back(row);
    controller.setViewportDemand(end_rows, 2, 99999);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
    EXPECT_LE(controller.pendingCount(), end_rows.size() + 2U);
}

TEST(StudioImportThumbnailScheduler, EnsureRespectsPendingHardCap)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(1000);
    for (int row = 0; row < 1000; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    for (int row = 0; row < 1000; ++row)
        controller.ensure(row);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
}

TEST(StudioImportThumbnailScheduler, CancelClearsPendingAndAllowsRecovery)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(4);
    for (int row = 0; row < 4; ++row)
    {
        candidates[static_cast<std::size_t>(row)].source_path = "/t" + std::to_string(row) + ".png";
        candidates[static_cast<std::size_t>(row)].display_name = "t.png";
        candidates[static_cast<std::size_t>(row)].size_bytes = 10;
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.setViewportDemand({0, 1, 2, 3}, 0);
    EXPECT_GT(controller.pendingCount(), 0U);
    controller.cancel("test_cancel");
    controller.clearPending();
    EXPECT_EQ(controller.pendingCount(), 0U);
    EXPECT_FALSE(controller.inFlight());
    controller.resetOperation();
    controller.setViewportDemand({0, 1}, 0);
    EXPECT_GT(controller.pendingCount(), 0U);
}

TEST(StudioImportThumbnailScheduler, WakeupsCoalesceUnderEnsureStorm)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(1200);
    for (int row = 0; row < 1200; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.clearObservations();
    for (int row = 0; row < 1000; ++row)
        controller.ensure(row);
    for (int round = 0; round < 50; ++round)
    {
        std::vector<int> rows;
        for (int i = 0; i < 40; ++i)
            rows.push_back(round * 10 + i);
        controller.setViewportDemand(rows, 2, rows.front());
    }
    // Coalesce: scheduled wakeups must be far below storm size.
    EXPECT_LE(controller.wakeupScheduledCount(), 60U);
    EXPECT_LE(controller.pendingCount(), controller.pendingHardCap());
    EXPECT_LE(controller.pendingHighWater(), controller.pendingHardCap());
}

TEST(StudioImportThumbnailScheduler, DispatchCheckpointsObserveInFlightBeforeCancel)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("shot.png");
    {
        QImage image(32, 24, QImage::Format_RGB888);
        image.fill(Qt::green);
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path.toStdString();
    candidate.display_name = "shot.png";
    candidate.size_bytes = 32;
    model.setCandidates({candidate});

    StudioImportThumbnailController controller(make_host(&model));
    std::promise<void> gate_promise;
    auto gate_future = gate_promise.get_future().share();
    controller.installDecodeGate(gate_future);
    controller.clearObservations();
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(
        wait_for([&] { return controller.inFlight() || !controller.dispatchedRows().empty(); }));
    ASSERT_FALSE(controller.dispatchedRows().empty());
    EXPECT_EQ(controller.dispatchedRows().front(), 0);
    EXPECT_TRUE(controller.inFlight());

    controller.cancel("test_cancel_inflight");
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    // Completion may be discarded due to cancel; must not hang forever.
    const auto discarded = controller.discardedRows();
    const auto completed = controller.completedRows();
    EXPECT_TRUE(!discarded.empty() || !completed.empty());
}

TEST(StudioImportThumbnailScheduler, ReplenishAdmitsRemainingDemandAfterCompletion)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < 8; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("%1.png").arg(row));
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(QColor(row * 20, 40, 80));
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("%1.png").arg(row).toStdString();
        candidate.size_bytes = 16;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    // Tiny capacity simulation: demand 8, hard cap still 256 so all admit; verify completion set.
    controller.setViewportDemand({0, 1, 2, 3, 4, 5, 6, 7}, 0, 3);
    ASSERT_TRUE(wait_for(
        [&]
        {
            int ready = 0;
            for (int row = 0; row < model.rowCount(); ++row)
                ready += !model.thumbnail(row).isNull() || model.inspected(row);
            return ready == 8;
        },
        30000));
    const auto completed = controller.completedRows();
    EXPECT_EQ(completed.size(), 8U);
    // Current row should be among the earliest dispatches.
    ASSERT_FALSE(controller.dispatchedRows().empty());
    EXPECT_EQ(controller.dispatchedRows().front(), 3);
}

TEST(StudioImportThumbnailScheduler, LegacyEnsureDoesNotRepromoteOffViewport)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(100);
    for (int row = 0; row < 100; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.setViewportDemand({50, 51, 52}, 1, 51);
    const auto pending_before = controller.pendingCount();
    controller.ensure(0); // off-viewport legacy ensure must not reinsert
    EXPECT_EQ(controller.pendingCount(), pending_before);
    controller.ensure(51); // already demanded — ok if already pending
    EXPECT_LE(controller.pendingCount(), pending_before + 1);
}

TEST(StudioImportThumbnailScheduler, StoppedControllerRejectsNewDemand)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(4);
    for (int row = 0; row < 4; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.shutdown();
    EXPECT_TRUE(controller.stopped());
    controller.setViewportDemand({0, 1}, 0, 0);
    EXPECT_EQ(controller.pendingCount(), 0U);
    controller.ensure(0);
    EXPECT_EQ(controller.pendingCount(), 0U);
}

} // namespace ravo
