#pragma once

#include <cstdint>
#include <functional>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

// Owns debounce timer, generation, cancel, cache key, and published preview rows.
class StudioImportDestinationPreviewController final : public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        QObject *callback_receiver = nullptr;
        SerialExecutor *executor = nullptr;
        std::function<CatalogService *()> service;
        std::function<bool()> page_open;
        std::function<bool()> can_schedule; // page/busy/mode/destination/selection gates
        std::function<QByteArray()> build_key;
        std::function<ImportRequest()> build_request;
    };

    explicit StudioImportDestinationPreviewController(Host host, QObject *parent = nullptr);
    ~StudioImportDestinationPreviewController() override;

    void refresh();            // recompute key; debounce start when non-empty
    void clearPublished();     // cancel + clear rows/error/active (draft replace / close)
    void invalidateCacheKey(); // force next refresh to recompute
    void shutdown();

    [[nodiscard]] const QVariantList &folders() const noexcept
    {
        return folders_;
    }
    [[nodiscard]] const QString &error() const noexcept
    {
        return error_;
    }
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }
    [[nodiscard]] const QByteArray &key() const noexcept
    {
        return key_;
    }
    [[nodiscard]] bool stopped() const noexcept
    {
        return stopped_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }

signals:
    void changed();

private:
    void start();
    void publishResult(std::uint64_t generation, Result<ImportDestinationPreview> preview);

    Host host_;
    QTimer timer_;
    CancellationSource operation_;
    QVariantList folders_;
    QString error_;
    QByteArray key_;
    std::uint64_t generation_ = 0;
    bool active_ = false;
    bool stopped_ = false;
};

} // namespace ravo
