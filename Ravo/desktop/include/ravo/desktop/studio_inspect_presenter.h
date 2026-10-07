#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <QCache>
#include <QPointer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/domain/types.h"
#include "ravo/desktop/preview_request_owner.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/foundation/executor.h"

namespace ravo
{
class PreviewService;
class StudioDisplayPresentation;
// GUI-thread presentation/identity owner. Worker inputs are owned QImage
// snapshots. Observed selection is read-only context, never a selection writer.
// Image-provider getters return synchronized value snapshots from any thread.
class StudioInspectPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportWidth READ previewViewportWidth NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportHeight READ previewViewportHeight NOTIFY previewChanged)
    Q_PROPERTY(QUrl inspectRoiUrl READ inspectRoiUrl NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiX READ inspectRoiX NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiY READ inspectRoiY NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiWidth READ inspectRoiWidth NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiHeight READ inspectRoiHeight NOTIFY inspectRoiChanged)
    Q_PROPERTY(quint64 gpuPreviewGeneration READ gpuPreviewGeneration NOTIFY previewChanged)
    Q_PROPERTY(quint64 gpuPreviewNativeSurface READ gpuPreviewNativeSurface NOTIFY previewChanged)
    Q_PROPERTY(int gpuPreviewWidth READ gpuPreviewWidth NOTIFY previewChanged)
    Q_PROPERTY(int gpuPreviewHeight READ gpuPreviewHeight NOTIFY previewChanged)
    Q_PROPERTY(quint64 gpuRoiGeneration READ gpuRoiGeneration NOTIFY inspectRoiChanged)
    Q_PROPERTY(quint64 gpuRoiNativeSurface READ gpuRoiNativeSurface NOTIFY inspectRoiChanged)
    Q_PROPERTY(int gpuRoiWidth READ gpuRoiWidth NOTIFY inspectRoiChanged)
    Q_PROPERTY(int gpuRoiHeight READ gpuRoiHeight NOTIFY inspectRoiChanged)
    Q_PROPERTY(QUrl comparisonBeforeUrl READ comparisonBeforeUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)

    Q_PROPERTY(QVariantMap cropPreviewLayout READ cropPreviewLayout NOTIFY frameChanged)
    Q_PROPERTY(QString scopeMode READ scopeMode WRITE setScopeMode NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramRed READ scopeHistogramRed NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramGreen READ scopeHistogramGreen NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramBlue READ scopeHistogramBlue NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramLuma READ scopeHistogramLuma NOTIFY scopesChanged)
    Q_PROPERTY(double scopeHistogramMax READ scopeHistogramMax NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeParadeUrl READ scopeParadeUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeWaveformUrl READ scopeWaveformUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeVectorscopeUrl READ scopeVectorscopeUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeSplitUrl READ scopeSplitUrl NOTIFY scopesChanged)

