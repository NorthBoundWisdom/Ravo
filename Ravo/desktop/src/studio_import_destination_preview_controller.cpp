#include "studio_import_destination_preview_controller.h"

#include <atomic>
#include <memory>

#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>

#include "ravo/foundation/error.h"
#include "studio_qt.h"

namespace ravo
{

StudioImportDestinationPreviewController::StudioImportDestinationPreviewController(Host host,
                                                                                   QObject *parent)
    : QObject(parent)
    , host_(std::move(host))
{
    timer_.setSingleShot(true);
    timer_.setInterval(0); // Coalesce one UI turn before scheduling the projection.
    connect(&timer_, &QTimer::timeout, this, &StudioImportDestinationPreviewController::start);
}

StudioImportDestinationPreviewController::~StudioImportDestinationPreviewController()
{
    shutdown();
}

void StudioImportDestinationPreviewController::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    timer_.stop();
    operation_.cancel("destination_preview_shutdown");
    active_ = false;
    folders_.clear();
    tree_folders_.clear();
    emit foldersChanged();
    emit changed();
}

void StudioImportDestinationPreviewController::invalidateCacheKey()
{
    key_.clear();
}

void StudioImportDestinationPreviewController::clearPublished()
{
    if (stopped_)
        return;
    timer_.stop();
    static_cast<void>(operation_.invalidate("destination_preview_cleared"));
    static_cast<void>(operation_.begin());
    key_.clear();
    folders_.clear();
    tree_folders_.clear();
    error_.clear();
    active_ = false;
    emit foldersChanged();
    emit changed();
}

void StudioImportDestinationPreviewController::refresh()
{
    if (stopped_)
        return;
    QByteArray next;
    if (host_.can_schedule && host_.can_schedule())
        next = host_.build_key ? host_.build_key() : QByteArray{};
    if (next == key_)
        return;
    key_ = std::move(next);
    static_cast<void>(operation_.invalidate("destination_preview_changed"));
    static_cast<void>(operation_.begin());
    timer_.stop();
    // Keep the last published projection while a replacement is computed. Status
    // changes must not repeatedly reset the browser or take over the window.
    if (key_.isEmpty())
    {
        folders_.clear();
        tree_folders_.clear();
        emit foldersChanged();
    }
    error_.clear();
    active_ = !key_.isEmpty();
    if (active_)
        timer_.start();
    emit changed();
}

void StudioImportDestinationPreviewController::publishResult(
    const std::uint64_t generation, Result<ImportDestinationPreview> preview, const bool complete)
{
    if (stopped_ || !operation_.accepts(generation))
        return;
    if (host_.page_open && !host_.page_open())
        return;
    active_ = !complete && preview.has_value();
    folders_.clear();
    tree_folders_.clear();
    if (!preview)
        error_ = qstring_from_utf8(preview.error().message);
    else
    {
        tree_folders_ = std::move(preview.value().folders);
        for (const auto &folder : tree_folders_)
            folders_.push_back(QVariantMap{
                {QStringLiteral("path"), qstring_from_utf8(folder.path)},
                {QStringLiteral("name"), qstring_from_utf8(folder.name)},
                {QStringLiteral("depth"), static_cast<int>(folder.depth)},
                {QStringLiteral("photoCount"), static_cast<qulonglong>(folder.photo_count)},
                {QStringLiteral("willCreate"), folder.will_create},
                {QStringLiteral("secondCopy"), folder.second_copy}});
    }
    emit foldersChanged();
    emit changed();
}

void StudioImportDestinationPreviewController::start()
{
    if (stopped_ || !active_ || host_.executor == nullptr || host_.callback_receiver == nullptr)
        return;
    if (!host_.build_request)
        return;
    auto request = host_.build_request();
    request.cancellation = operation_.token();
    const auto generation = operation_.revision();
    const bool queued = host_.executor->post(
        [this, request = std::move(request), generation]
        {
            auto *service = host_.service ? host_.service() : nullptr;
            const QPointer<StudioImportDestinationPreviewController> self(this);
            // At most one bounded partial snapshot may wait on the UI queue.
            // The final result is always delivered after the queued partial.
            const auto pending = std::make_shared<std::atomic_bool>(false);
            const auto progress = [self, receiver = host_.callback_receiver, generation,
                                   pending](const ImportDestinationPreview &partial)
            {
                if (pending->exchange(true))
                    return;
                QMetaObject::invokeMethod(
                    receiver,
                    [self, generation, pending, partial]() mutable
                    {
                        pending->store(false);
                        if (self)
                            self->publishResult(generation, std::move(partial), false);
                    },
                    Qt::QueuedConnection);
            };
            auto preview = service != nullptr ?
                               service->import().preview_import_destinations(request, progress) :
                               Result<ImportDestinationPreview>{
                                   make_error(ErrorCode::kIo, "Catalog session is closed")};
            QMetaObject::invokeMethod(
                host_.callback_receiver,
                [self, generation, preview = std::move(preview)]() mutable
                {
                    if (!self)
                        return;
                    self->publishResult(generation, std::move(preview));
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        active_ = false;
        error_ = QStringLiteral("Import destination preview worker is stopped.");
        emit changed();
    }
}

} // namespace ravo
