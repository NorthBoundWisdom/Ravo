#include "ravo/desktop/studio_presenter.h"

#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/services/display_presentation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/domain/types.h"
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#endif
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

#if defined(Q_OS_MACOS)
void StudioPresenter::release_gpu_preview_presented_surface()
{
    if (gpu_preview_presented_surface_ != 0U)
    {
        studio_metal::release_iosurface(gpu_preview_presented_surface_);
        gpu_preview_presented_surface_ = 0U;
    }
}

void StudioPresenter::release_gpu_roi_presented_surface()
{
    if (gpu_roi_presented_surface_ != 0U)
    {
        studio_metal::release_iosurface(gpu_roi_presented_surface_);
        gpu_roi_presented_surface_ = 0U;
    }
}

bool StudioPresenter::publish_gpu_preview_presented_surface(const QImage &presented)
{
    if (presented.isNull() || presented.width() <= 0 || presented.height() <= 0)
        return false;
    auto created =
        studio_metal::create_iosurface_rgba8(static_cast<std::uint32_t>(presented.width()),
                                             static_cast<std::uint32_t>(presented.height()));
    if (!created)
        return false;
    if (!studio_metal::write_rgb8_to_iosurface(created.value(), presented))
    {
        studio_metal::release_iosurface(created.value());
        return false;
    }
    release_gpu_preview_presented_surface();
    gpu_preview_presented_surface_ = created.value();
    gpu_preview_native_surface_ = gpu_preview_presented_surface_;
    gpu_preview_width_ = presented.width();
    gpu_preview_height_ = presented.height();
    return true;
}

bool StudioPresenter::publish_gpu_roi_presented_surface(const QImage &presented)
{
    if (presented.isNull() || presented.width() <= 0 || presented.height() <= 0)
        return false;
    auto created =
        studio_metal::create_iosurface_rgba8(static_cast<std::uint32_t>(presented.width()),
                                             static_cast<std::uint32_t>(presented.height()));
    if (!created)
        return false;
    if (!studio_metal::write_rgb8_to_iosurface(created.value(), presented))
    {
        studio_metal::release_iosurface(created.value());
        return false;
    }
    release_gpu_roi_presented_surface();
    gpu_roi_presented_surface_ = created.value();
    gpu_roi_native_surface_ = gpu_roi_presented_surface_;
    gpu_roi_width_ = presented.width();
    gpu_roi_height_ = presented.height();
    return true;
}
#else
void StudioPresenter::release_gpu_preview_presented_surface()
{
}
void StudioPresenter::release_gpu_roi_presented_surface()
{
}
bool StudioPresenter::publish_gpu_preview_presented_surface(const QImage &)
{
    return false;
}
bool StudioPresenter::publish_gpu_roi_presented_surface(const QImage &)
{
    return false;
}
#endif

QImage
StudioPresenter::apply_display_presentation_image(const QImage &output_referred,
                                                  const ColorProfileState &source_profile) const
{
    if (output_referred.isNull() || display_presentation_ == nullptr ||
        !display_presentation_->valid())
    {
        return output_referred;
    }
    QImage rgb = output_referred;
    if (rgb.format() != QImage::Format_RGB888)
        rgb = rgb.convertToFormat(QImage::Format_RGB888);
    if (rgb.width() <= 0 || rgb.height() <= 0)
        return output_referred;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rgb.width()) *
                                     static_cast<std::size_t>(rgb.height()) * 3U);
    for (int y = 0; y < rgb.height(); ++y)
    {
        std::copy_n(rgb.constScanLine(y), static_cast<std::size_t>(rgb.width()) * 3U,
                    pixels.begin() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(rgb.width()) * 3U));
    }
    auto converted = apply_display_presentation_rgb8(
        pixels, static_cast<std::uint32_t>(rgb.width()), static_cast<std::uint32_t>(rgb.height()),
        source_profile, display_presentation_->presentationState(), CancellationToken{});
    if (!converted)
        return output_referred;

    QImage presented(static_cast<int>(converted.value().width),
                     static_cast<int>(converted.value().height), QImage::Format_RGB888);
    for (int y = 0; y < presented.height(); ++y)
    {
        std::copy_n(converted.value().rgb8.data() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(presented.width()) *
                                                    3U),
                    static_cast<std::size_t>(presented.width()) * 3U, presented.scanLine(y));
    }
    if (!converted.value().color_profile.icc_bytes.empty())
    {
        const QByteArray bytes(
            reinterpret_cast<const char *>(converted.value().color_profile.icc_bytes.data()),
            static_cast<qsizetype>(converted.value().color_profile.icc_bytes.size()));
        const QColorSpace space = QColorSpace::fromIccProfile(bytes);
        if (space.isValid())
            presented.setColorSpace(space);
    }
    return presented;
}