public:
    struct Context
    {
        const QString &selected_asset_id;
        const QString &catalog_path;
        const std::optional<EngineFacade> &engine;
        SerialExecutor &executor;
    };
    struct Host
    {
        // preview_service is called only on the foreground executor; all other
        // callbacks observe transient Develop view state on the GUI thread.
        std::function<PreviewService *()> preview_service;
        std::function<DevelopParams()> develop_params;
        std::function<bool()> mask_overlay_visible;
        std::function<void()> clear_comparison;
    };
    explicit StudioInspectPresenter(Context context, Host host, QObject *parent = nullptr);
    void notifyPreviewChanged();
    void seedViewport(int width, int height);
    void setPreviewLoading(bool loading);
    void clearDecodedImages();
    void clearComparisonImages();
    [[nodiscard]] bool adoptPreviewAsComparison();
    void restorePreviewBase();
    [[nodiscard]] QImage previewBaseImage() const;
    [[nodiscard]] std::uint64_t frameRevision() const noexcept;
    [[nodiscard]] std::uint32_t frameWidth() const noexcept;
    [[nodiscard]] std::uint32_t frameHeight() const noexcept;
    [[nodiscard]] QString frameColorProfile() const;
    void bindDisplayPresentation(StudioDisplayPresentation *owner);
    void releasePresentationResources();

    ~StudioInspectPresenter() override;
    void shutdown();
    void observeSelection(QString asset_id);
    void setBaseSource(QImage image);
    void resetIdentity();
    [[nodiscard]] QString pixelSha256() const;
    [[nodiscard]] bool identityPending() const noexcept;
    [[nodiscard]] RgbHistogram histogramSnapshot() const;
    [[nodiscard]] const std::optional<DevelopParams> &displayedDevelop() const noexcept;
    void observeDisplayedDevelop(std::optional<DevelopParams> params);
    [[nodiscard]] const QVariantMap &cropPreviewLayout() const noexcept;
    void observeFrameLayout(QVariantMap layout);
    [[nodiscard]] QString scopeMode() const;
    void setScopeMode(const QString &mode);
    [[nodiscard]] QVariantList scopeHistogramRed() const;
    [[nodiscard]] QVariantList scopeHistogramGreen() const;
    [[nodiscard]] QVariantList scopeHistogramBlue() const;
    [[nodiscard]] QVariantList scopeHistogramLuma() const;
    [[nodiscard]] double scopeHistogramMax() const noexcept;
    [[nodiscard]] QUrl scopeParadeUrl() const;
    [[nodiscard]] QImage scopeParadeImage() const;
    [[nodiscard]] QUrl scopeWaveformUrl() const;
    [[nodiscard]] QImage scopeWaveformImage() const;
    [[nodiscard]] QUrl scopeVectorscopeUrl() const;
    [[nodiscard]] QImage scopeVectorscopeImage() const;
    [[nodiscard]] QUrl scopeSplitUrl() const;
    [[nodiscard]] QImage scopeSplitImage() const;
    void schedule_preview_analysis(const QImage &identity_image, const QImage &scope_image,
                                   std::uint64_t preview_revision, const std::string &asset_id,
                                   const QString &profile_id);
    void cancel_preview_analysis(std::string reason);
    void refresh_scopes(const QImage &image);
    void clear_scopes();

    [[nodiscard]] bool show_preview_result(const PreviewResult &preview, std::uint64_t revision,
                                           bool preserve_viewport_extent);
    [[nodiscard]] quint64 gpuPreviewGeneration() const noexcept;
    [[nodiscard]] double inspectRoiHeight() const noexcept;
    [[nodiscard]] QUrl inspectRoiUrl() const;
    [[nodiscard]] int gpuRoiWidth() const noexcept;
    [[nodiscard]] bool previewLoading() const noexcept;
    [[nodiscard]] quint64 gpuPreviewNativeSurface() const noexcept;
    [[nodiscard]] QImage comparisonBeforeImage() const;
    Q_INVOKABLE void requestInspectRoi(double x, double y, double width, double height);
    void show_comparison_before_result(const PreviewResult &preview, std::uint64_t revision);
    [[nodiscard]] int gpuPreviewHeight() const noexcept;
    [[nodiscard]] double inspectRoiX() const noexcept;
    [[nodiscard]] int previewViewportHeight() const noexcept;
    [[nodiscard]] QImage previewImage() const;
    [[nodiscard]] quint64 gpuRoiNativeSurface() const noexcept;
    [[nodiscard]] int gpuRoiHeight() const noexcept;
    void clear_displayed_preview();
    void refresh_inspect_roi();
    Q_INVOKABLE void adjustZoom(int wheel_delta);
    [[nodiscard]] QUrl previewUrl() const;
    [[nodiscard]] QString zoomMode() const;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] QImage inspectRoiImage() const;
    void clear_inspect_roi();
    [[nodiscard]] QUrl comparisonBeforeUrl() const;
    Q_INVOKABLE void setZoomMode(const QString &mode);
    [[nodiscard]] int previewViewportWidth() const noexcept;
    Q_INVOKABLE void toggleActualSize();
    [[nodiscard]] double inspectRoiY() const noexcept;
    Q_INVOKABLE void setZoomFactor(double factor);
    [[nodiscard]] int gpuPreviewWidth() const noexcept;
    [[nodiscard]] quint64 gpuRoiGeneration() const noexcept;
    [[nodiscard]] double inspectRoiWidth() const noexcept;

