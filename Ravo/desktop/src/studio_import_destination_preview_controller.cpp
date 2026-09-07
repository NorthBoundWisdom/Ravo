#include "studio_import_destination_preview_controller.h"

#include <QMetaObject>
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
    timer_.setInterval(250);
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
    static_cast<void>(operation_.cancel("destination_preview_shutdown"));
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
    static_cast<void>(operation_.cancel("destination_preview_cleared"));
    operation_ = CancellationSource{};
    ++generation_;
    key_.clear();
    folders_.clear();
    error_.clear();
    active_ = false;
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
    static_cast<void>(operation_.cancel("destination_preview_changed"));
    operation_ = CancellationSource{};
    ++generation_;
    timer_.stop();
    folders_.clear();
    error_.clear();
    active_ = !key_.isEmpty();
    if (active_)
        timer_.start();
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
    const auto generation = generation_;
    host_.executor->post(
        [this, request = std::move(request), generation]
        {
            auto *service = host_.service ? host_.service() : nullptr;
            auto preview = service != nullptr ? service->preview_import_destinations(request) :
                                                Result<ImportDestinationPreview>{make_error(
                                                    ErrorCode::kIo, "Catalog session is closed")};
            QMetaObject::invokeMethod(
                host_.callback_receiver,
                [this, generation, preview = std::move(preview)]() mutable
                {
                    if (stopped_ || generation != generation_)
                        return;
                    if (host_.page_open && !host_.page_open())
                        return;
                    active_ = false;
                    if (!preview)
                        error_ = qstring_from_utf8(preview.error().message);
                    else
                        for (const auto &folder : preview.value().folders)
                            folders_.push_back(QVariantMap{
                                {QStringLiteral("path"), qstring_from_utf8(folder.path)},
                                {QStringLiteral("name"), qstring_from_utf8(folder.name)},
                                {QStringLiteral("depth"), static_cast<int>(folder.depth)},
                                {QStringLiteral("photoCount"),
                                 static_cast<qulonglong>(folder.photo_count)},
                                {QStringLiteral("willCreate"), folder.will_create},
                                {QStringLiteral("secondCopy"), folder.second_copy}});
                    emit changed();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
