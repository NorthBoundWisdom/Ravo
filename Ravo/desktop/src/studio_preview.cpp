#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QColorSpace>
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
namespace
{

[[nodiscard]] QImage scope_image(const RgbScopeImage &scope)
{
    if (scope.width == 0U || scope.height == 0U ||
        scope.rgb.size() != static_cast<std::size_t>(scope.width) * scope.height * 3U)
        return {};
    const QImage view(scope.rgb.data(), static_cast<int>(scope.width),
                      static_cast<int>(scope.height), static_cast<int>(scope.width * 3U),
                      QImage::Format_RGB888);
    return view.copy();
}

} // namespace

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
    preview_base_image_ = QImage();
    preview_mask_alpha_.clear();
    clear_scopes();
}

QVariantList
StudioPresenter::histogram_channel_list(const std::array<std::uint32_t, kRgbHistogramBins> &channel)
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
    scope_waveform_image_ = QImage();
    scope_waveform_url_.clear();
    scope_vectorscope_image_ = QImage();
    scope_vectorscope_url_.clear();
    scope_split_image_ = QImage();
    scope_split_url_.clear();
    ++scope_revision_;
    emit scopesChanged();
}

void StudioPresenter::refresh_scopes_from_thumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        clear_scopes();
        return;
    }
    const int row = assets_.indexOf(asset_id);
    if (row < 0)
    {
        clear_scopes();
        return;
    }
    const QUrl url = assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
    if (!url.isLocalFile())
    {
        clear_scopes();
        return;
    }
    refresh_scopes(QImage(url.toLocalFile()));
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
                    raster.srgb.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) *
                                                                      raster.width * 3U));
    }
    auto histogram = collect_rgb_histogram(raster);
    auto parade = collect_rgb_parade(raster);
    auto waveform = collect_rgb_waveform(raster);
    auto vectorscope = collect_uv_vectorscope(raster);
    auto split = collect_split_scope(raster);
    if (!histogram || !parade || !waveform || !vectorscope || !split)
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
    scope_waveform_image_ = scope_image(waveform.value());
    scope_vectorscope_image_ = scope_image(vectorscope.value());
    scope_split_image_ = scope_image(split.value());
    ++scope_revision_;
    scope_parade_url_ =
        QUrl(QStringLiteral("image://studioScope/parade?r=%1").arg(scope_revision_));
    scope_waveform_url_ =
        QUrl(QStringLiteral("image://studioScope/waveform?r=%1").arg(scope_revision_));
    scope_vectorscope_url_ =
        QUrl(QStringLiteral("image://studioScope/vectorscope?r=%1").arg(scope_revision_));
    scope_split_url_ = QUrl(QStringLiteral("image://studioScope/split?r=%1").arg(scope_revision_));
    emit scopesChanged();
}