void StudioPresenter::bindDisplayPresentation(StudioDisplayPresentation *owner)
{
    if (display_presentation_ == owner)
        return;
    if (display_presentation_ != nullptr)
        disconnect(display_presentation_, nullptr, this, nullptr);
    display_presentation_ = owner;
    if (display_presentation_ == nullptr)
        return;
    connect(display_presentation_, &StudioDisplayPresentation::stateChanged, this,
            &StudioPresenter::handle_display_presentation_changed);
    reapply_display_presentation_to_cached_previews();
    reapply_display_presentation_to_cached_thumbnails();
}

void StudioPresenter::handle_display_presentation_changed()
{
    reapply_display_presentation_to_cached_previews();
    reapply_display_presentation_to_cached_thumbnails();
}

void StudioPresenter::reapply_display_presentation_to_cached_previews()
{
    bool changed = false;
    if (!preview_base_image_.isNull())
    {
        QImage presented =
            apply_display_presentation_image(preview_base_image_, preview_output_profile_);
        {
            const QMutexLocker lock(&preview_image_mutex_);
            if (presented.cacheKey() != preview_image_.cacheKey())
            {
                preview_image_ = presented;
                changed = true;
            }
        }
        if (gpu_preview_generation_ != 0U && publish_gpu_preview_presented_surface(presented))
        {
            ++gpu_preview_generation_;
            changed = true;
        }
    }
    if (!comparison_before_base_image_.isNull())
    {
        QImage presented = apply_display_presentation_image(comparison_before_base_image_,
                                                            comparison_before_output_profile_);
        {
            const QMutexLocker lock(&preview_image_mutex_);
            if (presented.cacheKey() != comparison_before_image_.cacheKey())
            {
                comparison_before_image_ = std::move(presented);
                changed = true;
            }
        }
    }
    if (changed)
        emit previewChanged();
}

void StudioPresenter::clear_thumbnail_presentation_cache()
{
    thumbnail_base_paths_.clear();
    thumbnail_base_profiles_.clear();
    if (!thumbnail_presented_root_.isEmpty())
    {
        QDir(thumbnail_presented_root_).removeRecursively();
        thumbnail_presented_root_.clear();
    }
}

QUrl StudioPresenter::present_gallery_thumbnail_url(const std::string &asset_id,
                                                    const QString &base_path,
                                                    const ColorProfileState &source_profile)
{
    if (base_path.isEmpty() || !QFileInfo::exists(base_path))
        return {};
    if (display_presentation_ == nullptr || !display_presentation_->valid())
        return QUrl::fromLocalFile(base_path);

    QImage base(base_path);
    if (base.isNull())
        return QUrl::fromLocalFile(base_path);

    QImage presented = apply_display_presentation_image(base, source_profile);
    if (presented.isNull())
        return QUrl::fromLocalFile(base_path);

    if (thumbnail_presented_root_.isEmpty())
    {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        thumbnail_presented_root_ =
            QDir(root.isEmpty() ? QDir::tempPath() : root)
                .filePath(QStringLiteral("ravo-gallery-display-%1")
                              .arg(reinterpret_cast<quintptr>(this), 0, 16));
    }
    const QString fingerprint =
        qstring_from_utf8(display_presentation_->presentationState().profile_fingerprint);
    const QString dir_path =
        QDir(thumbnail_presented_root_)
            .filePath(fingerprint.isEmpty() ? QStringLiteral("fallback") : fingerprint);
    QDir().mkpath(dir_path);
    const QString out_path =
        QDir(dir_path).filePath(QString::fromStdString(asset_id) + QStringLiteral(".png"));
    if (!presented.save(out_path, "PNG"))
        return QUrl::fromLocalFile(base_path);
    return QUrl::fromLocalFile(out_path);
}

