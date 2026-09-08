#include "studio_import_thumbnail_controller.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/services/import_thumbnail.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{
[[nodiscard]] QImage import_thumbnail_image(const RasterBuffer &raster)
{
    if (raster.width == 0 || raster.height == 0 ||
        raster.srgb.size() < static_cast<std::size_t>(raster.width) * raster.height * 3U)
        return {};
    QImage image(static_cast<int>(raster.width), static_cast<int>(raster.height),
                 QImage::Format_RGB888);
    const auto row_bytes = static_cast<std::size_t>(raster.width) * 3U;
    for (std::uint32_t row = 0; row < raster.height; ++row)
        std::memcpy(image.scanLine(static_cast<int>(row)),
                    raster.srgb.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes);
    return image;
}
} // namespace

StudioImportThumbnailController::StudioImportThumbnailController(Host host, QObject *parent)
    : QObject(parent)
    , host_(std::move(host))
{
}

StudioImportThumbnailController::~StudioImportThumbnailController()
{
    shutdown();
}

bool StudioImportThumbnailController::gatesAllowWork() const
{
    if (stopped_)
        return false;
    // Cancelled operations stay idle until resetOperation() + new demand.
    if (operation_.token().is_cancellation_requested())
        return false;
    if (!host_.page_open || !host_.page_open())
        return false;
    if (host_.work_active && host_.work_active())
        return false;
    if (host_.preflight_active && host_.preflight_active())
        return false;
    return true;
}

const std::vector<StudioImportThumbnailController::ObservationEvent>
    StudioImportThumbnailController::kEmptyObservations{};

bool StudioImportThumbnailController::observationsEnabled() const noexcept
{
    return observation_sink_ != nullptr || diagnostic_ring_enabled_;
}

void StudioImportThumbnailController::bumpObservationCounter(ObservationEvent::Kind kind) noexcept
{
    switch (kind)
    {
    case ObservationEvent::Kind::kWakeupScheduled:
        ++wakeup_scheduled_;
        break;
    case ObservationEvent::Kind::kWakeupFired:
        ++wakeup_fired_;
        break;
    case ObservationEvent::Kind::kDispatched:
        ++dispatched_count_;
        break;
    case ObservationEvent::Kind::kCompleted:
        ++completed_count_;
        break;
    case ObservationEvent::Kind::kDiscarded:
        ++discarded_count_;
        break;
    case ObservationEvent::Kind::kReplenished:
        ++replenished_count_;
        break;
    }
}

void StudioImportThumbnailController::record(ObservationEvent event)
{
    bumpObservationCounter(event.kind);
    // Production trails must not retain source paths by default.
    event.identity.source_path.clear();
    if (!observationsEnabled())
        return;
    if (observation_sink_ != nullptr)
        observation_sink_->push_back(event);
    if (!diagnostic_ring_enabled_)
        return;
    if (diagnostic_ring_cap_ == 0)
    {
        ++dropped_observations_;
        return;
    }
    if (diagnostic_ring_.size() >= diagnostic_ring_cap_)
    {
        ++dropped_observations_;
        return;
    }
    diagnostic_ring_.push_back(std::move(event));
}

void StudioImportThumbnailController::setObservationSink(
    std::vector<ObservationEvent> *sink) noexcept
{
    observation_sink_ = sink;
}

void StudioImportThumbnailController::enableDiagnosticRing(std::size_t max_events)
{
    diagnostic_ring_enabled_ = true;
    diagnostic_ring_cap_ = max_events;
    if (diagnostic_ring_.size() > diagnostic_ring_cap_)
    {
        dropped_observations_ += diagnostic_ring_.size() - diagnostic_ring_cap_;
        diagnostic_ring_.resize(diagnostic_ring_cap_);
    }
}

void StudioImportThumbnailController::disableDiagnosticRing()
{
    diagnostic_ring_enabled_ = false;
    diagnostic_ring_cap_ = 0;
    diagnostic_ring_.clear();
}

