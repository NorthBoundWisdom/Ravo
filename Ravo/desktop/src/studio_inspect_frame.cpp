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
namespace
{
[[nodiscard]] Result<QImage> preview_result_image(const PreviewResult &preview,
                                                  QCache<QString, QImage> &decoded_images)
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
    const QFileInfo file(qstring_from_utf8(preview.cache_path));
    if (!file.isFile())
        return make_error(ErrorCode::kIo, "Preview resource could not be loaded",
                          {{"path", preview.cache_path}});
    const QString key = file.absoluteFilePath() + QLatin1Char('\n') + QString::number(file.size()) +
                        QLatin1Char('\n') +
                        QString::number(file.lastModified().toMSecsSinceEpoch());
    if (const auto *cached = decoded_images.object(key))
        return *cached;
    QImage cached(file.absoluteFilePath());
    if (cached.isNull())
    {
        return make_error(ErrorCode::kIo, "Preview resource could not be loaded",
                          {{"path", preview.cache_path}});
    }
    const auto cost = std::max<qsizetype>(1024, (cached.sizeInBytes() + 1023) / 1024);
    if (cost <= decoded_images.maxCost())
        decoded_images.insert(key, new QImage(cached), static_cast<int>(cost));
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
namespace
{
Result<std::uint64_t> create_presented_surface(const QImage &presented)
{
    if (presented.isNull() || presented.width() <= 0 || presented.height() <= 0)
        return make_error(ErrorCode::kValidation, "GPU presentation image is empty");
    auto created =
        studio_metal::create_iosurface_rgba8(static_cast<std::uint32_t>(presented.width()),
                                             static_cast<std::uint32_t>(presented.height()));
    if (!created)
        return created.error();
    auto written = studio_metal::write_rgb8_to_iosurface(created.value(), presented);
    if (!written)
    {
        studio_metal::release_iosurface(created.value());
        return written.error();
    }
    return created;
}
} // namespace
void StudioInspectPresenter::release_gpu_preview_presented_surface()
{
    if (gpu_preview_presented_surface_ != 0U)
    {
        studio_metal::release_iosurface(gpu_preview_presented_surface_);
        gpu_preview_presented_surface_ = 0U;
    }
}

void StudioInspectPresenter::release_gpu_roi_presented_surface()
{
    if (gpu_roi_presented_surface_ != 0U)
    {
        studio_metal::release_iosurface(gpu_roi_presented_surface_);
        gpu_roi_presented_surface_ = 0U;
    }
}

bool StudioInspectPresenter::publish_gpu_preview_presented_surface(const QImage &presented)
{
    auto created = create_presented_surface(presented);
    if (!created)
    {
        emit errorOccurred(qstring_from_utf8(created.error().message));
        return false;
    }
    release_gpu_preview_presented_surface();
    gpu_preview_presented_surface_ = created.value();

    gpu_preview_width_ = presented.width();
    gpu_preview_height_ = presented.height();
    return true;
}

bool StudioInspectPresenter::publish_gpu_roi_presented_surface(const QImage &presented)
{
    auto created = create_presented_surface(presented);
    if (!created)
    {
        emit errorOccurred(qstring_from_utf8(created.error().message));
        return false;
    }
    release_gpu_roi_presented_surface();
    gpu_roi_presented_surface_ = created.value();

    gpu_roi_width_ = presented.width();
    gpu_roi_height_ = presented.height();
    return true;
}
#else
void StudioInspectPresenter::release_gpu_preview_presented_surface()
{
}
void StudioInspectPresenter::release_gpu_roi_presented_surface()
{
}
bool StudioInspectPresenter::publish_gpu_preview_presented_surface(const QImage &)
{
    emit errorOccurred(QStringLiteral("GPU preview presentation transport is unavailable"));
    return false;
}
bool StudioInspectPresenter::publish_gpu_roi_presented_surface(const QImage &)
{
    emit errorOccurred(QStringLiteral("GPU inspect presentation transport is unavailable"));
    return false;
}
#endif

void StudioInspectPresenter::notifyPreviewChanged()
{
    emit previewChanged();
}
void StudioInspectPresenter::seedViewport(int width, int height)
{
    preview_viewport_width_ = width;
    preview_viewport_height_ = height;
}
void StudioInspectPresenter::setPreviewLoading(bool loading)
{
    preview_loading_ = loading;
}
void StudioInspectPresenter::clearDecodedImages()
{
    decoded_preview_images_.clear();
}
void StudioInspectPresenter::clearComparisonImages()
{
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = {};
    }
    comparison_before_base_image_ = {};
    comparison_before_output_profile_ = {};
    comparison_before_url_.clear();
}
bool StudioInspectPresenter::adoptPreviewAsComparison()
{
    if (stopped_)
    {
        emit errorOccurred(QStringLiteral("Inspect owner is closed"));
        return false;
    }
    QImage before;
    {
        const QMutexLocker lock(&preview_image_mutex_);
        before = preview_image_;
    }
    if (before.isNull())
        return false;
    comparison_before_base_image_ = preview_base_image_;
    comparison_before_output_profile_ = preview_output_profile_;
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = std::move(before);
    }
    comparison_before_url_ =
        QUrl(QStringLiteral("image://studioPreview/before?r=%1").arg(live_preview_revision_));
    return true;
}
void StudioInspectPresenter::restorePreviewBase()
{
    if (stopped_)
    {
        emit errorOccurred(QStringLiteral("Inspect owner is closed"));
        return;
    }
    auto presentation =
        apply_display_presentation_image(preview_base_image_, preview_output_profile_);
    if (!presentation)
    {
        emit errorOccurred(qstring_from_utf8(presentation.error().message));
        return;
    }
    QImage presented = std::move(presentation).value();
    if (gpu_preview_generation_ != 0U)
    {
        if (!publish_gpu_preview_presented_surface(presented))
            return;
        ++gpu_preview_generation_;
    }
    const QMutexLocker lock(&preview_image_mutex_);
    preview_image_ = std::move(presented);
}
QImage StudioInspectPresenter::previewBaseImage() const
{
    return preview_base_image_;
}
std::uint64_t StudioInspectPresenter::frameRevision() const noexcept
{
    return live_preview_revision_;
}
std::uint32_t StudioInspectPresenter::frameWidth() const noexcept
{
    return live_preview_width_;
}
std::uint32_t StudioInspectPresenter::frameHeight() const noexcept
{
    return live_preview_height_;
}
QString StudioInspectPresenter::frameColorProfile() const
{
    return live_preview_color_profile_id_;
}
Result<QImage> StudioInspectPresenter::preparePreview(const PreviewResult &preview)
{
    return preview_result_image(preview, decoded_preview_images_);
}
void StudioInspectPresenter::releasePresentationResources()
{
    release_gpu_preview_presented_surface();
    release_gpu_roi_presented_surface();
}
void StudioInspectPresenter::bindDisplayPresentation(StudioDisplayPresentation *owner)
{
    if (display_presentation_ == owner)
        return;
    if (display_presentation_ != nullptr)
        disconnect(display_presentation_, nullptr, this, nullptr);
    display_presentation_ = owner;
    if (display_presentation_ == nullptr)
        return;
    connect(display_presentation_, &StudioDisplayPresentation::stateChanged, this,
            &StudioInspectPresenter::reapply_display_presentation_to_cached_previews);
    reapply_display_presentation_to_cached_previews();
}

