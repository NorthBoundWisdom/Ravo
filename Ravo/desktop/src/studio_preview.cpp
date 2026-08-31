#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/domain/types.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{

inline constexpr std::size_t kMaximumPendingThumbnailRequests = kLibraryPageDefaultSize * 3U;

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

struct PreparedScopeAnalysis
{
    QString scope_mode;
    RgbHistogram histogram;
    QImage diagnostic;
};

struct PreparedPreviewAnalysis : PreparedScopeAnalysis
{
    QString pixel_sha256;
};

[[nodiscard]] Result<QString> preview_pixel_sha256(const QImage &image, const QString &profile_id,
                                                   const CancellationToken &cancellation)
{
    if (image.isNull())
        return make_error(ErrorCode::kValidation, "Preview identity image is empty");
    const QImage rgb = image.format() == QImage::Format_RGB888 ?
                           image :
                           image.convertToFormat(QImage::Format_RGB888);
    auto active = cancellation.check();
    if (!active)
        return active.error();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int row = 0; row < rgb.height(); ++row)
    {
        if ((row & 15) == 0)
        {
            active = cancellation.check();
            if (!active)
                return active.error();
        }
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(rgb.constScanLine(row)),
                                    static_cast<qsizetype>(rgb.width() * 3)));
    }
    hash.addData(profile_id.toUtf8());
    active = cancellation.check();
    if (!active)
        return active.error();
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] Result<PreparedScopeAnalysis>
prepare_scope_analysis(const QImage &scope_source, const QString &scope_mode,
                       const CancellationToken &cancellation)
{
    if (scope_source.isNull())
        return make_error(ErrorCode::kValidation, "Preview scope image is empty");

    QImage rgb = scope_source;
    if (rgb.format() != QImage::Format_RGB888)
        rgb = rgb.convertToFormat(QImage::Format_RGB888);
    if (rgb.width() <= 0 || rgb.height() <= 0)
        return make_error(ErrorCode::kValidation, "Preview scope dimensions are invalid");

    RasterBuffer raster;
    raster.width = static_cast<std::uint32_t>(rgb.width());
    raster.height = static_cast<std::uint32_t>(rgb.height());
    raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    for (std::uint32_t y = 0; y < raster.height; ++y)
    {
        if ((y & 15U) == 0U)
        {
            auto active = cancellation.check();
            if (!active)
                return active.error();
        }
        const auto *row = rgb.constScanLine(static_cast<int>(y));
        std::copy_n(row, static_cast<std::size_t>(raster.width) * 3U,
                    raster.srgb.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) *
                                                                      raster.width * 3U));
    }

    auto histogram = collect_rgb_histogram(raster);
    if (!histogram)
        return histogram.error();
    auto active = cancellation.check();
    if (!active)
        return active.error();

    PreparedScopeAnalysis result;
    result.scope_mode = scope_mode;
    result.histogram = std::move(histogram).value();
    if (scope_mode == QLatin1String("parade"))
    {
        auto parade = collect_rgb_parade(raster);
        if (!parade)
            return parade.error();
        const auto &value = parade.value();
        if (value.bins == 0 || value.tones == 0 ||
            value.rgb.size() != static_cast<std::size_t>(value.bins) * 3U * value.tones * 3U)
        {
            return make_error(ErrorCode::kValidation, "Preview RGB parade is invalid");
        }
        const QImage view(value.rgb.data(), static_cast<int>(value.bins * 3U),
                          static_cast<int>(value.tones), static_cast<int>(value.bins * 9U),
                          QImage::Format_RGB888);
        result.diagnostic = view.copy();
    }
    else if (scope_mode == QLatin1String("waveform"))
    {
        auto waveform = collect_rgb_waveform(raster);
        if (!waveform)
            return waveform.error();
        result.diagnostic = scope_image(waveform.value());
    }
    else if (scope_mode == QLatin1String("vectorscope"))
    {
        auto vectorscope = collect_uv_vectorscope(raster);
        if (!vectorscope)
            return vectorscope.error();
        result.diagnostic = scope_image(vectorscope.value());
    }
    else if (scope_mode == QLatin1String("split"))
    {
        auto split = collect_split_scope(raster);
        if (!split)
            return split.error();
        result.diagnostic = scope_image(split.value());
    }
    active = cancellation.check();
    if (!active)
        return active.error();
    return result;
}

