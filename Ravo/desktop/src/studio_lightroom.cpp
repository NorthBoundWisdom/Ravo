#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_import_workspace.h"
#include "ravo/services/foreign_catalog.h"
#include "studio_qt.h"

#include <QCoreApplication>
#include <QMetaObject>

namespace ravo
{
void StudioPresenter::importLightroomCatalog(const QString &path)
{
    if (busy_ || catalog_operation_active_ || import_workspace_->importWorkActive() ||
        catalog_path_.isEmpty())
        return;
    if (path.trimmed().isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Choose a Lightroom catalog."));
        return;
    }
    catalog_operation_ = CancellationSource{};
    ForeignCatalogConversionRequest request;
    request.source_path = utf8_from_qstring(path);
    request.source_kind = ForeignCatalogSourceKind::kLightroomClassic;
    request.cancellation = catalog_operation_.token();
    setBusy(true);
    setError({});
    setCatalogOperation(
        QCoreApplication::translate("StudioPresenter", "Importing Lightroom catalog…"), 0, 0, true);
    const auto catalog = catalog_path_;
    if (!executor_.post(
            [this, request, catalog]
            {
                Result<ForeignCatalogConversionReport> result =
                    make_error(ErrorCode::kIo, "Catalog session is closed");
                if (service_)
                    result = service_->conversion().convert_foreign_catalog(request);
                QMetaObject::invokeMethod(
                    this,
                    [this, catalog, result = std::move(result)]() mutable
                    {
                        setBusy(false);
                        setCatalogOperation({}, 0, 0, false);
                        if (catalog_path_ != catalog)
                            return;
                        if (!result)
                        {
                            setError(qstring_from_utf8(result.error().message));
                            reloadVisibleAssets();
                            return;
                        }
                        const auto &report = result.value();
                        setStatus(
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Lightroom: %1 imported, %2 skipped, %3 unsupported, %4 failed. %5 fields were not converted.")
                                .arg(report.imported)
                                .arg(report.skipped)
                                .arg(report.unsupported)
                                .arg(report.failed)
                                .arg(report.unsupported_fields));
                        bool has_omissions =
                            report.unsupported_fields != 0 || !report.archived_only_tables.empty();
                        for (const auto &collection : report.collections)
                            has_omissions |= !collection.reasons.empty();
                        if (has_omissions)
                            setError(QCoreApplication::translate(
                                "StudioPresenter",
                                "Lightroom adjustments, history, collections and custom metadata may not be converted. Original photos and the Lightroom catalog are unchanged."));
                        if (report.cancelled)
                            setError(QCoreApplication::translate(
                                "StudioPresenter",
                                "Lightroom import cancelled. Already imported photos remain in this library."));
                        if (!report.issues.empty())
                            setError(qstring_from_utf8(report.issues.front().message));
                        else
                            for (const auto &audit : report.source_audits)
                                if (audit.error && audit.error->code != ErrorCode::kCancelled)
                                {
                                    setError(qstring_from_utf8(audit.error->message));
                                    break;
                                }
                        reloadVisibleAssets();
                    },
                    Qt::QueuedConnection);
            }))
    {
        setBusy(false);
        setCatalogOperation({}, 0, 0, false);
        setError(
            QCoreApplication::translate("StudioPresenter", "Catalog executor is unavailable."));
    }
}
} // namespace ravo
