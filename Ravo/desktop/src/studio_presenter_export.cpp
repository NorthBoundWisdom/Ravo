#include "ravo/desktop/studio_presenter.h"

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

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_qt.h"

namespace ravo
{

QVariantList StudioPresenter::exportFormatChoices() const
{
    return studio_export_format_choices();
}

QVariantList StudioPresenter::jpegSubsamplingChoices() const
{
    return studio_jpeg_subsampling_choices();
}

QVariantList StudioPresenter::pngBitDepthChoices() const
{
    return studio_png_bit_depth_choices();
}

QVariantList StudioPresenter::tiffSampleTypeChoices() const
{
    return studio_tiff_sample_type_choices();
}

QVariantList StudioPresenter::tiffCompressionChoices() const
{
    return studio_tiff_compression_choices();
}

QVariantList StudioPresenter::exportMetadataModeChoices() const
{
    return studio_export_metadata_mode_choices();
}

QVariantMap StudioPresenter::exportDefaultOptions() const
{
    return studio_export_default_options();
}

QVariantMap StudioPresenter::exportOptionBounds() const
{
    return studio_export_option_bounds();
}

void StudioPresenter::exportSelectedToPath(const QString &path, const QString &format,
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
        setError(QCoreApplication::translate("StudioExport", request.error().message.c_str()));
        return;
    }
    ExportRequest snapshot = std::move(request).value();
    snapshot.cancellation = shutdown_.token();
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Exporting…"));
    executor_.post(
        [this, snapshot]()
        {
            Result<ExportResult> exported = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                exported = service_->export_asset(snapshot);
            }
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported)]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Export failed."));
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter", "Exported %1 (%2×%3)")
                                  .arg(QFileInfo(qstring_from_utf8(exported.value().output_path))
                                           .fileName())
                                  .arg(exported.value().width)
                                  .arg(exported.value().height));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::exportSelectedToDirectory(const QString &directory,
                                                const QString &filename_template,
                                                const QString &format, const QVariantMap &options)
{
    const auto asset_ids = selected_asset_ids();
    if (busy_ || catalog_path_.isEmpty() || asset_ids.empty())
        return;
    auto export_options = make_studio_export_options(format, options);
    if (!export_options)
    {
        setError(
            QCoreApplication::translate("StudioExport", export_options.error().message.c_str()));
        return;
    }
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = utf8_from_qstring(directory);
    request.filename_template = utf8_from_qstring(filename_template);
    request.options = std::move(export_options).value();
    request.cancellation = shutdown_.token();
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Exporting selected photos…"));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<std::vector<ExportResult>> exported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                exported = service_->export_assets(request);
            const QString destination = qstring_from_utf8(request.output_directory);
            const auto total = request.asset_ids.size();
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported), destination, total]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        const auto completed = exported.error().context.find("completed_count");
                        if (completed != exported.error().context.end())
                        {
                            setStatus(QCoreApplication::translate(
                                          "StudioPresenter",
                                          "Export stopped after %1 of %2 selected photos.")
                                          .arg(qstring_from_utf8(completed->second))
                                          .arg(total));
                        }
                        else
                        {
                            setStatus(QCoreApplication::translate("StudioPresenter",
                                                                  "Batch export failed."));
                        }
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Exported %1 selected photos to %2")
                                  .arg(exported.value().size())
                                  .arg(destination));
                },
                Qt::QueuedConnection);
        });
}


} // namespace ravo
