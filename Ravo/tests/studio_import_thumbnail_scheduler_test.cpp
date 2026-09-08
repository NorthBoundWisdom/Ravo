#include <atomic>
#include <set>
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

TEST(StudioImportThumbnailScheduler, OverBudgetDemandReachesFiniteTerminalWithoutThrash)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    constexpr int kRows = 300;
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    candidates.reserve(kRows);
    for (int row = 0; row < kRows; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(QColor((row * 17) % 256, 40, 80));
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("%1.png").arg(row).toStdString();
        candidate.size_bytes = 64;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));

    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::vector<int> demand(kRows);
    for (int row = 0; row < kRows; ++row)
        demand[static_cast<std::size_t>(row)] = row;
    controller.setViewportDemand(demand, 0, 3);

    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 120000));

    const auto completed = controller.completedCount();
    const auto satisfied = controller.demandSatisfiedCount();
    const auto deferred = controller.demandCapacityDeferredCount();
    EXPECT_LE(completed, static_cast<std::uint64_t>(controller.pendingHardCap()) + 8U);
    EXPECT_LE(satisfied, controller.pendingHardCap());
    EXPECT_EQ(satisfied + deferred, static_cast<std::size_t>(kRows));
    EXPECT_GT(deferred, 0U);
    // Must not thrash: far below the 3,000 re-decode feedback loop.
    EXPECT_LE(completed, 400U);
    EXPECT_TRUE(controller.demandQuiescent());
    EXPECT_FALSE(model.thumbnail(3).isNull());
}

TEST(StudioImportThumbnailScheduler, InBudgetVisibleDemandCompletes)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    constexpr int kRows = 32;
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < kRows; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("in%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::darkGreen);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("in%1.png").arg(row).toStdString();
        candidate.size_bytes = 64;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::vector<int> demand(kRows);
    for (int row = 0; row < kRows; ++row)
        demand[static_cast<std::size_t>(row)] = row;
    controller.setViewportDemand(demand, 0, 0);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 60000));
    EXPECT_EQ(controller.demandSatisfiedCount(), static_cast<std::size_t>(kRows));
    EXPECT_EQ(controller.demandCapacityDeferredCount(), 0U);
    EXPECT_EQ(controller.completedCount(), static_cast<std::uint64_t>(kRows));
    for (int row = 0; row < kRows; ++row)
        EXPECT_FALSE(model.thumbnail(row).isNull()) << row;
}

TEST(StudioImportThumbnailScheduler, OverBudgetComparesDispatchCompleteAndDeferredSets)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    constexpr int kRows = 300;
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < kRows; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("s%1.png").arg(row));
        QImage image(4, 4, QImage::Format_RGB888);
        image.fill(Qt::blue);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("s%1.png").arg(row).toStdString();
        candidate.size_bytes = 32;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::vector<int> demand(kRows);
    for (int row = 0; row < kRows; ++row)
        demand[static_cast<std::size_t>(row)] = row;
    controller.setViewportDemand(demand, 0, 0);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 120000));

    std::set<int> requested(demand.begin(), demand.end());
    std::set<int> dispatched;
    std::set<int> completed;
    for (const int row : controller.dispatchedRows())
        if (requested.count(row) > 0)
            dispatched.insert(row);
    for (const int row : controller.completedRows())
        if (requested.count(row) > 0)
            completed.insert(row);
    EXPECT_EQ(dispatched, completed);
    EXPECT_EQ(completed.size(), controller.demandSatisfiedCount());
    EXPECT_EQ(controller.demandSatisfiedCount() + controller.demandCapacityDeferredCount(),
              requested.size());
    // Each demanded row is allowed at most one successful completion in this generation.
    EXPECT_EQ(controller.completedCount(),
              static_cast<std::uint64_t>(controller.demandSatisfiedCount()));
    EXPECT_LE(controller.dispatchedCount(),
              controller.completedCount() + controller.discardedCount());
}