[[nodiscard]] Result<PreparedPreviewAnalysis>
prepare_preview_analysis(const QImage &identity_image, const QImage &scope_source,
                         const QString &profile_id, const QString &scope_mode,
                         const CancellationToken &cancellation)
{
    auto digest = preview_pixel_sha256(identity_image, profile_id, cancellation);
    if (!digest)
        return digest.error();
    auto scopes = prepare_scope_analysis(scope_source, scope_mode, cancellation);
    if (!scopes)
        return scopes.error();
    auto prepared_scopes = std::move(scopes).value();
    PreparedPreviewAnalysis result;
    result.scope_mode = std::move(prepared_scopes.scope_mode);
    result.histogram = std::move(prepared_scopes.histogram);
    result.diagnostic = std::move(prepared_scopes.diagnostic);
    result.pixel_sha256 = std::move(digest).value();
    return result;
}

[[nodiscard]] Result<QImage> preview_result_image(const PreviewResult &preview)
{
    if (!preview.rgb.empty())
    {
        const auto expected = static_cast<std::size_t>(preview.width) * preview.height * 3U;
        if (preview.width == 0 || preview.height == 0 || preview.rgb.size() != expected)
        {
            return make_error(ErrorCode::kValidation, "Interactive preview pixels are invalid");
        }
        const QImage view(preview.rgb.data(), static_cast<int>(preview.width),
                          static_cast<int>(preview.height), static_cast<int>(preview.width * 3U),
                          QImage::Format_RGB888);
        QImage owned = view.copy();
        if (preview.color_profile.icc_bytes.empty())
        {
            return make_error(ErrorCode::kValidation,
                              "Interactive preview has no declared ICC profile");
        }
        const QByteArray bytes(
            reinterpret_cast<const char *>(preview.color_profile.icc_bytes.data()),
            static_cast<qsizetype>(preview.color_profile.icc_bytes.size()));
        const QColorSpace color_space = QColorSpace::fromIccProfile(bytes);
        if (!color_space.isValid())
        {
            return make_error(ErrorCode::kValidation, "Interactive preview ICC profile is invalid");
        }
        owned.setColorSpace(color_space);
        return owned;
    }
    if (preview.cache_path.empty())
    {
        return make_error(ErrorCode::kValidation, "Preview returned neither pixels nor a resource");
    }
    QImage cached(qstring_from_utf8(preview.cache_path));
    if (cached.isNull())
    {
        return make_error(ErrorCode::kIo, "Preview resource could not be loaded",
                          {{"path", preview.cache_path}});
    }
    return cached;
}

[[nodiscard]] QSize stable_preview_viewport_size(const QSize current, const QSize displayed,
                                                 const bool preserve_extent)
{
    if (!preserve_extent || current.isEmpty())
    {
        return displayed;
    }

    const double current_aspect =
        static_cast<double>(current.width()) / static_cast<double>(current.height());
    const double displayed_aspect =
        static_cast<double>(displayed.width()) / static_cast<double>(displayed.height());
    const int compared_extent = std::min(std::max(current.width(), current.height()),
                                         std::max(displayed.width(), displayed.height()));
    if (std::abs(current_aspect - displayed_aspect) <= 1.0 / static_cast<double>(compared_extent))
    {
        return current;
    }

    const int extent = std::max(current.width(), current.height());
    if (displayed.width() >= displayed.height())
    {
        return QSize(extent, std::max(1, static_cast<int>(std::lround(static_cast<double>(extent) *
                                                                      displayed.height() /
                                                                      displayed.width()))));
    }
    return QSize(std::max(1, static_cast<int>(std::lround(static_cast<double>(extent) *
                                                          displayed.width() / displayed.height()))),
                 extent);
}

} // namespace

QUrl StudioPresenter::previewUrl() const
{
    return preview_url_;
}

int StudioPresenter::previewViewportWidth() const noexcept
{
    return preview_viewport_width_;
}

int StudioPresenter::previewViewportHeight() const noexcept
{
    return preview_viewport_height_;
}