void StudioImportThumbnailController::clearObservations()
{
    if (observation_sink_ != nullptr)
        observation_sink_->clear();
    diagnostic_ring_.clear();
    dropped_observations_ = 0;
    wakeup_scheduled_ = 0;
    wakeup_fired_ = 0;
    dispatched_count_ = 0;
    completed_count_ = 0;
    discarded_count_ = 0;
    replenished_count_ = 0;
    pending_high_water_ = pending_rows_.size();
}

std::size_t StudioImportThumbnailController::observationTrailSize() const noexcept
{
    if (observation_sink_ != nullptr)
        return observation_sink_->size();
    if (diagnostic_ring_enabled_)
        return diagnostic_ring_.size();
    return 0;
}

const std::vector<StudioImportThumbnailController::ObservationEvent> &
StudioImportThumbnailController::observations() const noexcept
{
    if (observation_sink_ != nullptr)
        return *observation_sink_;
    if (diagnostic_ring_enabled_)
        return diagnostic_ring_;
    return kEmptyObservations;
}

std::vector<int> StudioImportThumbnailController::dispatchedRows() const
{
    std::vector<int> rows;
    for (const auto &event : observations())
        if (event.kind == ObservationEvent::Kind::kDispatched)
            rows.push_back(event.identity.row);
    return rows;
}

std::vector<int> StudioImportThumbnailController::completedRows() const
{
    std::vector<int> rows;
    for (const auto &event : observations())
        if (event.kind == ObservationEvent::Kind::kCompleted)
            rows.push_back(event.identity.row);
    return rows;
}

std::vector<int> StudioImportThumbnailController::discardedRows() const
{
    std::vector<int> rows;
    for (const auto &event : observations())
        if (event.kind == ObservationEvent::Kind::kDiscarded)
            rows.push_back(event.identity.row);
    return rows;
}

void StudioImportThumbnailController::installDecodeGate(std::shared_future<void> release)
{
    std::lock_guard lock(gate_mutex_);
    decode_gate_ = std::move(release);
}

void StudioImportThumbnailController::clearDecodeGate()
{
    std::lock_guard lock(gate_mutex_);
    decode_gate_.reset();
}

void StudioImportThumbnailController::clearDemandTerminals()
{
    demand_terminals_.clear();
}

bool StudioImportThumbnailController::hasDemandTerminal(const int row) const noexcept
{
    return demand_terminals_.find(row) != demand_terminals_.end();
}

void StudioImportThumbnailController::markDemandTerminal(const int row,
                                                         const DemandTerminal terminal)
{
    if (row < 0)
        return;
    demand_terminals_[row] = terminal;
}

std::size_t StudioImportThumbnailController::demandDecodeSlotsUsed() const noexcept
{
    std::size_t used = pending_rows_.size() + (in_flight_ ? 1U : 0U);
    for (const auto &entry : demand_terminals_)
    {
        if (entry.second == DemandTerminal::kSatisfied || entry.second == DemandTerminal::kFailed)
            ++used;
    }
    return used;
}

std::size_t StudioImportThumbnailController::demandSatisfiedCount() const noexcept
{
    std::size_t count = 0;
    for (const auto &entry : demand_terminals_)
        if (entry.second == DemandTerminal::kSatisfied)
            ++count;
    return count;
}

std::size_t StudioImportThumbnailController::demandFailedCount() const noexcept
{
    std::size_t count = 0;
    for (const auto &entry : demand_terminals_)
        if (entry.second == DemandTerminal::kFailed)
            ++count;
    return count;
}

std::size_t StudioImportThumbnailController::demandCapacityDeferredCount() const noexcept
{
    std::size_t count = 0;
    for (const auto &entry : demand_terminals_)
        if (entry.second == DemandTerminal::kCapacityDeferred)
            ++count;
    return count;
}

bool StudioImportThumbnailController::demandQuiescent() const noexcept
{
    return !in_flight_ && pending_rows_.empty() && !kick_scheduled_;
}

std::size_t StudioImportThumbnailController::physicalOutstandingCount() const noexcept
{
    return physical_outstanding_;
}