QUrl StudioInspectPresenter::previewUrl() const
{
    return preview_url_;
}

int StudioInspectPresenter::previewViewportWidth() const noexcept
{
    return preview_viewport_width_;
}

int StudioInspectPresenter::previewViewportHeight() const noexcept
{
    return preview_viewport_height_;
}

QImage StudioInspectPresenter::previewImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return preview_image_;
}

QUrl StudioInspectPresenter::comparisonBeforeUrl() const
{
    return comparison_before_url_;
}

QImage StudioInspectPresenter::comparisonBeforeImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return comparison_before_image_;
}

void StudioInspectPresenter::clear_displayed_preview()
{
    cancel_preview_analysis("preview_cleared");
    host_.clear_comparison();
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = QImage();
        preview_url_.clear();
    }
    preview_base_image_ = QImage();
    setBaseSource({});
    preview_output_profile_ = {};
    comparison_before_base_image_ = QImage();
    comparison_before_output_profile_ = {};
    preview_viewport_width_ = 0;
    preview_viewport_height_ = 0;
    observeFrameLayout({});
    live_preview_revision_ = 0;
    live_preview_width_ = 0;
    live_preview_height_ = 0;
    live_preview_color_profile_id_.clear();
    resetIdentity();
    observeDisplayedDevelop({});
    gpu_preview_generation_ = 0;

    release_gpu_preview_presented_surface();
    gpu_preview_width_ = 0;
    gpu_preview_height_ = 0;
    clear_scopes();
}

