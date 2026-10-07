#include "ravo/desktop/studio_inspect_presenter.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/services/display_presentation.h"
#include "ravo/services/preview_service.h"
#include "studio_preview_handoff.h"
#include "studio_qt.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <QColorSpace>
#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#endif
namespace ravo
{

QUrl StudioInspectPresenter::inspectRoiUrl() const
{
    return inspect_roi_url_;
}

QImage StudioInspectPresenter::inspectRoiImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return inspect_roi_image_;
}

double StudioInspectPresenter::inspectRoiX() const noexcept
{
    return inspect_roi_x_;
}

double StudioInspectPresenter::inspectRoiY() const noexcept
{
    return inspect_roi_y_;
}

double StudioInspectPresenter::inspectRoiWidth() const noexcept
{
    return inspect_roi_width_;
}

double StudioInspectPresenter::inspectRoiHeight() const noexcept
{
    return inspect_roi_height_;
}

quint64 StudioInspectPresenter::gpuPreviewGeneration() const noexcept
{
    return gpu_preview_generation_;
}

quint64 StudioInspectPresenter::gpuPreviewNativeSurface() const noexcept
{
    return gpu_preview_presented_surface_;
}

int StudioInspectPresenter::gpuPreviewWidth() const noexcept
{
    return gpu_preview_width_;
}

int StudioInspectPresenter::gpuPreviewHeight() const noexcept
{
    return gpu_preview_height_;
}

quint64 StudioInspectPresenter::gpuRoiGeneration() const noexcept
{
    return gpu_roi_generation_;
}

quint64 StudioInspectPresenter::gpuRoiNativeSurface() const noexcept
{
    return gpu_roi_presented_surface_;
}

int StudioInspectPresenter::gpuRoiWidth() const noexcept
{
    return gpu_roi_width_;
}

int StudioInspectPresenter::gpuRoiHeight() const noexcept
{
    return gpu_roi_height_;
}

void StudioInspectPresenter::refresh_inspect_roi()
{
    if (zoom_mode_ != QLatin1String("actual") || context_.selected_asset_id.isEmpty() ||
        inspect_roi_width_ <= 0.0 || inspect_roi_height_ <= 0.0)
    {
        return;
    }
    requestInspectRoi(inspect_roi_x_, inspect_roi_y_, inspect_roi_width_, inspect_roi_height_);
}

void StudioInspectPresenter::clear_inspect_roi()
{
    inspect_roi_owner_.cancel("inspect_roi_cleared");
    {
        const QMutexLocker lock(&preview_image_mutex_);
        inspect_roi_image_ = {};
    }
    const bool had_roi = !inspect_roi_url_.isEmpty() || inspect_roi_width_ != 0.0 ||
                         inspect_roi_height_ != 0.0 || gpu_roi_generation_ != 0U ||
                         gpu_roi_presented_surface_ != 0U;
    inspect_roi_url_.clear();
    inspect_roi_x_ = 0.0;
    inspect_roi_y_ = 0.0;
    inspect_roi_width_ = 0.0;
    inspect_roi_height_ = 0.0;
    gpu_roi_generation_ = 0;

    release_gpu_roi_presented_surface();
    gpu_roi_width_ = 0;
    gpu_roi_height_ = 0;
    if (had_roi)
        emit inspectRoiChanged();
}