bool StudioImportThumbnailController::physicalDrained() const noexcept
{
    return physical_outstanding_ == 0 && demandQuiescent();
}

void StudioImportThumbnailController::scheduleKick()
{
    if (stopped_)
        return;
    if (kick_scheduled_)
        return;
    kick_scheduled_ = true;
    record(ObservationEvent{ObservationEvent::Kind::kWakeupScheduled, {}, {}, {}});
    QTimer::singleShot(0, this, &StudioImportThumbnailController::kick);
}

void StudioImportThumbnailController::ensure(const int row)
{
    if (!gatesAllowWork() || !host_.model || row < 0 || row >= host_.model->rowCount())
        return;
    // Residency / terminal / in-flight / pending idempotence must precede capacity.
    // Otherwise a repeat ensure at a full pending set marks an already-queued row
    // kCapacityDeferred, clears loading, and lets kick drop it via hasDemandTerminal.
    if (hasDemandTerminal(row) || !host_.model->thumbnail(row).isNull())
        return;
    if (in_flight_ && in_flight_row_ == row)
        return;
    if (pending_rows_.count(row) > 0)
        return;
    // Legacy ensure must not re-promote off-viewport rows over active demand.
    if (!visible_demand_.empty() || !prefetch_demand_.empty() || current_row_ >= 0)
    {
        const bool in_demand = row == current_row_ || visible_demand_.count(row) > 0 ||
                               prefetch_demand_.count(row) > 0;
        if (!in_demand)
            return;
    }
    if (demandDecodeSlotsUsed() >= kDemandDecodeBudget)
    {
        markDemandTerminal(row, DemandTerminal::kCapacityDeferred);
        host_.model->setThumbnailLoading(row, false);
        return;
    }
    pending_rows_.insert(row);
    host_.model->setThumbnailLoading(row, true);
    trimPendingToCap();
    pending_high_water_ = std::max(pending_high_water_, pending_rows_.size());
    scheduleKick();
}

void StudioImportThumbnailController::setViewportDemand(const std::vector<int> &visible_rows,
                                                        const int prefetch_rows,
                                                        const int current_row)
{
    if (stopped_)
        return;
    std::set<int> next_visible;
    for (const int row : visible_rows)
        if (row >= 0)
            next_visible.insert(row);
    std::set<int> next_prefetch;
    if (!visible_rows.empty() && prefetch_rows > 0 && host_.model)
    {
        const int last = *std::max_element(visible_rows.begin(), visible_rows.end());
        for (int row = last + 1; row <= last + prefetch_rows && row < host_.model->rowCount();
             ++row)
            next_prefetch.insert(row);
    }
    // Identical normalized demand is idempotent: do not clear terminals or re-decode.
    if (current_row == current_row_ && next_visible == visible_demand_ &&
        next_prefetch == prefetch_demand_)
        return;
    ++demand_generation_;
    clearDemandTerminals();
    visible_demand_ = std::move(next_visible);
    prefetch_demand_ = std::move(next_prefetch);
    current_row_ = current_row;
    // Replace pending with current demand only (drop scrolled-away rows). Keep an
    // in-flight row that remains in demand from being double-counted as pending.
    if (host_.model)
    {
        for (const int row : pending_rows_)
            host_.model->setThumbnailLoading(row, false);
    }
    pending_rows_.clear();
    replenishPendingFromDemand();
    scheduleKick();
}

