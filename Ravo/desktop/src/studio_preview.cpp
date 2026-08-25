#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <cstddef>

#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/domain/types.h"
#include "studio_qt.h"

namespace ravo
{
QUrl StudioPresenter::previewUrl() const
{
    return preview_url_;
}

QImage StudioPresenter::previewImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return preview_image_;
}

void StudioPresenter::clear_displayed_preview()
{
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = QImage();
        preview_url_.clear();
    }
    clear_scopes();
}

QVariantList StudioPresenter::histogram_channel_list(
    const std::array<std::uint32_t, kRgbHistogramBins> &channel)
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(kRgbHistogramBins));
    for (const auto count : channel)
    {
        list.push_back(QVariant::fromValue(count));
    }
    return list;
}

void StudioPresenter::clear_scopes()
{
    scope_histogram_ = {};
    scope_parade_image_ = QImage();
    scope_parade_url_.clear();
    ++scope_revision_;
    emit scopesChanged();
}

void StudioPresenter::refresh_scopes(const QImage &image)
{
    if (image.isNull())
    {
        clear_scopes();
        return;
    }
    QImage rgb = image;
    if (rgb.format() != QImage::Format_RGB888)
    {
        rgb = rgb.convertToFormat(QImage::Format_RGB888);
    }
    RasterBuffer raster;
    raster.width = static_cast<std::uint32_t>(rgb.width());
    raster.height = static_cast<std::uint32_t>(rgb.height());
    raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    for (std::uint32_t y = 0; y < raster.height; ++y)
    {
        const auto *row = rgb.constScanLine(static_cast<int>(y));
        std::copy_n(row, static_cast<std::size_t>(raster.width) * 3U,
                    raster.srgb.begin() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * raster.width * 3U));
    }
    auto histogram = collect_rgb_histogram(raster);
    auto parade = collect_rgb_parade(raster);
    if (!histogram || !parade)
    {
        clear_scopes();
        return;
    }
    scope_histogram_ = std::move(histogram).value();
    const auto &parade_value = parade.value();
    if (parade_value.bins == 0 || parade_value.tones == 0 ||
        parade_value.rgb.size() !=
            static_cast<std::size_t>(parade_value.bins) * 3U * parade_value.tones * 3U)
    {
        scope_parade_image_ = QImage();
    }
    else
    {
        const QImage view(parade_value.rgb.data(), static_cast<int>(parade_value.bins * 3U),
                          static_cast<int>(parade_value.tones),
                          static_cast<int>(parade_value.bins * 9U), QImage::Format_RGB888);
        scope_parade_image_ = view.copy();
    }
    ++scope_revision_;
    scope_parade_url_ =
        QUrl(QStringLiteral("image://studioScope/parade?r=%1").arg(scope_revision_));
    emit scopesChanged();
}

void StudioPresenter::show_preview_result(const PreviewResult &preview,
                                          const std::uint64_t revision)
{
    if (!preview.srgb.empty())
    {
        const auto expected = static_cast<std::size_t>(preview.width) * preview.height * 3U;
        if (preview.width == 0 || preview.height == 0 || preview.srgb.size() != expected)
        {
            setError(QStringLiteral("Interactive preview pixels are invalid"));
            return;
        }
        const QImage view(preview.srgb.data(), static_cast<int>(preview.width),
                          static_cast<int>(preview.height), static_cast<int>(preview.width * 3U),
                          QImage::Format_RGB888);
        const QImage owned = view.copy();
        {
            const QMutexLocker lock(&preview_image_mutex_);
            preview_image_ = owned;
        }
        preview_url_ = QUrl(QStringLiteral("image://studioPreview/live?r=%1").arg(revision));
        refresh_scopes(owned);
        return;
    }
    const QImage cached = QImage(qstring_from_utf8(preview.cache_path));
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = cached;
    }
    preview_url_ = QUrl::fromLocalFile(qstring_from_utf8(preview.cache_path));
    refresh_scopes(cached);
}

bool StudioPresenter::previewLoading() const noexcept
{
    return preview_loading_;
}

QString StudioPresenter::scopeMode() const
{
    return scope_mode_;
}

void StudioPresenter::setScopeMode(const QString &mode)
{
    const QString next = mode == QLatin1String("parade") ? QStringLiteral("parade") :
                                                           QStringLiteral("histogram");
    if (scope_mode_ == next)
    {
        return;
    }
    scope_mode_ = next;
    emit scopesChanged();
}

QVariantList StudioPresenter::scopeHistogramRed() const
{
    return histogram_channel_list(scope_histogram_.red);
}

QVariantList StudioPresenter::scopeHistogramGreen() const
{
    return histogram_channel_list(scope_histogram_.green);
}

QVariantList StudioPresenter::scopeHistogramBlue() const
{
    return histogram_channel_list(scope_histogram_.blue);
}

double StudioPresenter::scopeHistogramMax() const noexcept
{
    return static_cast<double>(scope_histogram_.max_count);
}

QUrl StudioPresenter::scopeParadeUrl() const
{
    return scope_parade_url_;
}

QImage StudioPresenter::scopeParadeImage() const
{
    return scope_parade_image_;
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto existing = assets_.assetById(asset_id);
    if (!existing)
    {
        return;
    }
    const auto revision = ++thumbnail_revision_;
    const auto id = utf8_from_qstring(asset_id);
    thumbnail_requests_[id] = revision;
    executor_.post(
        [this, id, revision]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kThumbnailMaxEdge;
                request.request_revision = revision;
                request.cancellation = shutdown_.token();
                preview = service_->request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = thumbnail_requests_.find(id);
                    if (latest == thumbnail_requests_.end() || latest->second != revision)
                    {
                        return;
                    }
                    if (catalog_path_.isEmpty() || !assets_.assetById(qstring_from_utf8(id)))
                    {
                        return;
                    }
                    if (preview)
                    {
                        assets_.setThumbnail(
                            id, QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path)),
                            preview.value().original_missing ? QStringLiteral("missing") :
                                                               QStringLiteral("ready"));
                        if (utf8_from_qstring(selected_asset_id_) == id)
                        {
                            emit selectionChanged();
                        }
                        if (preview.value().original_missing)
                        {
                            assets_.markOriginalMissing(id);
                        }
                        return;
                    }
                    if (preview.error().code == ErrorCode::kNotFound)
                    {
                        assets_.markOriginalMissing(id);
                        assets_.setThumbnail(id, {}, QStringLiteral("missing"));
                        if (selected_ids_.contains(id))
                        {
                            emit selectionChanged();
                        }
                        return;
                    }
                    assets_.setThumbnail(id, {}, QStringLiteral("failed"));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::requestPreviewForSelection()
{
    enqueue_preview();
}

} // namespace ravo