void StudioPresenter::remember_thumbnail_base(const std::string &asset_id, const QString &base_path,
                                              const ColorProfileState &source_profile,
                                              const QString &thumb_state)
{
    if (asset_id.empty() || base_path.isEmpty())
        return;
    thumbnail_base_paths_[asset_id] = base_path;
    thumbnail_base_profiles_[asset_id] = source_profile;
    const QUrl presented = present_gallery_thumbnail_url(asset_id, base_path, source_profile);
    assets_.setThumbnail(asset_id, presented.isEmpty() ? QUrl::fromLocalFile(base_path) : presented,
                         thumb_state);
}

void StudioPresenter::reapply_display_presentation_to_cached_thumbnails()
{
    if (thumbnail_base_paths_.empty())
        return;
    bool changed = false;
    for (const auto &[asset_id, base_path] : thumbnail_base_paths_)
    {
        if (!assets_.assetById(qstring_from_utf8(asset_id)))
            continue;
        const auto profile_it = thumbnail_base_profiles_.find(asset_id);
        const ColorProfileState profile =
            profile_it == thumbnail_base_profiles_.end() ? ColorProfileState{} : profile_it->second;
        const QString state = assets_.thumbnailState(asset_id);
        const QString publish_state = state.isEmpty() ? QStringLiteral("ready") : state;
        const QUrl presented = present_gallery_thumbnail_url(asset_id, base_path, profile);
        if (presented.isEmpty())
            continue;
        const int row = assets_.indexOf(qstring_from_utf8(asset_id));
        const QUrl current =
            row < 0 ? QUrl{} :
                      assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
        if (current == presented)
            continue;
        assets_.setThumbnail(asset_id, presented, publish_state);
        changed = true;
    }
    if (changed)
        emit thumbnailsChanged();
}

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
    comparison_before_base_image_ = QImage();
    comparison_before_output_profile_ = {};
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
    preview_output_profile_ = {};
    comparison_before_base_image_ = QImage();
    comparison_before_output_profile_ = {};
    preview_viewport_width_ = 0;
    preview_viewport_height_ = 0;
    preview_mask_alpha_.clear();
    live_preview_revision_ = 0;
    live_preview_width_ = 0;
    live_preview_height_ = 0;
    live_preview_color_profile_id_.clear();
    live_preview_pixel_sha256_.clear();
    displayed_develop_.reset();
    gpu_preview_generation_ = 0;
    gpu_preview_native_surface_ = 0;
    release_gpu_preview_presented_surface();
    gpu_preview_width_ = 0;
    gpu_preview_height_ = 0;
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
    const auto id = utf8_from_qstring(asset_id);
    const auto base = thumbnail_base_paths_.find(id);
    if (base != thumbnail_base_paths_.end() && QFileInfo::exists(base->second))
    {
        refresh_scopes(QImage(base->second));
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
    gpu_preview_generation_ = preview.gpu_display_generation;
    gpu_preview_native_surface_ = preview.gpu_display_native_surface;
    gpu_preview_width_ = static_cast<int>(preview.gpu_display_width);
    gpu_preview_height_ = static_cast<int>(preview.gpu_display_height);
    QImage owned;
    bool native_snapshot = false;
    if (preview.gpu_display_generation != 0U && preview.rgb.empty())
    {
        if (preview.gpu_display_native_surface == 0U || preview.gpu_display_width == 0U ||
            preview.gpu_display_height == 0U)
        {
            setError(QStringLiteral("GPU preview display is missing"));
            return;
        }
#if defined(Q_OS_MACOS)
        auto snapshot = studio_metal::snapshot_iosurface_rgb8(preview.gpu_display_native_surface,
                                                              preview.gpu_display_width,
                                                              preview.gpu_display_height);
        if (!snapshot)
        {
            setError(qstring_from_utf8(snapshot.error().message));
            return;
        }
        owned = std::move(snapshot).value();
        native_snapshot = true;
#else
        setError(QStringLiteral("GPU preview display cannot be captured on this platform"));
        return;
#endif
    }
    else
    {
        auto prepared = preview_result_image(preview);
        if (!prepared)
        {
            setError(qstring_from_utf8(prepared.error().message));
            return;
        }
        owned = std::move(prepared).value();
    }
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
    preview_output_profile_ = preview.color_profile;
    preview_base_image_ = displayed;
    QImage presented = apply_display_presentation_image(displayed, preview_output_profile_);
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = presented;
    }
    // DISPLAY-01: never expose Engine IOSurface to QML; publish C++-presented RGB8.
    if (gpu_preview_generation_ != 0U)
    {
        if (!publish_gpu_preview_presented_surface(presented))
        {
            release_gpu_preview_presented_surface();
            gpu_preview_generation_ = 0;
            gpu_preview_native_surface_ = 0;
            gpu_preview_width_ = 0;
            gpu_preview_height_ = 0;
        }
    }
    else
    {
        release_gpu_preview_presented_surface();
        gpu_preview_native_surface_ = 0;
        gpu_preview_width_ = 0;
        gpu_preview_height_ = 0;
    }
    const QSize viewport_size =
        stable_preview_viewport_size(QSize(preview_viewport_width_, preview_viewport_height_),
                                     presented.size(), preserve_viewport_extent);
    preview_viewport_width_ = viewport_size.width();
    preview_viewport_height_ = viewport_size.height();
    live_preview_revision_ = revision;
    live_preview_width_ = static_cast<std::uint32_t>(std::max(0, presented.width()));
    live_preview_height_ = static_cast<std::uint32_t>(std::max(0, presented.height()));
    live_preview_color_profile_id_ = native_snapshot ?
                                         QStringLiteral("srgb") :
                                         qstring_from_utf8(preview.color_profile.identifier);
    if (live_preview_color_profile_id_.isEmpty() && displayed.colorSpace().isValid())
    {
        live_preview_color_profile_id_ = displayed.colorSpace().description();
        if (live_preview_color_profile_id_.isEmpty())
            live_preview_color_profile_id_ = QStringLiteral("embedded-icc");
    }
    live_preview_pixel_sha256_.clear();
    preview_url_ = !preview.rgb.empty() || native_snapshot ?
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
    comparison_before_output_profile_ = preview.color_profile;
    comparison_before_base_image_ = std::move(prepared).value();
    QImage presented = apply_display_presentation_image(comparison_before_base_image_,
                                                        comparison_before_output_profile_);
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = std::move(presented);
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
                        const QString thumb_state =
                            preview.value().media_state == "proxy" ?
                                QStringLiteral("proxy") :
                                (preview.value().original_missing ? QStringLiteral("missing") :
                                                                    QStringLiteral("ready"));
                        remember_thumbnail_base(id, qstring_from_utf8(preview.value().cache_path),
                                                preview.value().color_profile, thumb_state);
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
    if (browse_mode_ == QLatin1String("survey"))
    {
        preview_loading_ = false;
        emit previewChanged();
        requestSurveyPreviews();
        return;
    }
    enqueue_preview();
}