void StudioImportThumbnailController::replenishPendingFromDemand()
{
    if (!host_.model)
        return;
    auto consider = [&](const int row)
    {
        if (row < 0 || row >= host_.model->rowCount())
            return;
        if (hasDemandTerminal(row))
            return;
        if (!host_.model->thumbnail(row).isNull())
        {
            // Already resident for this demand — treat as satisfied without re-decode.
            markDemandTerminal(row, DemandTerminal::kSatisfied);
            host_.model->setThumbnailLoading(row, false);
            return;
        }
        if (pending_rows_.count(row) > 0)
            return;
        if (in_flight_ && in_flight_row_ == row)
            return;
        if (demandDecodeSlotsUsed() >= kDemandDecodeBudget)
        {
            markDemandTerminal(row, DemandTerminal::kCapacityDeferred);
            host_.model->setThumbnailLoading(row, false);
            return;
        }
        pending_rows_.insert(row);
        host_.model->setThumbnailLoading(row, true);
    };
    if (current_row_ >= 0)
        consider(current_row_);
    for (const int row : visible_demand_)
        consider(row);
    for (const int row : prefetch_demand_)
        consider(row);
    trimPendingToCap();
    pending_high_water_ = std::max(pending_high_water_, pending_rows_.size());
    if (observationsEnabled())
    {
        record(ObservationEvent{ObservationEvent::Kind::kReplenished,
                                {},
                                {},
                                "pending=" + std::to_string(pending_rows_.size())});
    }
    else
    {
        bumpObservationCounter(ObservationEvent::Kind::kReplenished);
    }
}

void StudioImportThumbnailController::trimPendingToCap()
{
    // Drop lowest-priority (highest prefetch, then highest visible) first; keep current.
    while (pending_rows_.size() > kPendingHardCap)
    {
        int drop = -1;
        for (auto it = prefetch_demand_.rbegin(); it != prefetch_demand_.rend(); ++it)
        {
            if (pending_rows_.count(*it) && *it != current_row_)
            {
                drop = *it;
                break;
            }
        }
        if (drop < 0)
        {
            for (auto it = visible_demand_.rbegin(); it != visible_demand_.rend(); ++it)
            {
                if (pending_rows_.count(*it) && *it != current_row_)
                {
                    drop = *it;
                    break;
                }
            }
        }
        if (drop < 0)
        {
            // Fall back: drop highest row number that is not current.
            for (auto it = pending_rows_.rbegin(); it != pending_rows_.rend(); ++it)
            {
                if (*it != current_row_)
                {
                    drop = *it;
                    break;
                }
            }
        }
        if (drop < 0)
            break;
        pending_rows_.erase(drop);
        if (host_.model)
            host_.model->setThumbnailLoading(drop, false);
    }
}

void StudioImportThumbnailController::kick()
{
    kick_scheduled_ = false;
    record(ObservationEvent{ObservationEvent::Kind::kWakeupFired, {}, {}, {}});
    if (in_flight_ || !gatesAllowWork())
        return;
    while (!pending_rows_.empty())
    {
        // Priority: current, then ascending visible, then ascending prefetch, then remaining.
        int row = -1;
        if (current_row_ >= 0 && pending_rows_.count(current_row_))
            row = current_row_;
        else
        {
            for (const int candidate : visible_demand_)
            {
                if (pending_rows_.count(candidate))
                {
                    row = candidate;
                    break;
                }
            }
            if (row < 0)
            {
                for (const int candidate : prefetch_demand_)
                {
                    if (pending_rows_.count(candidate))
                    {
                        row = candidate;
                        break;
                    }
                }
            }
            if (row < 0)
                row = *pending_rows_.begin();
        }
        pending_rows_.erase(row);
        if (!host_.model || row < 0 || row >= host_.model->rowCount() || hasDemandTerminal(row) ||
            !host_.model->thumbnail(row).isNull())
        {
            if (host_.model && row >= 0 && row < host_.model->rowCount() &&
                !host_.model->thumbnail(row).isNull() && !hasDemandTerminal(row))
            {
                markDemandTerminal(row, DemandTerminal::kSatisfied);
                host_.model->setThumbnailLoading(row, false);
            }
            record(ObservationEvent{
                ObservationEvent::Kind::kDiscarded,
                RequestIdentity{row,
                                host_.model ? host_.model->generation() : 0,
                                host_.scan_generation ? host_.scan_generation() : 0,
                                demand_generation_,
                                {}},
                DiscardReason::kSkippedReady, "already ready"});
            continue;
        }
        start(row);
        return;
    }
}

void StudioImportThumbnailController::clearPending()
{
    if (host_.model)
    {
        for (const int row : pending_rows_)
            host_.model->setThumbnailLoading(row, false);
    }
    pending_rows_.clear();
}

