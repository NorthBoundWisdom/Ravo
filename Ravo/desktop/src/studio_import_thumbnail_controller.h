#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"

namespace ravo
{
class ImportCandidateListModel;

// Desktop-private owner for Import candidate thumbnail decode work.
class StudioImportThumbnailController final : public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        ImportCandidateListModel *model = nullptr;
        QObject *callback_receiver = nullptr; // UI-thread receiver for completions
        std::function<bool()> page_open;
        std::function<bool()> work_active;
        std::function<bool()> preflight_active;
        std::function<std::uint64_t()> scan_generation;
        std::function<void(QString)> set_error;
    };

    enum class DiscardReason : std::uint8_t
    {
        kStaleGeneration,
        kPageClosed,
        kWorkActive,
        kTokenCancelled,
        kSourceMismatch,
        kStopped,
        kPostRejected,
        kSkippedReady,
    };

    struct RequestIdentity
    {
        int row = -1;
        std::uint64_t model_generation = 0;
        std::uint64_t scan_generation = 0;
        QString source_path;
    };

    struct ObservationEvent
    {
        enum class Kind : std::uint8_t
        {
            kWakeupScheduled,
            kWakeupFired,
            kDispatched,
            kCompleted,
            kDiscarded,
            kReplenished,
        };
        Kind kind = Kind::kWakeupScheduled;
        RequestIdentity identity;
        DiscardReason discard = DiscardReason::kStopped;
        std::string detail;
    };

    explicit StudioImportThumbnailController(Host host, QObject *parent = nullptr);
    ~StudioImportThumbnailController() override;

    static constexpr std::size_t kPendingHardCap = 256;

    void ensure(int row); // legacy delegate path; prefer setViewportDemand
    // visible_rows = currently on-screen; current_row elevates one cell; prefetch after max visible.
    void setViewportDemand(const std::vector<int> &visible_rows, int prefetch_rows = 2,
                           int current_row = -1);
    void kick();
    void clearPending();
    void cancel(const char *reason);
    void resetOperation();
    void shutdown(); // cancel, reset engine on executor, stop+wait

    [[nodiscard]] SerialExecutor &executor() noexcept
    {
        return executor_;
    }
    [[nodiscard]] bool inFlight() const noexcept
    {
        return in_flight_;
    }
    [[nodiscard]] bool stopped() const noexcept
    {
        return stopped_;
    }
    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        return pending_rows_.size();
    }
    [[nodiscard]] std::size_t pendingHardCap() const noexcept
    {
        return kPendingHardCap;
    }
    [[nodiscard]] std::size_t visibleDemandCount() const noexcept
    {
        return visible_demand_.size();
    }
    [[nodiscard]] std::size_t prefetchDemandCount() const noexcept
    {
        return prefetch_demand_.size();
    }

    // Deterministic test checkpoints (UI thread). Observation is append-only until clear.
    void clearObservations();
    [[nodiscard]] std::uint64_t wakeupScheduledCount() const noexcept
    {
        return wakeup_scheduled_;
    }
    [[nodiscard]] std::uint64_t wakeupFiredCount() const noexcept
    {
        return wakeup_fired_;
    }
    [[nodiscard]] std::size_t pendingHighWater() const noexcept
    {
        return pending_high_water_;
    }
    [[nodiscard]] const std::vector<ObservationEvent> &observations() const noexcept
    {
        return observations_;
    }
    [[nodiscard]] std::vector<int> dispatchedRows() const;
    [[nodiscard]] std::vector<int> completedRows() const;
    [[nodiscard]] std::vector<int> discardedRows() const;

    // Hang decode(s) on the worker until the shared future becomes ready. Failure paths must release.
    void installDecodeGate(std::shared_future<void> release);
    void clearDecodeGate();

private:
    void scheduleKick();
    void start(int row);
    void trimPendingToCap();
    void replenishPendingFromDemand();
    [[nodiscard]] bool gatesAllowWork() const;
    void record(ObservationEvent event);
    void finishUi(RequestIdentity identity, QImage image, std::optional<TaskError> error,
                  CancellationToken token);

    Host host_;
    SerialExecutor executor_;
    std::optional<EngineFacade> engine_;
    CancellationSource operation_;
    std::set<int> pending_rows_; // ascending within admitted set
    std::set<int> visible_demand_;
    std::set<int> prefetch_demand_;
    int current_row_ = -1;
    bool in_flight_ = false;
    bool stopped_ = false;
    bool kick_scheduled_ = false;

    std::uint64_t wakeup_scheduled_ = 0;
    std::uint64_t wakeup_fired_ = 0;
    std::size_t pending_high_water_ = 0;
    std::vector<ObservationEvent> observations_;

    std::mutex gate_mutex_;
    std::optional<std::shared_future<void>> decode_gate_;
};

} // namespace ravo