void StudioPresenter::rebuild_survey_slots()
{
    survey_slot_ids_.clear();
    if (burst_compare_slot_ids_.size() >= static_cast<std::size_t>(kSurveySlotMinimum))
    {
        const std::size_t count =
            std::min(burst_compare_slot_ids_.size(), static_cast<std::size_t>(kSurveySlotMaximum));
        survey_slot_ids_.assign(burst_compare_slot_ids_.begin(),
                                burst_compare_slot_ids_.begin() +
                                    static_cast<std::ptrdiff_t>(count));
    }
    else
    {
        const auto ids = selected_asset_ids();
        std::size_t count = 0U;
        if (ids.size() >= static_cast<std::size_t>(kSurveySlotMaximum))
            count = static_cast<std::size_t>(kSurveySlotMaximum);
        else if (ids.size() >= static_cast<std::size_t>(kSurveySlotMinimum))
            count = static_cast<std::size_t>(kSurveySlotMinimum);
        survey_slot_ids_.assign(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count));
    }
    for (auto it = survey_preview_urls_.begin(); it != survey_preview_urls_.end();)
    {
        if (std::find(survey_slot_ids_.begin(), survey_slot_ids_.end(), it->first) ==
            survey_slot_ids_.end())
            it = survey_preview_urls_.erase(it);
        else
            ++it;
    }
}