void StudioImportThumbnailController::resetSourceSession()
{
    clearPending();
    clearDemandTerminals();
    visible_demand_.clear();
    prefetch_demand_.clear();
    current_row_ = -1;
    // Release the UI admission latch so ensure/setViewportDemand can admit new demand even
    // while a cancelled worker is still draining. Do not clear physical_outstanding_: start()
    // refuses additional executor posts until finishUi releases the physical slot. finishUi
    // matches in_flight_demand_generation_ before clearing a newer admission latch.
    in_flight_ = false;
    in_flight_row_ = -1;
    in_flight_demand_generation_ = 0;
    ++demand_generation_;
}

void StudioImportThumbnailController::cancel(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
}

void StudioImportThumbnailController::resetOperation()
{
    operation_ = CancellationSource{};
}

void StudioImportThumbnailController::shutdown()
{
    if (stopped_)
        return;
    // Request cancel before flipping UI-only stopped_ so worker gate waits observe the
    // thread-safe token rather than racing on stopped_.
    cancel("thumbnail_controller_shutdown");
    stopped_ = true;
    clearPending();
    clearDemandTerminals();
    visible_demand_.clear();
    prefetch_demand_.clear();
    current_row_ = -1;
    in_flight_ = false;
    in_flight_row_ = -1;
    in_flight_demand_generation_ = 0;
    physical_outstanding_ = 0;
    kick_scheduled_ = false;
    clearDecodeGate();
    observation_sink_ = nullptr;
    disableDiagnosticRing();
    try
    {
        executor_.submit([this] { engine_.reset(); });
    }
    catch (const std::runtime_error &)
    {
        // Already stopped during an earlier shutdown path.
    }
    executor_.request_stop();
    executor_.wait();
}

void StudioImportThumbnailController::finishUi(RequestIdentity identity, QImage image,
                                               std::optional<TaskError> error,
                                               CancellationToken token)
{
    // Physical slot is independent of the UI admission latch.
    if (physical_outstanding_ > 0)
        --physical_outstanding_;
    // Only the request that currently owns the latch may release it. A stale completion
    // after resetSourceSession must not drop a newer in-flight admission for the same row.
    const bool owns_inflight = in_flight_ && identity.row == in_flight_row_ &&
                               identity.demand_generation == in_flight_demand_generation_;
    if (owns_inflight)
    {
        in_flight_ = false;
        in_flight_row_ = -1;
        in_flight_demand_generation_ = 0;
    }
    if (host_.model)
    {
        const bool same_row_identity =
            host_.model->generation() == identity.model_generation &&
            host_.model->sourcePath(identity.row) == identity.source_path;
        if (owns_inflight || same_row_identity)
            host_.model->setThumbnailLoading(identity.row, false);
    }
    if (stopped_)
    {
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kStopped, "stopped"});
        return;
    }
    const bool page_ok = host_.page_open && host_.page_open();
    const bool work_ok = !(host_.work_active && host_.work_active());
    const auto current_generation = host_.scan_generation ? host_.scan_generation() : 0U;
    bool terminal_without_retry = false;
    if (!page_ok)
    {
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kPageClosed, "page"});
    }
    else if (!work_ok)
    {
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kWorkActive, "work"});
    }
    else if (identity.scan_generation != current_generation)
    {
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kStaleGeneration, "scan_generation"});
    }
    else if (identity.demand_generation != demand_generation_)
    {
        // Warm cache for a still-matching row without touching the new demand terminals.
        if (host_.model && host_.model->generation() == identity.model_generation &&
            host_.model->sourcePath(identity.row) == identity.source_path && !image.isNull() &&
            !error.has_value())
            host_.model->finishThumbnail(identity.row, image, {});
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kStaleGeneration, "demand_generation"});
    }
    else if (!token.check())
    {
        // Cancelled requests must not self-retry; a new operation/demand is required.
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kTokenCancelled, "token"});
        terminal_without_retry = true;
    }
    else if (!host_.model || host_.model->generation() != identity.model_generation ||
             host_.model->sourcePath(identity.row) != identity.source_path)
    {
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kSourceMismatch, "source"});
    }
    else
    {
        host_.model->finishThumbnail(identity.row, std::move(image), std::move(error));
        if (error.has_value())
            markDemandTerminal(identity.row, DemandTerminal::kFailed);
        else
            markDemandTerminal(identity.row, DemandTerminal::kSatisfied);
        record(ObservationEvent{ObservationEvent::Kind::kCompleted, identity, {}, {}});
    }
    if (terminal_without_retry)
    {
        clearPending();
        return;
    }
    replenishPendingFromDemand();
    scheduleKick();
}