void StudioInspectPresenter::requestInspectRoi(const double x, const double y, const double width,
                                               const double height)
{
    if (stopped_)
    {
        emit errorOccurred(QStringLiteral("Inspect owner is closed"));
        return;
    }
    if (zoom_mode_ != QLatin1String("actual") || context_.selected_asset_id.isEmpty() ||
        context_.catalog_path.isEmpty())
    {
        clear_inspect_roi();
        return;
    }
    PreviewNormRect roi{x, y, width, height};
    const auto revision = inspect_roi_owner_.supersede("inspect_roi_requested");
    const auto cancellation = inspect_roi_owner_.begin();
    const auto asset_id = utf8_from_qstring(context_.selected_asset_id);
    const auto params = host_.develop_params();
    const bool queued = context_.executor.post(
        [this, roi, revision, cancellation, asset_id, params]()
        {
            PreviewRequest request;
            request.asset_id = asset_id;
            request.roi = roi;
            request.persist_preview_record = false;
            request.prefer_embedded_preview = false;
            request.request_revision = revision;
            request.cancellation = cancellation;
            request.need_cpu_pixels = false;
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (auto *service = host_.preview_service(); service != nullptr)
            {
                preview = service->request_preview(request, params);
                if (preview)
                {
                    auto owned = own_preview_pixels_for_handoff(preview.value(), cancellation);
                    if (!owned)
                        preview = owned.error();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, roi, revision, asset_id, preview = std::move(preview)]() mutable
                {
                    if (!inspect_roi_owner_.accepts(
                            revision, asset_id, utf8_from_qstring(context_.selected_asset_id)) ||
                        zoom_mode_ != QLatin1String("actual"))
                    {
                        return;
                    }
                    if (!preview)
                    {
                        const auto reason = preview.error().context.find("reason");
                        if (reason != preview.error().context.end() &&
                            (reason->second == "preview_roi_geometry_unsupported" ||
                             reason->second == "preview_roi_covers_full_frame" ||
                             reason->second == "preview_roi_media_unsupported" ||
                             reason->second == "preview_roi_sensor_unsupported"))
                        {
                            clear_inspect_roi();
                            return;
                        }
                        if (preview.error().code != ErrorCode::kCancelled)
                        {
                            emit errorOccurred(qstring_from_utf8(preview.error().message));
                        }
                        return;
                    }
                    auto prepared = preparePreview(preview.value());
                    if (!prepared)
                    {
                        emit errorOccurred(qstring_from_utf8(prepared.error().message));
                        return;
                    }
                    QImage roi_base = std::move(prepared).value();
                    if (preview.value().gpu_display_generation != 0U)
                    {
                        auto presentation = apply_display_presentation_image(
                            roi_base, preview.value().color_profile);
                        if (!presentation)
                        {
                            emit errorOccurred(qstring_from_utf8(presentation.error().message));
                            return;
                        }
                        if (!publish_gpu_roi_presented_surface(presentation.value()))
                        {
                            return;
                        }
                    }
                    else
                    {
                        release_gpu_roi_presented_surface();

                        gpu_roi_width_ = 0;
                        gpu_roi_height_ = 0;
                    }
                    gpu_roi_generation_ = preview.value().gpu_display_generation;
                    {
                        // Both display paths publish an owned image before exposing its URL.
                        const QMutexLocker lock(&preview_image_mutex_);
                        inspect_roi_image_ = roi_base;
                    }
                    inspect_roi_x_ = roi.x;
                    inspect_roi_y_ = roi.y;
                    inspect_roi_width_ = roi.width;
                    inspect_roi_height_ = roi.height;
                    inspect_roi_url_ =
                        QUrl(QStringLiteral("image://studioPreview/inspectRoi?r=%1").arg(revision));
                    emit inspectRoiChanged();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        inspect_roi_owner_.cancel("executor_rejected");
        emit errorOccurred(QStringLiteral("Unable to queue inspect preview"));
    }
}

QString StudioInspectPresenter::zoomMode() const
{
    return zoom_mode_;
}

double StudioInspectPresenter::zoomFactor() const noexcept
{
    return zoom_factor_;
}

void StudioInspectPresenter::setZoomMode(const QString &mode)
{
    QString normalized = QStringLiteral("fit");
    double factor = zoom_factor_;
    if (mode == QStringLiteral("fill"))
    {
        normalized = QStringLiteral("fill");
    }
    else if (mode == QStringLiteral("actual") || mode == QStringLiteral("100"))
    {
        normalized = QStringLiteral("actual");
        factor = 1.0;
    }
    else if (mode == QStringLiteral("custom"))
    {
        normalized = QStringLiteral("custom");
    }
    if (zoom_mode_ == normalized && zoom_factor_ == factor)
    {
        return;
    }
    zoom_mode_ = normalized;
    zoom_factor_ = factor;
    if (zoom_mode_ != QStringLiteral("actual"))
    {
        last_non_actual_zoom_mode_ = zoom_mode_;
        last_non_actual_zoom_factor_ = zoom_factor_;
        clear_inspect_roi();
    }
    emit zoomChanged();
}

void StudioInspectPresenter::setZoomFactor(const double factor)
{
    const double clamped = std::clamp(factor, 0.1, 8.0);
    if (zoom_mode_ == QStringLiteral("custom") && zoom_factor_ == clamped)
    {
        return;
    }
    zoom_mode_ = QStringLiteral("custom");
    zoom_factor_ = clamped;
    last_non_actual_zoom_mode_ = zoom_mode_;
    last_non_actual_zoom_factor_ = zoom_factor_;
    clear_inspect_roi();
    emit zoomChanged();
}

void StudioInspectPresenter::adjustZoom(const int wheel_delta)
{
    const double step = wheel_delta > 0 ? 1.1 : 1.0 / 1.1;
    const double current = zoom_mode_ == QStringLiteral("actual") ? 1.0 : zoom_factor_;
    setZoomFactor(current * step);
}

void StudioInspectPresenter::toggleActualSize()
{
    if (zoom_mode_ == QStringLiteral("actual"))
    {
        if (last_non_actual_zoom_mode_ == QStringLiteral("custom"))
        {
            setZoomFactor(last_non_actual_zoom_factor_);
            return;
        }
        setZoomMode(last_non_actual_zoom_mode_);
        return;
    }
    setZoomMode(QStringLiteral("actual"));
}
} // namespace ravo
