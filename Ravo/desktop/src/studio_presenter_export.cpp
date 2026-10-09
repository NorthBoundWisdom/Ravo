#include "ravo/desktop/studio_export_presenter.h"

#include "ravo/desktop/export_option_conversion.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_qt.h"

namespace ravo
{

StudioExportPresenter::StudioExportPresenter(
    const QString &catalog, const QString &selection, const bool &busy,
    const std::uint64_t &listing_generation, SerialExecutor &executor, CancellationToken shutdown,
    std::function<ExportService *()> service,
    std::function<std::vector<std::string>()> selected_assets,
    std::function<bool()> selection_has_video, QObject *parent)
    : QObject(parent)
    , catalog_path_(catalog)
    , selected_asset_id_(selection)
    , busy_(busy)
    , library_query_generation_(listing_generation)
    , executor_(executor)
    , shutdown_(std::move(shutdown))
    , service_(std::move(service))
    , selected_assets_(std::move(selected_assets))
    , selection_has_video_(std::move(selection_has_video))
{
}

QVariantList StudioExportPresenter::exportFormatChoices() const
{
    const auto choices = studio_export_format_choices();
    if (!selection_has_video_())
        return choices;
    for (const auto &item : choices)
        if (item.toMap().value(QStringLiteral("id")).toString() == QLatin1String("original"))
            return {item};
    qFatal("Export format owner omitted original-copy support");
}

QVariantList StudioExportPresenter::jpegSubsamplingChoices() const
{
    return studio_jpeg_subsampling_choices();
}

QVariantList StudioExportPresenter::pngBitDepthChoices() const
{
    return studio_png_bit_depth_choices();
}

QVariantList StudioExportPresenter::tiffSampleTypeChoices() const
{
    return studio_tiff_sample_type_choices();
}

QVariantList StudioExportPresenter::tiffCompressionChoices() const
{
    return studio_tiff_compression_choices();
}

QVariantList StudioExportPresenter::exportMetadataModeChoices() const
{
    return studio_export_metadata_mode_choices();
}

QVariantList StudioExportPresenter::exportWatermarkAlignmentChoices() const
{
    return studio_export_watermark_alignment_choices();
}

QVariantList StudioExportPresenter::exportOutputProfileChoices() const
{
    return studio_export_output_profile_choices();
}

QVariantList StudioExportPresenter::exportRenderingIntentChoices() const
{
    return studio_export_rendering_intent_choices();
}

QVariantMap StudioExportPresenter::exportDefaultOptions() const
{
    auto defaults = studio_export_default_options();
    if (selection_has_video_())
        defaults.insert(QStringLiteral("format"), QStringLiteral("original"));
    // Suggested form values are not active export constraints. Original size
    // remains the default until the user explicitly chooses a resize mode.
    defaults.insert(QStringLiteral("sizing"),
                    QVariantMap{{QStringLiteral("mode"), QStringLiteral("original")},
                                {QStringLiteral("longEdge"), 2560},
                                {QStringLiteral("width"), 2560},
                                {QStringLiteral("height"), 1440}});
    return defaults;
}

QVariantMap StudioExportPresenter::exportOptionBounds() const
{
    return studio_export_option_bounds();
}

void StudioExportPresenter::checkSelectedCompanionJpegs()
{
    const auto ids = selected_assets_();
    if (busy_ || catalog_path_.isEmpty() || ids.empty())
        return;
    const auto catalog = catalog_path_;
    const auto generation = library_query_generation_;
    emit busyRequested(true);
    emit errorOccurred({});
    const bool queued = executor_.post(
        [this, ids, catalog, generation]
        {
            Result<void> checked = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_())
                checked = service_()->check_companion_jpegs(ids, shutdown_);
            QMetaObject::invokeMethod(
                this,
                [this, ids, catalog, generation, checked = std::move(checked)]
                {
                    emit busyRequested(false);
                    if (catalog != catalog_path_ || generation != library_query_generation_ ||
                        ids != selected_assets_())
                        return;
                    if (checked)
                        emit companionExportReady();
                    else if (checked.error().context.contains("reason") &&
                             checked.error().context.at("reason") == "companion_jpeg_missing")
                        emit companionExportMissing();
                    else
                        emit errorOccurred(qstring_from_utf8(checked.error().message));
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        emit busyRequested(false);
        emit errorOccurred(QStringLiteral("Export worker is unavailable."));
    }
}

void StudioExportPresenter::exportSelectedToPath(const QString &path, const QString &format,
                                                 const QVariantMap &options)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
    {
        return;
    }
    auto request =
        make_studio_export_request(utf8_from_qstring(selected_asset_id_), path, format, options);
    if (!request)
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioExport", request.error().message.c_str()));
        return;
    }
    ExportRequest snapshot = std::move(request).value();
    snapshot.cancellation = shutdown_;
    emit busyRequested(true);
    emit errorOccurred({});
    emit statusOccurred(QCoreApplication::translate("StudioPresenter", "Exporting…"));
    executor_.post(
        [this, snapshot]()
        {
            Result<ExportResult> exported = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_() != nullptr)
            {
                exported = service_()->export_asset(snapshot);
            }
            QMetaObject::invokeMethod(
                this,
                [this, snapshot, exported = std::move(exported)]() mutable
                {
                    emit busyRequested(false);
                    if (!exported)
                    {
                        if (snapshot.format == ExportFormat::kCompanionJpeg &&
                            exported.error().context.contains("reason") &&
                            exported.error().context.at("reason") == "companion_jpeg_missing" &&
                            utf8_from_qstring(selected_asset_id_) == snapshot.asset_id)
                        {
                            emit companionExportMissing();
                            return;
                        }
                        emit errorOccurred(qstring_from_utf8(exported.error().message));
                        emit statusOccurred(
                            QCoreApplication::translate("StudioPresenter", "Export failed."));
                        return;
                    }
                    emit statusOccurred(
                        QCoreApplication::translate("StudioPresenter", "Exported %1 (%2×%3)")
                            .arg(QFileInfo(qstring_from_utf8(exported.value().output_path))
                                     .fileName())
                            .arg(exported.value().width)
                            .arg(exported.value().height));
                },
                Qt::QueuedConnection);
        });
}