void StudioPresenter::requestSurveyPreviews()
{
    rebuild_survey_slots();
    emit surveyChanged();
    for (const auto &id : survey_slot_ids_)
    {
        if (survey_preview_urls_.contains(id) || survey_preview_requests_.contains(id))
            continue;
        if (std::find(pending_survey_ids_.begin(), pending_survey_ids_.end(), id) !=
            pending_survey_ids_.end())
            continue;
        pending_survey_ids_.push_back(id);
    }
    if (!survey_preview_in_flight_ && !pending_survey_ids_.empty())
    {
        auto next = pending_survey_ids_.front();
        pending_survey_ids_.pop_front();
        startSurveyPreviewRequest(std::move(next));
    }
}

void StudioPresenter::startSurveyPreviewRequest(std::string id)
{
    const auto revision = ++survey_preview_revision_;
    survey_preview_in_flight_ = true;
    survey_preview_requests_[id] = revision;
    const auto cancellation = thumbnail_work_.token();
    static_cast<void>(executor_.post(
        [this, id, revision, cancellation]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kDefaultPreviewMaxEdge;
                request.request_revision = revision;
                request.purpose = PreviewPurpose::kBrowse;
                request.prefer_embedded_preview = false;
                request.cancellation = cancellation;
                preview = service_->request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = survey_preview_requests_.find(id);
                    if (latest == survey_preview_requests_.end() || latest->second != revision)
                    {
                        finishSurveyPreviewRequest(false);
                        return;
                    }
                    survey_preview_requests_.erase(latest);
                    if (preview)
                    {
                        survey_preview_urls_[id] =
                            QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path));
                        emit surveyChanged();
                        finishSurveyPreviewRequest(true);
                        return;
                    }
                    finishSurveyPreviewRequest(false);
                },
                Qt::QueuedConnection);
        }));
}

void StudioPresenter::finishSurveyPreviewRequest(const bool success)
{
    static_cast<void>(success);
    survey_preview_in_flight_ = false;
    if (browse_mode_ != QLatin1String("survey") || pending_survey_ids_.empty())
        return;
    auto next = pending_survey_ids_.front();
    pending_survey_ids_.pop_front();
    startSurveyPreviewRequest(std::move(next));
}

QUrl StudioPresenter::inspectRoiUrl() const
{
    return inspect_roi_url_;
}

QImage StudioPresenter::inspectRoiImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return inspect_roi_image_;
}

double StudioPresenter::inspectRoiX() const noexcept
{
    return inspect_roi_x_;
}

double StudioPresenter::inspectRoiY() const noexcept
{
    return inspect_roi_y_;
}

double StudioPresenter::inspectRoiWidth() const noexcept
{
    return inspect_roi_width_;
}

double StudioPresenter::inspectRoiHeight() const noexcept
{
    return inspect_roi_height_;
}

quint64 StudioPresenter::gpuPreviewGeneration() const noexcept
{
    return gpu_preview_generation_;
}

quint64 StudioPresenter::gpuPreviewNativeSurface() const noexcept
{
    return gpu_preview_native_surface_;
}

int StudioPresenter::gpuPreviewWidth() const noexcept
{
    return gpu_preview_width_;
}

int StudioPresenter::gpuPreviewHeight() const noexcept
{
    return gpu_preview_height_;
}

quint64 StudioPresenter::gpuRoiGeneration() const noexcept
{
    return gpu_roi_generation_;
}

quint64 StudioPresenter::gpuRoiNativeSurface() const noexcept
{
    return gpu_roi_native_surface_;
}