signals:
    void previewChanged();
    void inspectRoiChanged();
    void zoomChanged();
    void inspectContextChanged();
    void frameChanged();
    void scopesChanged();
    void identityChanged();
    void errorOccurred(QString error);

private:
    void reapply_display_presentation_to_cached_previews();
    [[nodiscard]] bool publish_gpu_preview_presented_surface(const QImage &presented);
    void release_gpu_roi_presented_surface();
    void release_gpu_preview_presented_surface();
    [[nodiscard]] bool publish_gpu_roi_presented_surface(const QImage &presented);
    [[nodiscard]] Result<QImage>
    apply_display_presentation_image(const QImage &output_referred,
                                     const ColorProfileState &source_profile) const;
    [[nodiscard]] Result<QImage> preparePreview(const PreviewResult &preview);
    Context context_;
    Host host_;
    QPointer<StudioDisplayPresentation> display_presentation_;
    QUrl preview_url_;
    QUrl comparison_before_url_;
    QImage preview_image_;
    QImage comparison_before_image_;
    QImage preview_base_image_;
    QCache<QString, QImage> decoded_preview_images_{64 * 1024};
    QImage comparison_before_base_image_;
    ColorProfileState preview_output_profile_;
    ColorProfileState comparison_before_output_profile_;
    int preview_viewport_width_ = 0;
    int preview_viewport_height_ = 0;
    std::uint64_t live_preview_revision_ = 0;
    std::uint32_t live_preview_width_ = 0;
    std::uint32_t live_preview_height_ = 0;
    QString live_preview_color_profile_id_;
    mutable QMutex preview_image_mutex_;
    QString zoom_mode_{QStringLiteral("fit")};
    double zoom_factor_ = 1.0;
    QString last_non_actual_zoom_mode_{QStringLiteral("fit")};
    double last_non_actual_zoom_factor_ = 1.0;
    bool preview_loading_ = false;
    PreviewRequestOwner inspect_roi_owner_;
    QUrl inspect_roi_url_;
    QImage inspect_roi_image_;
    double inspect_roi_x_ = 0.0;
    double inspect_roi_y_ = 0.0;
    double inspect_roi_width_ = 0.0;
    double inspect_roi_height_ = 0.0;
    quint64 gpu_preview_generation_ = 0;
    quint64 gpu_preview_presented_surface_ = 0;
    int gpu_preview_width_ = 0;
    int gpu_preview_height_ = 0;
    quint64 gpu_roi_generation_ = 0;
    quint64 gpu_roi_presented_surface_ = 0;
    int gpu_roi_width_ = 0;
    int gpu_roi_height_ = 0;
    void drain_preview_analysis();
    [[nodiscard]] static QVariantList
    histogram_channel_list(const std::array<std::uint32_t, kRgbHistogramBins> &channel);
    SerialExecutor preview_analysis_executor_;
    QMutex preview_analysis_queue_mutex_;
    std::optional<std::function<void()>> pending_preview_analysis_;
    bool preview_analysis_worker_active_ = false;
    PreviewRequestOwner preview_analysis_owner_;
    mutable QMutex images_mutex_;
    QString observed_selection_;
    std::uint64_t observed_frame_revision_ = 0;
    QImage base_source_;
    QString live_preview_pixel_sha256_;
    bool preview_identity_pending_ = false;
    bool stopped_ = false;
    std::optional<DevelopParams> displayed_develop_;
    QVariantMap crop_preview_layout_;
    QString scope_mode_{QStringLiteral("parade")};
    RgbHistogram scope_histogram_{};
    QImage scope_parade_image_;
    QUrl scope_parade_url_;
    QImage scope_waveform_image_;
    QUrl scope_waveform_url_;
    QImage scope_vectorscope_image_;
    QUrl scope_vectorscope_url_;
    QImage scope_split_image_;
    QUrl scope_split_url_;
    std::uint64_t scope_revision_ = 0;
};

} // namespace ravo
