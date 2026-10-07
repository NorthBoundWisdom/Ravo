#include "ravo/desktop/studio_inspect_presenter.h"

#include <algorithm>
#include <utility>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QMetaObject>
#include <QMutexLocker>

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

} // namespace

StudioInspectPresenter::StudioInspectPresenter(Context context, Host host, QObject *parent)
    : QObject(parent)
    , context_(context)
    , host_(std::move(host))
{
    connect(this, &StudioInspectPresenter::zoomChanged, this,
            &StudioInspectPresenter::inspectContextChanged);
    connect(this, &StudioInspectPresenter::previewChanged, this,
            &StudioInspectPresenter::frameChanged);
}

StudioInspectPresenter::~StudioInspectPresenter()
{
    shutdown();
}

void StudioInspectPresenter::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    clear_inspect_roi();
    releasePresentationResources();
    decoded_preview_images_.clear();
    cancel_preview_analysis("window_closed");
    preview_analysis_executor_.request_stop();
    preview_analysis_executor_.wait();
}

void StudioInspectPresenter::observeSelection(QString asset_id)
{
    observed_selection_ = std::move(asset_id);
}

void StudioInspectPresenter::setBaseSource(QImage image)
{
    base_source_ = std::move(image);
}

void StudioInspectPresenter::resetIdentity()
{
    live_preview_pixel_sha256_.clear();
    observed_frame_revision_ = 0;
}

QString StudioInspectPresenter::pixelSha256() const
{
    return live_preview_pixel_sha256_;
}

bool StudioInspectPresenter::identityPending() const noexcept
{
    return preview_identity_pending_;
}

RgbHistogram StudioInspectPresenter::histogramSnapshot() const
{
    const QMutexLocker lock(&images_mutex_);
    return scope_histogram_;
}

const std::optional<DevelopParams> &StudioInspectPresenter::displayedDevelop() const noexcept
{
    return displayed_develop_;
}

void StudioInspectPresenter::observeDisplayedDevelop(std::optional<DevelopParams> params)
{
    displayed_develop_ = std::move(params);
}

const QVariantMap &StudioInspectPresenter::cropPreviewLayout() const noexcept
{
    return crop_preview_layout_;
}

void StudioInspectPresenter::observeFrameLayout(QVariantMap layout)
{
    crop_preview_layout_ = std::move(layout);
}

QVariantList StudioInspectPresenter::histogram_channel_list(
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

void StudioInspectPresenter::clear_scopes()
{
    {
        const QMutexLocker lock(&images_mutex_);
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
    }
    emit scopesChanged();
}

void StudioInspectPresenter::refresh_scopes(const QImage &image)
{
    auto prepared = prepare_scope_analysis(image, scope_mode_, {});
    if (!prepared)
    {
        clear_scopes();
        return;
    }
    auto analysis = std::move(prepared).value();
    {
        const QMutexLocker lock(&images_mutex_);
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
    }
    emit scopesChanged();
}

void StudioInspectPresenter::schedule_preview_analysis(const QImage &identity_image,
                                                       const QImage &scope_source,
                                                       const std::uint64_t preview_revision,
                                                       const std::string &asset_id,
                                                       const QString &profile_id)
{
    observed_frame_revision_ = preview_revision;
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
                if (stopped_ ||
                    !preview_analysis_owner_.accepts(analysis_revision, asset_id,
                                                     utf8_from_qstring(observed_selection_)) ||
                    observed_frame_revision_ != preview_revision)
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
                        emit errorOccurred(qstring_from_utf8(prepared.error().message));
                    }
                    emit identityChanged();
                    return;
                }

                auto analysis = std::move(prepared).value();
                live_preview_pixel_sha256_ = std::move(analysis.pixel_sha256);
                if (analysis.scope_mode == scope_mode_)
                {
                    {
                        const QMutexLocker lock(&images_mutex_);
                        scope_histogram_ = std::move(analysis.histogram);
                        ++scope_revision_;
                        if (scope_mode_ == QLatin1String("parade"))
                        {
                            scope_parade_image_ = std::move(analysis.diagnostic);
                            scope_parade_url_ =
                                QUrl(QStringLiteral("image://studioScope/parade?r=%1")
                                         .arg(scope_revision_));
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
                            scope_split_url_ = QUrl(QStringLiteral("image://studioScope/split?r=%1")
                                                        .arg(scope_revision_));
                        }
                    }
                    emit scopesChanged();
                }
                emit identityChanged();
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
        emit errorOccurred(QStringLiteral("Preview analysis worker is unavailable."));
    }
}

void StudioInspectPresenter::drain_preview_analysis()
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

void StudioInspectPresenter::cancel_preview_analysis(std::string reason)
{
    static_cast<void>(preview_analysis_owner_.supersede(std::move(reason)));
    const QMutexLocker lock(&preview_analysis_queue_mutex_);
    pending_preview_analysis_.reset();
    preview_identity_pending_ = false;
}

QString StudioInspectPresenter::scopeMode() const
{
    return scope_mode_;
}

void StudioInspectPresenter::setScopeMode(const QString &mode)
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
    if (base_source_.isNull())
    {
        emit scopesChanged();
        return;
    }
    refresh_scopes(base_source_);
}

QVariantList StudioInspectPresenter::scopeHistogramRed() const
{
    return histogram_channel_list(scope_histogram_.red);
}

QVariantList StudioInspectPresenter::scopeHistogramGreen() const
{
    return histogram_channel_list(scope_histogram_.green);
}

QVariantList StudioInspectPresenter::scopeHistogramBlue() const
{
    return histogram_channel_list(scope_histogram_.blue);
}

QVariantList StudioInspectPresenter::scopeHistogramLuma() const
{
    return histogram_channel_list(scope_histogram_.luma);
}

double StudioInspectPresenter::scopeHistogramMax() const noexcept
{
    return static_cast<double>(scope_histogram_.max_count);
}

QUrl StudioInspectPresenter::scopeParadeUrl() const
{
    return scope_parade_url_;
}

QImage StudioInspectPresenter::scopeParadeImage() const
{
    const QMutexLocker lock(&images_mutex_);
    return scope_parade_image_;
}

QUrl StudioInspectPresenter::scopeWaveformUrl() const
{
    return scope_waveform_url_;
}

QImage StudioInspectPresenter::scopeWaveformImage() const
{
    const QMutexLocker lock(&images_mutex_);
    return scope_waveform_image_;
}

QUrl StudioInspectPresenter::scopeVectorscopeUrl() const
{
    return scope_vectorscope_url_;
}

QImage StudioInspectPresenter::scopeVectorscopeImage() const
{
    const QMutexLocker lock(&images_mutex_);
    return scope_vectorscope_image_;
}

QUrl StudioInspectPresenter::scopeSplitUrl() const
{
    return scope_split_url_;
}

QImage StudioInspectPresenter::scopeSplitImage() const
{
    const QMutexLocker lock(&images_mutex_);
    return scope_split_image_;
}

} // namespace ravo
