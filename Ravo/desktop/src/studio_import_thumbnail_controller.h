#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
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
    // Per demand-generation decode budget matches the model thumbnail count cache.
    static constexpr std::size_t kDemandDecodeBudget = kPendingHardCap;

    // Resource contract (demand generation scoped):
    // - not-dispatched: in demand, no terminal yet, may enter pending
    // - in-flight: decode running for this generation
    // - satisfied / failed: decode finished; eviction may drop pixels but must not
    //   auto-retry within the same demand generation
    // - capacity-deferred: visible demand beyond decode/cache budget for this generation
    // Eviction expresses residency only. A new setViewportDemand clears terminals so
    // scroll-back can rebuild missing pixels under a fresh generation.
    enum class DemandTerminal : std::uint8_t
    {
        kSatisfied,
        kFailed,
        kCapacityDeferred,
    };

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
    [[nodiscard]] std::uint64_t demandGeneration() const noexcept
    {
        return demand_generation_;
    }
    [[nodiscard]] std::size_t demandTerminalCount() const noexcept
    {
        return demand_terminals_.size();
    }
    [[nodiscard]] std::size_t demandSatisfiedCount() const noexcept;
    [[nodiscard]] std::size_t demandFailedCount() const noexcept;
    [[nodiscard]] std::size_t demandCapacityDeferredCount() const noexcept;
    [[nodiscard]] bool demandQuiescent() const noexcept;

    // Deterministic test checkpoints (UI thread).
    // Production default keeps constant-space counters only. Full trails require an
    // explicit test-owned sink and/or a bounded diagnostic ring (with drop count).
    void clearObservations();
    void setObservationSink(std::vector<ObservationEvent> *sink) noexcept;
    void enableDiagnosticRing(std::size_t max_events);
    void disableDiagnosticRing();
    [[nodiscard]] std::uint64_t wakeupScheduledCount() const noexcept
    {
        return wakeup_scheduled_;
    }
    [[nodiscard]] std::uint64_t wakeupFiredCount() const noexcept
    {
        return wakeup_fired_;
    }
    [[nodiscard]] std::uint64_t dispatchedCount() const noexcept
    {
        return dispatched_count_;
    }
    [[nodiscard]] std::uint64_t completedCount() const noexcept
    {
        return completed_count_;
    }
    [[nodiscard]] std::uint64_t discardedCount() const noexcept
    {
        return discarded_count_;
    }
    [[nodiscard]] std::uint64_t replenishedCount() const noexcept
    {
        return replenished_count_;
    }
    [[nodiscard]] std::uint64_t droppedObservationCount() const noexcept
    {
        return dropped_observations_;
    }
    [[nodiscard]] std::size_t pendingHighWater() const noexcept
    {
        return pending_high_water_;
    }
    [[nodiscard]] std::size_t observationTrailSize() const noexcept;
    [[nodiscard]] const std::vector<ObservationEvent> &observations() const noexcept;
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
    void clearDemandTerminals();
    [[nodiscard]] bool hasDemandTerminal(int row) const noexcept;
    void markDemandTerminal(int row, DemandTerminal terminal);
    [[nodiscard]] std::size_t demandDecodeSlotsUsed() const noexcept;
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
    std::unordered_map<int, DemandTerminal> demand_terminals_;
    std::uint64_t demand_generation_ = 0;
    int current_row_ = -1;
    bool in_flight_ = false;
    bool stopped_ = false;
    bool kick_scheduled_ = false;

    std::uint64_t wakeup_scheduled_ = 0;
    std::uint64_t wakeup_fired_ = 0;
    std::uint64_t dispatched_count_ = 0;
    std::uint64_t completed_count_ = 0;
    std::uint64_t discarded_count_ = 0;
    std::uint64_t replenished_count_ = 0;
    std::uint64_t dropped_observations_ = 0;
    std::size_t pending_high_water_ = 0;
    std::vector<ObservationEvent> *observation_sink_ = nullptr; // test-owned; non-owning
    bool diagnostic_ring_enabled_ = false;
    std::size_t diagnostic_ring_cap_ = 0;
    std::vector<ObservationEvent> diagnostic_ring_;
    static const std::vector<ObservationEvent> kEmptyObservations;

    [[nodiscard]] bool observationsEnabled() const noexcept;
    void bumpObservationCounter(ObservationEvent::Kind kind) noexcept;

    std::mutex gate_mutex_;
    std::optional<std::shared_future<void>> decode_gate_;
};

} // namespace ravo