QImage StudioPresenter::previewImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return preview_image_;
}

QUrl StudioPresenter::comparisonBeforeUrl() const
{
    return comparison_before_url_;
}

QImage StudioPresenter::comparisonBeforeImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return comparison_before_image_;
}

bool StudioPresenter::clear_comparison()
{
    const bool changed =
        comparison_active_ || comparison_before_requested_ || !comparison_before_url_.isEmpty();
    comparison_active_ = false;
    comparison_before_requested_ = false;
    if (pending_preview_.has_value() && pending_preview_->comparison_before)
    {
        pending_preview_.reset();
    }
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = QImage();
    }
    comparison_before_url_.clear();
    return changed;
}

void StudioPresenter::clear_displayed_preview()
{
    cancel_preview_analysis("preview_cleared");
    static_cast<void>(clear_comparison());
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = QImage();
        preview_url_.clear();
    }
    preview_base_image_ = QImage();
    preview_viewport_width_ = 0;
    preview_viewport_height_ = 0;
    preview_mask_alpha_.clear();
    live_preview_revision_ = 0;
    live_preview_width_ = 0;
    live_preview_height_ = 0;
    live_preview_color_profile_id_.clear();
    live_preview_pixel_sha256_.clear();
    displayed_develop_.reset();
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
    auto prepared = prepare_scope_analysis(image, scope_mode_, {});
    if (!prepared)
    {
        clear_scopes();
        return;
    }
    auto analysis = std::move(prepared).value();
    scope_histogram_ = std::move(analysis.histogram);
    ++scope_revision_;
    if (scope_mode_ == QLatin1String("parade"))
    {
        scope_parade_image_ = std::move(analysis.diagnostic);
        scope_parade_url_ =
            QUrl(QStringLiteral("image://studioScope/parade?r=%1").arg(scope_revision_));
    }
    else if (scope_mode_ == QLatin1String("waveform"))
    {
        scope_waveform_image_ = std::move(analysis.diagnostic);
        scope_waveform_url_ =
            QUrl(QStringLiteral("image://studioScope/waveform?r=%1").arg(scope_revision_));
    }
    else if (scope_mode_ == QLatin1String("vectorscope"))
    {
        scope_vectorscope_image_ = std::move(analysis.diagnostic);
        scope_vectorscope_url_ =
            QUrl(QStringLiteral("image://studioScope/vectorscope?r=%1").arg(scope_revision_));
    }
    else if (scope_mode_ == QLatin1String("split"))
    {
        scope_split_image_ = std::move(analysis.diagnostic);
        scope_split_url_ =
            QUrl(QStringLiteral("image://studioScope/split?r=%1").arg(scope_revision_));
    }
    emit scopesChanged();
}