TEST(StudioImportThumbnailScheduler, ViewportEndThenFirstRebuildsUnderNewDemandGeneration)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    constexpr int kRows = 64;
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < kRows; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("v%1.png").arg(row));
        QImage image(4, 4, QImage::Format_RGB888);
        image.fill(Qt::darkCyan);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("v%1.png").arg(row).toStdString();
        candidate.size_bytes = 32;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    const auto first_gen = controller.demandGeneration();
    controller.setViewportDemand({0, 1, 2, 3}, 0, 0);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 30000));
    EXPECT_GT(controller.demandGeneration(), first_gen);
    EXPECT_FALSE(model.thumbnail(0).isNull());

    controller.setViewportDemand({60, 61, 62, 63}, 0, 63);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 30000));
    EXPECT_FALSE(model.thumbnail(63).isNull());

    controller.setViewportDemand({0, 1, 2, 3}, 0, 0);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 30000));
    EXPECT_FALSE(model.thumbnail(0).isNull());
}

TEST(StudioImportThumbnailScheduler, SourcePathChangeUnderGateUsesFreshDemand)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path_a = directory.filePath("A.png");
    const auto path_b = directory.filePath("B.png");
    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::red);
        ASSERT_TRUE(image.save(path_a, "PNG"));
        image.fill(Qt::blue);
        ASSERT_TRUE(image.save(path_b, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path_a.toStdString();
    candidate.display_name = "A.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});
    std::uint64_t scan_generation = 1;
    StudioImportThumbnailController controller(make_host(&model, &scan_generation));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::promise<void> gate_promise;
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));

    candidate.source_path = path_b.toStdString();
    candidate.display_name = "B.png";
    model.setCandidates({candidate});
    ++scan_generation;
    controller.cancel("path_swap");
    controller.resetOperation();
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    EXPECT_TRUE(model.thumbnail(0).isNull());

    controller.setViewportDemand({0}, 0, 0);
    ASSERT_TRUE(wait_for(
        [&] { return controller.demandQuiescent() && !model.thumbnail(0).isNull(); }, 30000));
    EXPECT_EQ(model.sourcePath(0), path_b);
}

