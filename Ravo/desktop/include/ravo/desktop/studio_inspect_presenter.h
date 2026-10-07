#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/foundation/executor.h"

namespace ravo
{
// GUI-thread presentation/identity owner. Worker inputs are owned QImage
// snapshots. Observed selection is read-only context, never a selection writer.
// Image-provider getters return synchronized value snapshots from any thread.
class StudioInspectPresenter final : public QObject
{
    Q_OBJECT
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
    explicit StudioInspectPresenter(QObject *parent = nullptr);
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

signals:
    void frameChanged();
    void scopesChanged();
    void identityChanged();
    void errorOccurred(QString error);

private:
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
