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
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
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
    // Cancelled in-flight work must discard without publishing a thumbnail.
    EXPECT_FALSE(controller.discardedRows().empty());
    EXPECT_TRUE(controller.completedRows().empty());
    EXPECT_TRUE(model.thumbnail(0).isNull());
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
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
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

TEST(StudioImportThumbnailScheduler, DestroyedControllerDropsLateCompletion)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("late.png");
    {
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(Qt::yellow);
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path.toStdString();
    candidate.display_name = "late.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});

    std::promise<void> gate_promise;
    auto gate_future = gate_promise.get_future().share();
    {
        StudioImportThumbnailController controller(make_host(&model));
        controller.installDecodeGate(gate_future);
        controller.setViewportDemand({0}, 0, 0);
        QGuiApplication::processEvents();
        ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
        // Shutdown drains the worker (interruptible gate) without publishing.
        controller.shutdown();
        EXPECT_TRUE(controller.stopped());
        EXPECT_FALSE(controller.inFlight());
    }
    gate_promise.set_value();
    QGuiApplication::processEvents();
    EXPECT_TRUE(model.thumbnail(0).isNull());
}

TEST(StudioImportThumbnailScheduler, GenerationMismatchDiscardsWithoutMutatingModel)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto first = directory.filePath("a.png");
    const auto second = directory.filePath("b.png");
    {
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(Qt::red);
        ASSERT_TRUE(image.save(first, "PNG"));
        image.fill(Qt::blue);
        ASSERT_TRUE(image.save(second, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = first.toStdString();
    candidate.display_name = "a.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});
    std::uint64_t scan_generation = 1;
    StudioImportThumbnailController controller(make_host(&model, &scan_generation));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::promise<void> gate_promise;
    auto gate_future = gate_promise.get_future().share();
    controller.installDecodeGate(gate_future);
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
    // Replace source identity while decode is hung.
    candidate.source_path = second.toStdString();
    candidate.display_name = "b.png";
    model.setCandidates({candidate});
    ++scan_generation;
    controller.cancel("source_replaced");
    controller.resetOperation();
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    EXPECT_TRUE(model.thumbnail(0).isNull());
    EXPECT_FALSE(controller.discardedRows().empty());
}

TEST(StudioImportThumbnailScheduler, GateCancelInterruptsWaitWithoutPublishing)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("gated.png");
    {
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(Qt::cyan);
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path.toStdString();
    candidate.display_name = "gated.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});

    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::promise<void> gate_promise;
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.clearObservations();
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
    ASSERT_FALSE(controller.dispatchedRows().empty());

    controller.cancel("gate_cancel");
    // Do not release the gate; token must interrupt the worker wait.
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    EXPECT_TRUE(model.thumbnail(0).isNull());
    EXPECT_FALSE(controller.discardedRows().empty());
    EXPECT_TRUE(controller.completedRows().empty());
    const auto dispatched_after_cancel = controller.dispatchedCount();
    QGuiApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    QGuiApplication::processEvents();
    EXPECT_EQ(controller.dispatchedCount(), dispatched_after_cancel);
    gate_promise.set_value();
    controller.clearDecodeGate();
}

TEST(StudioImportThumbnailScheduler, ShutdownFinishesWhileGateHeld)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("shutdown-gate.png");
    {
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(Qt::magenta);
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path.toStdString();
    candidate.display_name = "shutdown-gate.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});

    std::promise<void> gate_promise;
    StudioImportThumbnailController controller(make_host(&model));
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
    controller.shutdown();
    EXPECT_TRUE(controller.stopped());
    EXPECT_FALSE(controller.inFlight());
    EXPECT_TRUE(model.thumbnail(0).isNull());
    gate_promise.set_value();
}

TEST(StudioImportThumbnailScheduler, CancelledOperationDoesNotInfiniteRedispatch)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < 4; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("c%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::gray);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("c%1.png").arg(row).toStdString();
        candidate.size_bytes = 8;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::promise<void> gate_promise;
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.setViewportDemand({0, 1, 2, 3}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
    controller.cancel("no_retry");
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    const auto dispatched = controller.dispatchedCount();
    for (int i = 0; i < 20; ++i)
    {
        QGuiApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(controller.dispatchedCount(), dispatched);
    EXPECT_TRUE(controller.completedRows().empty());
    EXPECT_EQ(controller.pendingCount(), 0U);
}

TEST(StudioImportThumbnailScheduler, DefaultObservationsStayConstantSpaceUnderEventStorm)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(64);
    for (int row = 0; row < 64; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.clearObservations();
    // 100k demand/wakeup-style events with diagnostics disabled must not grow a trail.
    for (int i = 0; i < 100000; ++i)
    {
        std::vector<int> rows = {i % 64, (i + 1) % 64, (i + 2) % 64};
        controller.setViewportDemand(rows, 0, rows.front());
    }
    EXPECT_EQ(controller.observationTrailSize(), 0U);
    EXPECT_TRUE(controller.observations().empty());
    EXPECT_GT(controller.wakeupScheduledCount(), 0U);
    EXPECT_GT(controller.replenishedCount(), 0U);
    EXPECT_EQ(controller.droppedObservationCount(), 0U);
    // Counters are constant-space; trail stays empty without an installed sink.
    for (const auto &event : controller.observations())
        EXPECT_TRUE(event.identity.source_path.isEmpty());
}

TEST(StudioImportThumbnailScheduler, DiagnosticRingBoundsTrailAndCountsDrops)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(32);
    for (int row = 0; row < 32; ++row)
        candidates[static_cast<std::size_t>(row)].source_path =
            "/tmp/" + std::to_string(row) + ".png";
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.enableDiagnosticRing(16);
    controller.clearObservations();
    for (int i = 0; i < 200; ++i)
    {
        std::vector<int> rows = {i % 32, (i + 1) % 32};
        controller.setViewportDemand(rows, 0, rows.front());
    }
    EXPECT_LE(controller.observationTrailSize(), 16U);
    EXPECT_GT(controller.droppedObservationCount(), 0U);
    for (const auto &event : controller.observations())
        EXPECT_TRUE(event.identity.source_path.isEmpty());
}

TEST(StudioImportThumbnailScheduler, ObservationSinkClearedOnShutdown)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(4);
    for (int row = 0; row < 4; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    StudioImportThumbnailController controller(make_host(&model));
    controller.setObservationSink(&trail);
    controller.setViewportDemand({0, 1}, 0, 0);
    EXPECT_FALSE(trail.empty());
    const auto size_before = trail.size();
    controller.shutdown();
    // Shutdown detaches the sink; further demand must not append.
    controller.setViewportDemand({2, 3}, 0, 2);
    EXPECT_EQ(trail.size(), size_before);
}

} // namespace ravo