TEST(StudioImportThumbnailScheduler, QuiescenceRequiresPendingAndKickDrain)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(4);
    for (int row = 0; row < 4; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.setViewportDemand({0, 1, 2, 3}, 0, 0);
    // Immediately after demand, work may not be in-flight yet but is not quiescent.
    EXPECT_FALSE(controller.demandQuiescent());
    EXPECT_TRUE(controller.pendingCount() > 0 || controller.inFlight() ||
                controller.wakeupScheduledCount() > 0);
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

TEST(StudioImportThumbnailScheduler, ResetSourceSessionClearsTerminalsForEnsureRebuild)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < 3; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("s%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(row == 0 ? Qt::red : (row == 1 ? Qt::green : Qt::blue));
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("s%1.png").arg(row).toStdString();
        candidate.size_bytes = 16;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(candidates);
    std::uint64_t scan_generation = 1;
    StudioImportThumbnailController controller(make_host(&model, &scan_generation));
    for (int row = 0; row < 3; ++row)
        controller.ensure(row);
    ASSERT_TRUE(wait_for(
        [&]
        {
            for (int row = 0; row < 3; ++row)
                if (!model.inspected(row) || model.thumbnail(row).isNull())
                    return false;
            return controller.demandQuiescent();
        },
        30000));
    EXPECT_EQ(controller.demandTerminalCount(), 3U);

    // Same-count model replacement without session reset leaves row terminals that
    // reject ensure — the reopen/rescan regression before resetSourceSession.
    std::vector<ImportCandidate> replaced;
    for (int row = 0; row < 3; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("r%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::darkYellow);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("r%1.png").arg(row).toStdString();
        candidate.size_bytes = 16;
        replaced.push_back(std::move(candidate));
    }
    ++scan_generation;
    model.setCandidates(replaced);
    EXPECT_EQ(model.rowCount(), 3);
    EXPECT_GT(controller.demandTerminalCount(), 0U);
    for (int row = 0; row < 3; ++row)
        controller.ensure(row);
    EXPECT_EQ(controller.pendingCount(), 0U);
    EXPECT_TRUE(model.thumbnail(0).isNull());

    controller.cancel("model_replaced");
    controller.resetOperation();
    controller.resetSourceSession();
    EXPECT_EQ(controller.demandTerminalCount(), 0U);
    for (int row = 0; row < 3; ++row)
        controller.ensure(row);
    ASSERT_TRUE(wait_for(
        [&]
        {
            for (int row = 0; row < 3; ++row)
                if (!model.inspected(row) || model.thumbnail(row).isNull())
                    return false;
            return controller.demandQuiescent();
        },
        30000));
    EXPECT_FALSE(model.thumbnail(0).isNull());
}

TEST(StudioImportThumbnailScheduler, ResetSourceSessionDropsStaleInFlightTerminalWrite)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path_a = directory.filePath("genA.png");
    const auto path_b = directory.filePath("genB.png");
    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::red);
        ASSERT_TRUE(image.save(path_a, "PNG"));
        image.fill(Qt::blue);
        ASSERT_TRUE(image.save(path_b, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path_a.toStdString();
    candidate.display_name = "genA.png";
    candidate.size_bytes = 16;
    model.setCandidates({candidate});
    std::uint64_t scan_generation = 1;
    StudioImportThumbnailController controller(make_host(&model, &scan_generation));
    std::promise<void> gate_promise;
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.ensure(0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));

    candidate.source_path = path_b.toStdString();
    candidate.display_name = "genB.png";
    model.setCandidates({candidate});
    ++scan_generation;
    controller.cancel("session_reset");
    controller.resetOperation();
    controller.resetSourceSession();
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    EXPECT_TRUE(model.thumbnail(0).isNull());
    EXPECT_EQ(controller.demandTerminalCount(), 0U);

    controller.ensure(0);
    ASSERT_TRUE(wait_for(
        [&] { return controller.demandQuiescent() && !model.thumbnail(0).isNull(); }, 30000));
    EXPECT_EQ(model.sourcePath(0), path_b);
    EXPECT_EQ(model.thumbnail(0).pixelColor(0, 0), QColor(Qt::blue));
}

TEST(StudioImportThumbnailScheduler, EvictionClearsInspectedAndCapacityDeferredIsNotLoading)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    constexpr int kRows = 300;
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates;
    for (int row = 0; row < kRows; ++row)
    {
        const auto path = directory.filePath(QStringLiteral("e%1.png").arg(row));
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::darkGreen);
        ASSERT_TRUE(image.save(path, "PNG"));
        ImportCandidate candidate;
        candidate.source_path = path.toStdString();
        candidate.display_name = QStringLiteral("e%1.png").arg(row).toStdString();
        candidate.size_bytes = 16;
        candidates.push_back(std::move(candidate));
    }
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    std::vector<int> demand(kRows);
    for (int row = 0; row < kRows; ++row)
        demand[static_cast<std::size_t>(row)] = row;
    controller.setViewportDemand(demand, 0, 0);
    ASSERT_TRUE(wait_for([&] { return controller.demandQuiescent(); }, 60000));
    EXPECT_EQ(controller.demandSatisfiedCount() + controller.demandFailedCount(),
              StudioImportThumbnailController::kDemandDecodeBudget);
    EXPECT_GT(controller.demandCapacityDeferredCount(), 0U);
    int loading = 0;
    int deferred_loading = 0;
    for (int row = 0; row < kRows; ++row)
    {
        loading += model.thumbnailLoading(row) ? 1 : 0;
        if (model.thumbnail(row).isNull() && !model.inspected(row) && model.thumbnailLoading(row))
            ++deferred_loading;
    }
    EXPECT_EQ(loading, 0);
    EXPECT_EQ(deferred_loading, 0);

    // Force eviction of early rows by finishing beyond the count budget via direct model path,
    // then admit scroll-back under a fresh demand generation.
    QImage filler(16, 16, QImage::Format_RGB888);
    filler.fill(Qt::red);
    for (int row = 0; row < kRows; ++row)
        model.finishThumbnail(row, filler);
    EXPECT_FALSE(model.inspected(0));
    EXPECT_TRUE(model.thumbnail(0).isNull());
    EXPECT_FALSE(model.thumbnailLoading(0));

    controller.setViewportDemand({0, 1, 2, 3}, 0, 0);
    ASSERT_TRUE(wait_for(
        [&]
        {
            return controller.demandQuiescent() && !model.thumbnail(0).isNull() &&
                   model.inspected(0);
        },
        30000));
    EXPECT_FALSE(model.thumbnailLoading(0));
}