int StudioPresenter::gpuRoiWidth() const noexcept
{
    return gpu_roi_width_;
}

int StudioPresenter::gpuRoiHeight() const noexcept
{
    return gpu_roi_height_;
}

void StudioPresenter::refresh_inspect_roi()
{
    if (zoom_mode_ != QLatin1String("actual") || selected_asset_id_.isEmpty() ||
        inspect_roi_width_ <= 0.0 || inspect_roi_height_ <= 0.0)
    {
        return;
    }
    requestInspectRoi(inspect_roi_x_, inspect_roi_y_, inspect_roi_width_, inspect_roi_height_);
}

void StudioPresenter::clear_inspect_roi()
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
    gpu_roi_native_surface_ = 0;
    release_gpu_roi_presented_surface();
    gpu_roi_width_ = 0;
    gpu_roi_height_ = 0;
    if (had_roi)
        emit inspectRoiChanged();
}

void StudioPresenter::requestInspectRoi(const double x, const double y, const double width,
                                        const double height)
{
    if (zoom_mode_ != QLatin1String("actual") || selected_asset_id_.isEmpty() ||
        service_ == nullptr)
    {
        clear_inspect_roi();
        return;
    }
    PreviewNormRect roi{x, y, width, height};
    const auto revision = inspect_roi_owner_.supersede("inspect_roi_requested");
    const auto cancellation = inspect_roi_owner_.begin();
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const auto params = develop_;
    static_cast<void>(executor_.post(
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
            if (service_ != nullptr)
            {
                preview = service_->request_preview(request, params);
            }
            QMetaObject::invokeMethod(
                this,
                [this, roi, revision, asset_id, preview = std::move(preview)]() mutable
                {
                    if (!inspect_roi_owner_.accepts(revision, asset_id,
                                                    utf8_from_qstring(selected_asset_id_)) ||
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
                            setError(qstring_from_utf8(preview.error().message));
                        }
                        return;
                    }
                    gpu_roi_generation_ = preview.value().gpu_display_generation;
                    gpu_roi_native_surface_ = 0;
                    gpu_roi_width_ = static_cast<int>(preview.value().gpu_display_width);
                    gpu_roi_height_ = static_cast<int>(preview.value().gpu_display_height);
                    QImage roi_base;
                    if (preview.value().gpu_display_generation != 0U && preview.value().rgb.empty())
                    {
                        if (preview.value().gpu_display_native_surface == 0U)
                        {
                            setError(QStringLiteral("GPU inspect display is missing"));
                            return;
                        }
#if defined(Q_OS_MACOS)
                        auto snapshot = studio_metal::snapshot_iosurface_rgb8(
                            preview.value().gpu_display_native_surface,
                            preview.value().gpu_display_width, preview.value().gpu_display_height);
                        if (!snapshot)
                        {
                            setError(qstring_from_utf8(snapshot.error().message));
                            return;
                        }
                        roi_base = std::move(snapshot).value();
#else
                        setError(QStringLiteral(
                            "GPU inspect display cannot be captured on this platform"));
                        return;
#endif
                    }
                    else
                    {
                        auto prepared = preview_result_image(preview.value());
                        if (!prepared)
                        {
                            setError(qstring_from_utf8(prepared.error().message));
                            return;
                        }
                        roi_base = std::move(prepared).value();
                        const QMutexLocker lock(&preview_image_mutex_);
                        inspect_roi_image_ = roi_base;
                    }
                    if (gpu_roi_generation_ != 0U)
                    {
                        QImage presented_roi = apply_display_presentation_image(
                            roi_base, preview.value().color_profile);
                        if (!publish_gpu_roi_presented_surface(presented_roi))
                        {
                            release_gpu_roi_presented_surface();
                            gpu_roi_generation_ = 0;
                            gpu_roi_native_surface_ = 0;
                            gpu_roi_width_ = 0;
                            gpu_roi_height_ = 0;
                            setError(QStringLiteral("GPU inspect presentation publish failed"));
                            return;
                        }
                    }
                    else
                    {
                        release_gpu_roi_presented_surface();
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
        }));
}

} // namespace ravo
