#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
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

    explicit StudioImportThumbnailController(Host host, QObject *parent = nullptr);
    ~StudioImportThumbnailController() override;

    static constexpr std::size_t kPendingHardCap = 256;

    void ensure(int row); // legacy delegate path; prefer setViewportDemand
    void setViewportDemand(const std::vector<int> &visible_rows, int prefetch_rows = 2);
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
    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        return pending_rows_.size();
    }
    [[nodiscard]] std::size_t pendingHardCap() const noexcept
    {
        return kPendingHardCap;
    }

private:
    void start(int row);

    Host host_;
    SerialExecutor executor_;
    std::optional<EngineFacade> engine_;
    CancellationSource operation_;
    void trimPendingToCap();
    std::set<int> pending_rows_; // ascending; visible demand refilled via setViewportDemand
    std::set<int> visible_demand_;
    bool in_flight_ = false;
    bool stopped_ = false;
};

} // namespace ravo