void StudioPresenter::schedule_preview_analysis(const QImage &identity_image,
                                                const QImage &scope_source,
                                                const std::uint64_t preview_revision,
                                                const std::string &asset_id,
                                                const QString &profile_id)
{
    const auto analysis_revision = preview_analysis_owner_.supersede("preview_analysis_superseded");
    const auto cancellation = preview_analysis_owner_.begin();
    const QString requested_scope_mode = scope_mode_;
    preview_identity_pending_ = true;

    std::function<void()> task = [this, identity_image, scope_source, preview_revision, asset_id,
                                  profile_id, requested_scope_mode, analysis_revision, cancellation]
    {
        auto prepared = prepare_preview_analysis(identity_image, scope_source, profile_id,
                                                 requested_scope_mode, cancellation);
        QMetaObject::invokeMethod(
            this,
            [this, preview_revision, asset_id, analysis_revision,
             prepared = std::move(prepared)]() mutable
            {
                if (!preview_analysis_owner_.accepts(analysis_revision, asset_id,
                                                     utf8_from_qstring(selected_asset_id_)) ||
                    live_preview_revision_ != preview_revision)
                {
                    return;
                }
                preview_identity_pending_ = false;
                if (!prepared)
                {
                    live_preview_pixel_sha256_.clear();
                    if (prepared.error().code != ErrorCode::kCancelled)
                    {
                        clear_scopes();
                        setError(qstring_from_utf8(prepared.error().message));
                    }
                    emit previewIdentityChanged();
                    return;
                }

                auto analysis = std::move(prepared).value();
                live_preview_pixel_sha256_ = std::move(analysis.pixel_sha256);
                if (analysis.scope_mode == scope_mode_)
                {
                    scope_histogram_ = std::move(analysis.histogram);
                    ++scope_revision_;
                    if (scope_mode_ == QLatin1String("parade"))
                    {
                        scope_parade_image_ = std::move(analysis.diagnostic);
                        scope_parade_url_ = QUrl(
                            QStringLiteral("image://studioScope/parade?r=%1").arg(scope_revision_));
                    }
                    else if (scope_mode_ == QLatin1String("waveform"))
                    {
                        scope_waveform_image_ = std::move(analysis.diagnostic);
                        scope_waveform_url_ =
                            QUrl(QStringLiteral("image://studioScope/waveform?r=%1")
                                     .arg(scope_revision_));
                    }
                    else if (scope_mode_ == QLatin1String("vectorscope"))
                    {
                        scope_vectorscope_image_ = std::move(analysis.diagnostic);
                        scope_vectorscope_url_ =
                            QUrl(QStringLiteral("image://studioScope/vectorscope?r=%1")
                                     .arg(scope_revision_));
                    }
                    else if (scope_mode_ == QLatin1String("split"))
                    {
                        scope_split_image_ = std::move(analysis.diagnostic);
                        scope_split_url_ = QUrl(
                            QStringLiteral("image://studioScope/split?r=%1").arg(scope_revision_));
                    }
                    emit scopesChanged();
                }
                emit previewIdentityChanged();
            },
            Qt::QueuedConnection);
    };

    bool start_worker = false;
    {
        const QMutexLocker lock(&preview_analysis_queue_mutex_);
        pending_preview_analysis_ = std::move(task);
        if (!preview_analysis_worker_active_)
        {
            preview_analysis_worker_active_ = true;
            start_worker = true;
        }
    }
    if (start_worker && !preview_analysis_executor_.post([this] { drain_preview_analysis(); }))
    {
        {
            const QMutexLocker lock(&preview_analysis_queue_mutex_);
            pending_preview_analysis_.reset();
            preview_analysis_worker_active_ = false;
        }
        preview_identity_pending_ = false;
        setError(QStringLiteral("Preview analysis worker is unavailable."));
    }
}

void StudioPresenter::drain_preview_analysis()
{
    for (;;)
    {
        std::function<void()> task;
        {
            const QMutexLocker lock(&preview_analysis_queue_mutex_);
            if (!pending_preview_analysis_)
            {
                preview_analysis_worker_active_ = false;
                return;
            }
            task = std::move(*pending_preview_analysis_);
            pending_preview_analysis_.reset();
        }
        task();
    }
}

void StudioPresenter::cancel_preview_analysis(std::string reason)
{
    static_cast<void>(preview_analysis_owner_.supersede(std::move(reason)));
    const QMutexLocker lock(&preview_analysis_queue_mutex_);
    pending_preview_analysis_.reset();
    preview_identity_pending_ = false;
}

void StudioPresenter::show_preview_result(const PreviewResult &preview,
                                          const std::uint64_t revision,
                                          const bool preserve_viewport_extent)
{
    auto prepared = preview_result_image(preview);
    if (!prepared)
    {
        setError(qstring_from_utf8(prepared.error().message));
        return;
    }
    QImage owned = std::move(prepared).value();
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
    const QSize viewport_size =
        stable_preview_viewport_size(QSize(preview_viewport_width_, preview_viewport_height_),
                                     displayed.size(), preserve_viewport_extent);
    preview_viewport_width_ = viewport_size.width();
    preview_viewport_height_ = viewport_size.height();
    live_preview_revision_ = revision;
    live_preview_width_ = static_cast<std::uint32_t>(std::max(0, displayed.width()));
    live_preview_height_ = static_cast<std::uint32_t>(std::max(0, displayed.height()));
    live_preview_color_profile_id_ = qstring_from_utf8(preview.color_profile.identifier);
    if (live_preview_color_profile_id_.isEmpty() && displayed.colorSpace().isValid())
    {
        live_preview_color_profile_id_ = displayed.colorSpace().description();
        if (live_preview_color_profile_id_.isEmpty())
            live_preview_color_profile_id_ = QStringLiteral("embedded-icc");
    }
    live_preview_pixel_sha256_.clear();
    preview_url_ = !preview.rgb.empty() ?
                       QUrl(QStringLiteral("image://studioPreview/live?r=%1").arg(revision)) :
                       QUrl::fromLocalFile(qstring_from_utf8(preview.cache_path));
    schedule_preview_analysis(displayed, owned, revision, preview.asset_id,
                              live_preview_color_profile_id_);
}