void StudioExportPresenter::exportSelectedToDirectory(const QString &directory,
                                                      const QString &filename_template,
                                                      const QString &format,
                                                      const QVariantMap &options)
{
    const auto asset_ids = selected_assets_();
    if (busy_ || catalog_path_.isEmpty() || asset_ids.empty())
        return;
    auto export_options = make_studio_export_options(format, options);
    if (!export_options)
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioExport", export_options.error().message.c_str()));
        return;
    }
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = utf8_from_qstring(directory);
    request.filename_template = utf8_from_qstring(filename_template);
    request.options = std::move(export_options).value();
    request.cancellation = shutdown_;
    emit busyRequested(true);
    emit errorOccurred({});
    emit statusOccurred(
        QCoreApplication::translate("StudioPresenter", "Exporting selected photos…"));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<std::vector<ExportResult>> exported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_() != nullptr)
                exported = service_()->export_assets(request);
            const QString destination = qstring_from_utf8(request.output_directory);
            const auto total = request.asset_ids.size();
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported), destination, total, ids = request.asset_ids,
                 format = request.options.format]() mutable
                {
                    emit busyRequested(false);
                    if (!exported)
                    {
                        if (format == ExportFormat::kCompanionJpeg &&
                            exported.error().context.contains("reason") &&
                            exported.error().context.at("reason") == "companion_jpeg_missing" &&
                            (!exported.error().context.contains("completed_count") ||
                             exported.error().context.at("completed_count") == "0") &&
                            ids == selected_assets_())
                        {
                            emit companionExportMissing();
                            return;
                        }
                        emit errorOccurred(qstring_from_utf8(exported.error().message));
                        const auto completed = exported.error().context.find("completed_count");
                        if (completed != exported.error().context.end())
                        {
                            emit statusOccurred(
                                QCoreApplication::translate(
                                    "StudioPresenter",
                                    "Export stopped after %1 of %2 selected photos.")
                                    .arg(qstring_from_utf8(completed->second))
                                    .arg(total));
                        }
                        else
                        {
                            emit statusOccurred(QCoreApplication::translate(
                                "StudioPresenter", "Batch export failed."));
                        }
                        return;
                    }
                    emit statusOccurred(QCoreApplication::translate(
                                            "StudioPresenter", "Exported %1 selected photos to %2")
                                            .arg(exported.value().size())
                                            .arg(destination));
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
