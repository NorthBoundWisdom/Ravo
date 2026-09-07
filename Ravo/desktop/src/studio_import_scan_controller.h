#pragma once

#include <cstdint>
#include <optional>

#include <QObject>

#include "ravo/foundation/cancellation.h"

namespace ravo
{

// Owns Import source-scan generation, busy flag, progress counters, and cancel token.
// Publication of candidate batches remains on the Presenter UI thread callbacks.
class StudioImportScanController final : public QObject
{
    Q_OBJECT

public:
    explicit StudioImportScanController(QObject *parent = nullptr);

    // Cancel prior scan token, assign a fresh token, bump generation, mark active.
    [[nodiscard]] CancellationToken begin(const char *reason);
    void finish();                    // clear active when matching generation completes
    void abandon(const char *reason); // bump generation + clear active (page close / replace)
    void bumpGeneration(const char *reason); // invalidate late callbacks without clearing progress

    [[nodiscard]] bool matches(std::uint64_t generation) const noexcept
    {
        return generation == generation_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }
    [[nodiscard]] int completed() const noexcept
    {
        return completed_;
    }
    [[nodiscard]] int total() const noexcept
    {
        return total_;
    }
    [[nodiscard]] int duplicateCount() const noexcept
    {
        return duplicate_count_;
    }
    [[nodiscard]] const std::optional<std::int64_t> &catalogRevision() const noexcept
    {
        return catalog_revision_;
    }

    void setProgress(int completed, int total, int duplicates);
    void setTotal(int total);
    void setCatalogRevision(std::optional<std::int64_t> revision);
    void resetProgress();

    [[nodiscard]] CancellationToken token() const
    {
        return operation_.token();
    }
    void cancel(const char *reason);

private:
    CancellationSource operation_;
    std::uint64_t generation_ = 0;
    bool active_ = false;
    int completed_ = 0;
    int total_ = 0;
    int duplicate_count_ = 0;
    std::optional<std::int64_t> catalog_revision_;
};

} // namespace ravo