bool StudioInspectPresenter::show_preview_result(const PreviewResult &preview,
                                                 const std::uint64_t revision,
                                                 const bool preserve_viewport_extent)
{
    if (stopped_)
    {
        emit errorOccurred(QStringLiteral("Inspect owner is closed"));
        return false;
    }
    auto prepared = preparePreview(preview);
    if (!prepared)
    {
        emit errorOccurred(qstring_from_utf8(prepared.error().message));
        return false;
    }
    QImage owned = std::move(prepared).value();
    QImage displayed = owned;
    if (host_.mask_overlay_visible() && !preview.mask_alpha.empty() && context_.engine.has_value())
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
        auto composited = context_.engine->composite_preview_mask_overlay(
            rgb, static_cast<std::uint32_t>(owned.width()),
            static_cast<std::uint32_t>(owned.height()), preview.mask_alpha, {});
        if (!composited)
        {
            emit errorOccurred(qstring_from_utf8(composited.error().message));
            return false;
        }
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
    ColorProfileState output_profile = preview.color_profile;
    if (!preview.cache_path.empty() && displayed.colorSpace().isValid())
    {
        const auto icc = displayed.colorSpace().iccProfile();
        output_profile.kind = ColorProfileKind::kIcc;
        output_profile.model = ColorModel::kRgb;
        output_profile.identifier = "embedded_icc";
        const auto *begin = reinterpret_cast<const std::uint8_t *>(icc.constData());
        output_profile.icc_bytes.assign(begin, begin + icc.size());
    }
    auto presentation = apply_display_presentation_image(displayed, output_profile);
    if (!presentation)
    {
        emit errorOccurred(qstring_from_utf8(presentation.error().message));
        return false;
    }
    QImage presented = std::move(presentation).value();
    // DISPLAY-01: never expose Engine IOSurface to QML; publish C++-presented RGB8.
    if (preview.gpu_display_generation != 0U)
    {
        if (!publish_gpu_preview_presented_surface(presented))
        {
            return false;
        }
    }
    else
    {
        release_gpu_preview_presented_surface();

        gpu_preview_width_ = 0;
        gpu_preview_height_ = 0;
    }
    gpu_preview_generation_ = preview.gpu_display_generation;
    preview_output_profile_ = std::move(output_profile);
    preview_base_image_ = displayed;
    setBaseSource(preview_base_image_);
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = presented;
    }
    observeFrameLayout({});
    if (preview.crop_geometry)
    {
        const auto &geometry = *preview.crop_geometry;
        observeFrameLayout({{"x", geometry.region.x},
                            {"y", geometry.region.y},
                            {"width", geometry.region.width},
                            {"height", geometry.region.height},
                            {"widthScale", geometry.width_scale},
                            {"heightScale", geometry.height_scale}});
    }
    const QSize viewport_size =
        stable_preview_viewport_size(QSize(preview_viewport_width_, preview_viewport_height_),
                                     presented.size(), preserve_viewport_extent);
    preview_viewport_width_ = viewport_size.width();
    preview_viewport_height_ = viewport_size.height();
    live_preview_revision_ = revision;
    live_preview_width_ = static_cast<std::uint32_t>(std::max(0, presented.width()));
    live_preview_height_ = static_cast<std::uint32_t>(std::max(0, presented.height()));
    live_preview_color_profile_id_ = qstring_from_utf8(preview.color_profile.identifier);
    if (live_preview_color_profile_id_.isEmpty() && displayed.colorSpace().isValid())
    {
        live_preview_color_profile_id_ = displayed.colorSpace().description();
        if (live_preview_color_profile_id_.isEmpty())
            live_preview_color_profile_id_ = QStringLiteral("embedded-icc");
    }
    resetIdentity();
    preview_url_ = !preview.rgb.empty() || preserve_viewport_extent ?
                       QUrl(QStringLiteral("image://studioPreview/live?r=%1").arg(revision)) :
                       QUrl::fromLocalFile(qstring_from_utf8(preview.cache_path));
    schedule_preview_analysis(displayed, owned, revision, preview.asset_id,
                              live_preview_color_profile_id_);
    return true;
}

