#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
class ImportCandidateListModel;

// Owns Import source-scan generation, busy/progress, cancel token, and scan orchestration.
// Candidate model mutation and page signals stay on the UI-thread Host callbacks.
class StudioImportScanController final : public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        QObject *callback_receiver = nullptr;
        SerialExecutor *executor = nullptr;
        std::function<CatalogService *()> service;
        std::function<bool()> page_open;
        std::function<bool()> work_active;
        std::function<ImportCandidateListModel *()> model;
        std::function<QString()> source_root;
        std::function<bool(const QString &root)> recursive_for_root;
        std::function<void()> emit_page_changed;
        std::function<void(QString)> set_error;
        std::function<void()> prepare_thumbnails_for_rescan; // cancel/reset/clear pending
        std::function<void()> clear_preflight_active;
    };

    explicit StudioImportScanController(Host host, QObject *parent = nullptr);
    // Legacy construction for tests that only need generation/progress ownership.
    explicit StudioImportScanController(QObject *parent = nullptr);

    // Cancel prior scan token, assign a fresh token, bump generation, mark active.
    [[nodiscard]] CancellationToken begin(const char *reason);
    void finish();                    // clear active when matching generation completes
    void abandon(const char *reason); // bump generation + clear active (page close / replace)
    void bumpGeneration(const char *reason); // invalidate late callbacks without clearing progress

    // Orchestrate source scan on the catalog executor; UI publication via Host.
    void startRescan();

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
    Host host_;
    CancellationSource operation_;
    std::uint64_t generation_ = 0;
    bool active_ = false;
    int completed_ = 0;
    int total_ = 0;
    int duplicate_count_ = 0;
    std::optional<std::int64_t> catalog_revision_;
};

} // namespace ravo