void StudioImportThumbnailController::start(const int row)
{
    if (stopped_ || !host_.model)
        return;
    const QString source = host_.model->sourcePath(row);
    if (source.isEmpty())
        return;
    // Bound executor work across source-session replacement: while a prior physical post is
    // still draining, keep the latest demand pending instead of posting unbounded tasks.
    if (physical_outstanding_ > 0)
    {
        if (pending_rows_.insert(row).second)
            host_.model->setThumbnailLoading(row, true);
        pending_high_water_ = std::max(pending_high_water_, pending_rows_.size());
        return;
    }
    RequestIdentity identity;
    identity.row = row;
    identity.model_generation = host_.model->generation();
    identity.scan_generation = host_.scan_generation ? host_.scan_generation() : 0U;
    identity.demand_generation = demand_generation_;
    identity.source_path = source;
    const auto token = operation_.token();
    in_flight_ = true;
    in_flight_row_ = row;
    in_flight_demand_generation_ = identity.demand_generation;
    ++physical_outstanding_;
    record(ObservationEvent{ObservationEvent::Kind::kDispatched, identity, {}, {}});
    const bool queued = executor_.post(
        [this, identity, token]()
        {
            {
                std::optional<std::shared_future<void>> gate;
                {
                    std::lock_guard lock(gate_mutex_);
                    gate = decode_gate_;
                }
                // Interruptible wait via thread-safe cancellation token — never read UI-only
                // stopped_ from the worker (data race). Shutdown/cancel both request cancel.
                while (gate)
                {
                    if (token.is_cancellation_requested())
                        break;
                    if (gate->wait_for(std::chrono::milliseconds(10)) == std::future_status::ready)
                        break;
                }
            }
            const auto decode = [&]() -> Result<RasterBuffer>
            {
                if (auto active = token.check(); !active)
                    return active.error();
                if (!engine_)
                {
                    auto created = EngineFacade::create_phase1();
                    if (!created)
                        return created.error();
                    engine_ = std::move(created).value();
                }
                const QtRasterDecoder raster;
                return decode_import_thumbnail(*engine_, raster,
                                               utf8_from_qstring(identity.source_path), token);
            };
            auto decoded = decode();
            QImage image;
            std::optional<TaskError> error;
            if (decoded)
                image = import_thumbnail_image(decoded.value());
            else
                error = decoded.error();
            auto *receiver =
                host_.callback_receiver ? host_.callback_receiver : static_cast<QObject *>(this);
            const QPointer<StudioImportThumbnailController> self(this);
            const bool invoked = QMetaObject::invokeMethod(
                receiver,
                [self, identity, token, error = std::move(error),
                 image = std::move(image)]() mutable
                {
                    if (!self)
                        return;
                    self->finishUi(std::move(identity), std::move(image), std::move(error), token);
                },
                Qt::QueuedConnection);
            static_cast<void>(invoked);
        });
    if (!queued)
    {
        in_flight_ = false;
        in_flight_row_ = -1;
        in_flight_demand_generation_ = 0;
        if (physical_outstanding_ > 0)
            --physical_outstanding_;
        record(ObservationEvent{ObservationEvent::Kind::kDiscarded, identity,
                                DiscardReason::kPostRejected, "post"});
        if (host_.set_error)
            host_.set_error(QStringLiteral("Import thumbnail worker is stopped."));
        replenishPendingFromDemand();
    }
}

} // namespace ravo
