#include "studio_import_thumbnail_controller.h"

#include <cstring>
#include <stdexcept>

#include <QImage>
#include <QMetaObject>
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

void StudioImportThumbnailController::ensure(const int row)
{
    if (!host_.page_open || !host_.page_open() || (host_.work_active && host_.work_active()) ||
        (host_.preflight_active && host_.preflight_active()) || !host_.model || row < 0 ||
        row >= host_.model->rowCount() || host_.model->inspected(row) ||
        !host_.model->thumbnail(row).isNull())
        return;
    if (!pending_rows_.insert(row).second)
        return;
    QTimer::singleShot(0, this, &StudioImportThumbnailController::kick);
}

void StudioImportThumbnailController::kick()
{
    if (in_flight_ || !host_.page_open || !host_.page_open() ||
        (host_.work_active && host_.work_active()) ||
        (host_.preflight_active && host_.preflight_active()))
        return;
    while (!pending_rows_.empty())
    {
        const int row = *pending_rows_.begin();
        pending_rows_.erase(pending_rows_.begin());
        if (!host_.model || row < 0 || row >= host_.model->rowCount() ||
            host_.model->inspected(row) || !host_.model->thumbnail(row).isNull())
            continue;
        start(row);
        return;
    }
}

void StudioImportThumbnailController::clearPending()
{
    pending_rows_.clear();
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
    stopped_ = true;
    cancel("thumbnail_controller_shutdown");
    clearPending();
    in_flight_ = false;
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

void StudioImportThumbnailController::start(const int row)
{
    if (!host_.model)
        return;
    const QString source = host_.model->sourcePath(row);
    if (source.isEmpty())
        return;
    const auto generation = host_.scan_generation ? host_.scan_generation() : 0U;
    const auto token = operation_.token();
    in_flight_ = true;
    const bool queued = executor_.post(
        [this, row, source, generation, token]()
        {
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
                return decode_import_thumbnail(*engine_, raster, utf8_from_qstring(source), token);
            };
            auto decoded = decode();
            QImage image;
            std::optional<TaskError> error;
            if (decoded)
                image = import_thumbnail_image(decoded.value());
            else
                error = decoded.error();
            auto *receiver = host_.callback_receiver ? host_.callback_receiver : this;
            QMetaObject::invokeMethod(
                receiver,
                [this, row, source, generation, token, error = std::move(error),
                 image = std::move(image)]() mutable
                {
                    in_flight_ = false;
                    const bool page_ok = host_.page_open && host_.page_open();
                    const bool work_ok = !(host_.work_active && host_.work_active());
                    const auto current_generation =
                        host_.scan_generation ? host_.scan_generation() : 0U;
                    if (generation == current_generation && page_ok && work_ok && token.check() &&
                        host_.model && host_.model->sourcePath(row) == source)
                        host_.model->finishThumbnail(row, std::move(image), std::move(error));
                    kick();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        in_flight_ = false;
        if (host_.set_error)
            host_.set_error(QStringLiteral("Import thumbnail worker is stopped."));
    }
}

} // namespace ravo