void StudioPresenter::show_preview_result(const PreviewResult &preview,
                                          const std::uint64_t revision)
{
    if (!preview.rgb.empty())
    {
        const auto expected = static_cast<std::size_t>(preview.width) * preview.height * 3U;
        if (preview.width == 0 || preview.height == 0 || preview.rgb.size() != expected)
        {
            setError(QStringLiteral("Interactive preview pixels are invalid"));
            return;
        }
        const QImage view(preview.rgb.data(), static_cast<int>(preview.width),
                          static_cast<int>(preview.height), static_cast<int>(preview.width * 3U),
                          QImage::Format_RGB888);
        QImage owned = view.copy();
        if (!preview.color_profile.icc_bytes.empty())
        {
            const QByteArray bytes(
                reinterpret_cast<const char *>(preview.color_profile.icc_bytes.data()),
                static_cast<qsizetype>(preview.color_profile.icc_bytes.size()));
            const QColorSpace color_space = QColorSpace::fromIccProfile(bytes);
            if (!color_space.isValid())
            {
                setError(QStringLiteral("Interactive preview ICC profile is invalid"));
                return;
            }
            owned.setColorSpace(color_space);
        }
        else
        {
            setError(QStringLiteral("Interactive preview has no declared ICC profile"));
            return;
        }
        preview_base_image_ = owned;
        preview_mask_alpha_ = preview.mask_alpha;
        QImage displayed = owned;
        if (mask_overlay_visible_ && !preview.mask_alpha.empty() && engine_.has_value())
        {
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(owned.width()) *
                                          static_cast<std::size_t>(owned.height()) * 3U);
            for (int y = 0; y < owned.height(); ++y)
            {
                std::copy_n(owned.constScanLine(y), static_cast<std::size_t>(owned.width()) * 3U,
                            rgb.begin() + static_cast<std::ptrdiff_t>(
                                              static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(owned.width()) * 3U));
            }
            auto composited = engine_->composite_preview_mask_overlay(
                rgb, static_cast<std::uint32_t>(owned.width()),
                static_cast<std::uint32_t>(owned.height()), preview.mask_alpha, {});
            if (composited)
            {
                displayed = QImage(static_cast<int>(owned.width()), static_cast<int>(owned.height()),
                                   QImage::Format_RGB888);
                for (int y = 0; y < displayed.height(); ++y)
                {
                    std::copy_n(rgb.data() + static_cast<std::ptrdiff_t>(
                                                 static_cast<std::size_t>(y) *
                                                 static_cast<std::size_t>(displayed.width()) * 3U),
                                static_cast<std::size_t>(displayed.width()) * 3U,
                                displayed.scanLine(y));
                }
                if (owned.colorSpace().isValid())
                {
                    displayed.setColorSpace(owned.colorSpace());
                }
            }
        }
        {
            const QMutexLocker lock(&preview_image_mutex_);
            preview_image_ = displayed;
        }
        preview_url_ = QUrl(QStringLiteral("image://studioPreview/live?r=%1").arg(revision));
        refresh_scopes(owned);
        return;
    }
    const QImage cached = QImage(qstring_from_utf8(preview.cache_path));
    preview_base_image_ = cached;
    preview_mask_alpha_.clear();
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
    const QString next = mode == QLatin1String("waveform")    ? QStringLiteral("waveform") :
                         mode == QLatin1String("parade")      ? QStringLiteral("parade") :
                         mode == QLatin1String("vectorscope") ? QStringLiteral("vectorscope") :
                         mode == QLatin1String("split")       ? QStringLiteral("split") :
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

QVariantList StudioPresenter::scopeHistogramLuma() const
{
    return histogram_channel_list(scope_histogram_.luma);
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

QUrl StudioPresenter::scopeWaveformUrl() const
{
    return scope_waveform_url_;
}

QImage StudioPresenter::scopeWaveformImage() const
{
    return scope_waveform_image_;
}

QUrl StudioPresenter::scopeVectorscopeUrl() const
{
    return scope_vectorscope_url_;
}

QImage StudioPresenter::scopeVectorscopeImage() const
{
    return scope_vectorscope_image_;
}

QUrl StudioPresenter::scopeSplitUrl() const
{
    return scope_split_url_;
}

QImage StudioPresenter::scopeSplitImage() const
{
    return scope_split_image_;
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    const QString state = assets_.thumbnailState(id);
    if (state == QLatin1String("ready") || state == QLatin1String("missing") ||
        state == QLatin1String("failed"))
    {
        return;
    }
    if (thumbnail_requests_.contains(id))
    {
        return;
    }
    if (!assets_.assetById(asset_id))
    {
        return;
    }
    const auto revision = ++thumbnail_revision_;
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
                request.prefer_embedded_preview = true;
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
                        finishPreviewJob(false);
                        return;
                    }
                    thumbnail_requests_.erase(latest);
                    if (catalog_path_.isEmpty() || !assets_.assetById(qstring_from_utf8(id)))
                    {
                        finishPreviewJob(false);
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
                            emit thumbnailsChanged();
                            if (browse_mode_ == QLatin1String("grid"))
                            {
                                refresh_scopes_from_thumbnail(qstring_from_utf8(id));
                            }
                        }
                        if (preview.value().original_missing)
                        {
                            assets_.markOriginalMissing(id);
                        }
                        finishPreviewJob(true);
                        return;
                    }
                    if (preview.error().code == ErrorCode::kNotFound)
                    {
                        assets_.markOriginalMissing(id);
                        assets_.setThumbnail(id, {}, QStringLiteral("missing"));
                        if (selected_ids_.contains(id))
                        {
                            emit thumbnailsChanged();
                        }
                        finishPreviewJob(false);
                        return;
                    }
                    assets_.setThumbnail(id, {}, QStringLiteral("failed"));
                    finishPreviewJob(false);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::requestPreviewForSelection()
{
    if (browse_mode_ == QLatin1String("grid"))
    {
        preview_loading_ = false;
        emit previewChanged();
        refresh_scopes_from_thumbnail(selected_asset_id_);
        return;
    }
    enqueue_preview();
}

} // namespace ravo