void StudioPresenter::show_comparison_before_result(const PreviewResult &preview,
                                                    const std::uint64_t revision)
{
    auto prepared = preview_result_image(preview);
    if (!prepared)
    {
        setError(qstring_from_utf8(prepared.error().message));
        return;
    }
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = std::move(prepared).value();
    }
    comparison_before_url_ =
        QUrl(QStringLiteral("image://studioPreview/before?r=%1").arg(revision));
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
    if (preview_base_image_.isNull())
    {
        emit scopesChanged();
        return;
    }
    refresh_scopes(preview_base_image_);
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
    if (thumbnail_requests_.contains(id) ||
        std::find(pending_thumbnail_ids_.begin(), pending_thumbnail_ids_.end(), id) !=
            pending_thumbnail_ids_.end())
    {
        return;
    }
    if (!assets_.assetById(asset_id))
    {
        return;
    }
    if (!preview_work_active_ && !thumbnail_request_in_flight_ && pending_thumbnail_ids_.empty())
    {
        preview_work_completed_ = 0;
        preview_work_total_ = 0;
    }
    if (pending_thumbnail_ids_.size() >= kMaximumPendingThumbnailRequests)
    {
        pending_thumbnail_ids_.pop_back();
        preview_work_total_ = std::max(preview_work_completed_, preview_work_total_ - 1);
    }
    pending_thumbnail_ids_.push_front(id);
    ++preview_work_total_;
    preview_work_active_ = true;
    emit libraryWorkChanged();
    kickThumbnailDemand();
}

void StudioPresenter::startThumbnailRequest(std::string id)
{
    const auto revision = ++thumbnail_revision_;
    const auto cancellation = thumbnail_work_.token();
    thumbnail_requests_[id] = revision;
    const bool queued = executor_.post(
        [this, id, revision, cancellation]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kThumbnailMaxEdge;
                request.request_revision = revision;
                request.purpose = PreviewPurpose::kBrowse;
                request.prefer_embedded_preview = true;
                request.cancellation = cancellation;
                preview = service_->request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = thumbnail_requests_.find(id);
                    if (latest == thumbnail_requests_.end() || latest->second != revision)
                    {
                        finishThumbnailRequest(false);
                        return;
                    }
                    thumbnail_requests_.erase(latest);
                    if (catalog_path_.isEmpty() || !assets_.assetById(qstring_from_utf8(id)))
                    {
                        finishThumbnailRequest(false);
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
                        finishThumbnailRequest(true);
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
                        finishThumbnailRequest(false);
                        return;
                    }
                    if (preview.error().code == ErrorCode::kCancelled)
                    {
                        if (std::find(pending_thumbnail_ids_.begin(), pending_thumbnail_ids_.end(),
                                      id) == pending_thumbnail_ids_.end())
                        {
                            if (pending_thumbnail_ids_.size() >= kMaximumPendingThumbnailRequests)
                            {
                                pending_thumbnail_ids_.pop_back();
                                preview_work_total_ =
                                    std::max(preview_work_completed_, preview_work_total_ - 1);
                            }
                            pending_thumbnail_ids_.push_front(id);
                        }
                        thumbnail_request_in_flight_ = false;
                        kickThumbnailDemand();
                        return;
                    }
                    assets_.setThumbnail(id, {}, QStringLiteral("failed"));
                    finishThumbnailRequest(false);
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        thumbnail_requests_.erase(id);
        assets_.setThumbnail(id, {}, QStringLiteral("failed"));
        finishThumbnailRequest(false);
    }
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