void StudioInspectPresenter::show_comparison_before_result(const PreviewResult &preview,
                                                           const std::uint64_t revision)
{
    if (stopped_)
    {
        emit errorOccurred(QStringLiteral("Inspect owner is closed"));
        return;
    }
    auto prepared = preparePreview(preview);
    if (!prepared)
    {
        emit errorOccurred(qstring_from_utf8(prepared.error().message));
        return;
    }
    QImage base = std::move(prepared).value();
    auto presentation = apply_display_presentation_image(base, preview.color_profile);
    if (!presentation)
    {
        emit errorOccurred(qstring_from_utf8(presentation.error().message));
        return;
    }
    QImage presented = std::move(presentation).value();
    comparison_before_output_profile_ = preview.color_profile;
    comparison_before_base_image_ = std::move(base);
    {
        const QMutexLocker lock(&preview_image_mutex_);
        comparison_before_image_ = std::move(presented);
    }
    comparison_before_url_ =
        QUrl(QStringLiteral("image://studioPreview/before?r=%1").arg(revision));
}

bool StudioInspectPresenter::previewLoading() const noexcept
{
    return preview_loading_;
}

Result<QImage> StudioInspectPresenter::apply_display_presentation_image(
    const QImage &output_referred, const ColorProfileState &source_profile) const
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
        return converted.error();

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

void StudioInspectPresenter::reapply_display_presentation_to_cached_previews()
{
    if (stopped_)
        return;
    bool changed = false;
    if (!preview_base_image_.isNull())
    {
        auto presentation =
            apply_display_presentation_image(preview_base_image_, preview_output_profile_);
        if (!presentation)
            emit errorOccurred(qstring_from_utf8(presentation.error().message));
        else
        {
            QImage presented = std::move(presentation).value();
            const bool native = gpu_preview_generation_ != 0U;
            if (!native || publish_gpu_preview_presented_surface(presented))
            {
                {
                    const QMutexLocker lock(&preview_image_mutex_);
                    if (presented.cacheKey() != preview_image_.cacheKey())
                    {
                        preview_image_ = presented;
                        changed = true;
                    }
                }
                if (native)
                {
                    ++gpu_preview_generation_;
                    changed = true;
                }
            }
        }
    }
    if (!comparison_before_base_image_.isNull())
    {
        auto presentation = apply_display_presentation_image(comparison_before_base_image_,
                                                             comparison_before_output_profile_);
        if (!presentation)
            emit errorOccurred(qstring_from_utf8(presentation.error().message));
        else
        {
            QImage presented = std::move(presentation).value();
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
} // namespace ravo
