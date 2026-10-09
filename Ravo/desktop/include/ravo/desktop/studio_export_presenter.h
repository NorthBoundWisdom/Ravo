#pragma once
#include <cstdint>
#include <functional>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/services/exports_service.h"
namespace ravo
{
class StudioPresenter;
// GUI-thread export intent/result owner. Form values remain a transient view
// draft; requests snapshot selection and validated options before dispatch.
class StudioExportPresenter final : public QObject
{
    Q_OBJECT

public:
    void checkSelectedCompanionJpegs();
    Q_INVOKABLE QVariantMap exportDefaultOptions() const;
    Q_INVOKABLE QVariantList exportFormatChoices() const;
    Q_INVOKABLE QVariantList exportMetadataModeChoices() const;
    Q_INVOKABLE QVariantMap exportOptionBounds() const;
    Q_INVOKABLE QVariantList exportOutputProfileChoices() const;
    Q_INVOKABLE QVariantList exportRenderingIntentChoices() const;
    Q_INVOKABLE void exportSelectedToDirectory(const QString &directory,
                                               const QString &filename_template,
                                               const QString &format, const QVariantMap &options);
    Q_INVOKABLE void exportSelectedToPath(const QString &path, const QString &format,
                                          const QVariantMap &options);
    Q_INVOKABLE QVariantList exportWatermarkAlignmentChoices() const;
    Q_INVOKABLE QVariantList jpegSubsamplingChoices() const;
    Q_INVOKABLE QVariantList pngBitDepthChoices() const;
    Q_INVOKABLE QVariantList tiffCompressionChoices() const;
    Q_INVOKABLE QVariantList tiffSampleTypeChoices() const;
signals:
    void companionExportReady();
    void companionExportMissing();
    void busyRequested(bool busy);
    void errorOccurred(QString error);
    void statusOccurred(QString status);

private:
    friend class StudioPresenter;
    StudioExportPresenter(const QString &catalog, const QString &selection, const bool &busy,
                          const std::uint64_t &listing_generation, SerialExecutor &executor,
                          CancellationToken shutdown, std::function<ExportService *()> service,
                          std::function<std::vector<std::string>()> selected_assets,
                          std::function<bool()> selection_has_video, QObject *parent);
    const QString &catalog_path_;
    const QString &selected_asset_id_;
    const bool &busy_;
    const std::uint64_t &library_query_generation_;
    SerialExecutor &executor_;
    CancellationToken shutdown_;
    std::function<ExportService *()> service_;
    std::function<std::vector<std::string>()> selected_assets_;
    std::function<bool()> selection_has_video_;
};
} // namespace ravo