TEST(StudioImportThumbnailScheduler, IdenticalViewportDemandIsIdempotent)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(8);
    for (int row = 0; row < 8; ++row)
        candidates[static_cast<std::size_t>(row)].source_path = std::to_string(row);
    model.setCandidates(std::move(candidates));
    StudioImportThumbnailController controller(make_host(&model));
    controller.setViewportDemand({0, 1, 2}, 1, 1);
    const auto generation = controller.demandGeneration();
    const auto terminals = controller.demandTerminalCount();
    const auto pending = controller.pendingCount();
    for (int i = 0; i < 100; ++i)
        controller.setViewportDemand({0, 1, 2}, 1, 1);
    EXPECT_EQ(controller.demandGeneration(), generation);
    EXPECT_EQ(controller.demandTerminalCount(), terminals);
    EXPECT_EQ(controller.pendingCount(), pending);
}

TEST(StudioImportThumbnailScheduler, StaleDemandCompletionDoesNotWriteNewTerminals)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path_a = directory.filePath("demandA.png");
    const auto path_b = directory.filePath("demandB.png");
    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::red);
        ASSERT_TRUE(image.save(path_a, "PNG"));
        image.fill(Qt::blue);
        ASSERT_TRUE(image.save(path_b, "PNG"));
    }
    ImportCandidateListModel model;
    std::vector<ImportCandidate> candidates(2);
    candidates[0].source_path = path_a.toStdString();
    candidates[0].display_name = "demandA.png";
    candidates[0].size_bytes = 16;
    candidates[1].source_path = path_b.toStdString();
    candidates[1].display_name = "demandB.png";
    candidates[1].size_bytes = 16;
    model.setCandidates(candidates);
    StudioImportThumbnailController controller(make_host(&model));
    std::vector<StudioImportThumbnailController::ObservationEvent> trail;
    controller.setObservationSink(&trail);
    std::promise<void> gate_promise;
    controller.installDecodeGate(gate_promise.get_future().share());
    controller.setViewportDemand({0}, 0, 0);
    QGuiApplication::processEvents();
    ASSERT_TRUE(wait_for([&] { return controller.inFlight(); }));
    const auto old_gen = controller.demandGeneration();

    controller.setViewportDemand({1}, 0, 1);
    EXPECT_GT(controller.demandGeneration(), old_gen);
    EXPECT_EQ(controller.demandTerminalCount(), 0U);

    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for([&] { return !controller.inFlight(); }, 10000));
    // Old completion may warm row 0 cache but must not mark terminals for the new demand.
    EXPECT_EQ(controller.demandSatisfiedCount(), 0U);
    ASSERT_TRUE(wait_for(
        [&] { return controller.demandQuiescent() && !model.thumbnail(1).isNull(); }, 30000));
    EXPECT_EQ(model.thumbnail(1).pixelColor(0, 0), QColor(Qt::blue));
    EXPECT_FALSE(model.thumbnail(0).isNull());
}

TEST(StudioImportThumbnailScheduler, InFlightRowIsNotDoublePendingUnderSameDemand)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("once.png");
    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::cyan);
        ASSERT_TRUE(image.save(path, "PNG"));
    }
    ImportCandidateListModel model;
    ImportCandidate candidate;
    candidate.source_path = path.toStdString();
    candidate.display_name = "once.png";
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
    controller.ensure(0);
    controller.setViewportDemand({0}, 0, 0); // idempotent
    EXPECT_EQ(controller.pendingCount(), 0U);
    EXPECT_EQ(controller.dispatchedCount(), 1U);
    gate_promise.set_value();
    controller.clearDecodeGate();
    ASSERT_TRUE(wait_for(
        [&] { return controller.demandQuiescent() && !model.thumbnail(0).isNull(); }, 30000));
    EXPECT_EQ(controller.dispatchedCount(), 1U);
}

} // namespace ravo
