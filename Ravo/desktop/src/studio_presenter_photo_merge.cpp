#include "ravo/desktop/studio_presenter.h"
#include "studio_qt.h"

#include <cmath>
#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QUrl>

#include "ravo/services/catalog_service.h"
#include "ravo/services/photo_merge.h"

namespace ravo
{
void StudioPresenter::preparePhotoMerge(const QString &kind)
{
    const auto ids = selected_asset_ids();
    if (busy_ || catalog_operation_active_ || importPageOpen() || catalog_path_.isEmpty() ||
        ids.size() < 2 || ids.size() > 16 ||
        (kind != QLatin1String("hdr") && kind != QLatin1String("panorama")))
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "Select 2 to 16 photos in the library to merge."));
        return;
    }
    photo_merge_assets_ = ids;
    photo_merge_catalog_ = catalog_path_;
    photo_merge_revision_ = observed_catalog_revision_;
    photo_merge_kind_ = kind;
    ++photo_merge_token_;
    QStringList names;
    for (const auto &id : ids)
    {
        const auto record = assets_.assetById(qstring_from_utf8(id));
        names.push_back(record ?
                            QFileInfo(QUrl(qstring_from_utf8(record->normalized_uri)).toLocalFile())
                                .fileName() :
                            qstring_from_utf8(id));
    }
    emit photoMergeDialogRequested(
        QVariantMap{{QStringLiteral("token"), QString::number(photo_merge_token_)},
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("count"), static_cast<int>(ids.size())},
                    {QStringLiteral("sources"), names}});
}

void StudioPresenter::applyPhotoMerge(const QVariantMap &options)
{
    if (busy_ || catalog_operation_active_ || importPageOpen())
        return;
    if (options.value(QStringLiteral("token")).toString() != QString::number(photo_merge_token_) ||
        photo_merge_assets_.empty() || photo_merge_catalog_ != catalog_path_ ||
        photo_merge_assets_ != selected_asset_ids() ||
        photo_merge_revision_ != observed_catalog_revision_)
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Photo selection or library changed; reopen the merge dialog."));
        return;
    }
    PhotoMergeRequest request;
    request.asset_ids = photo_merge_assets_;
    request.options.kind = photo_merge_kind_ == QLatin1String("hdr") ? PhotoMergeKind::kHdr :
                                                                       PhotoMergeKind::kPanorama;
    request.options.auto_align = options.value(QStringLiteral("autoAlign"), true).toBool();
    request.options.auto_crop = options.value(QStringLiteral("autoCrop"), true).toBool();
    bool okay = false;
    const double edge = options.value(QStringLiteral("maxEdge"), 0).toDouble(&okay);
    if (!okay || !std::isfinite(edge) || edge < 0 || edge > 16000 || std::floor(edge) != edge)
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "Maximum edge must be 0 to 16000 pixels."));
        return;
    }
    request.max_edge = static_cast<std::uint32_t>(edge);
    const double deghost = options.value(QStringLiteral("deghost"), .2).toDouble(&okay);
    if (!okay || !std::isfinite(deghost) || deghost < 0 || deghost > 1)
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Deghost threshold must be 0 to 1."));
        return;
    }
    request.options.deghost_threshold = deghost;
    const auto stops = options.value(QStringLiteral("exposureStops")).toString().trimmed();
    if (!stops.isEmpty())
    {
        for (const auto &text : stops.split(QLatin1Char(',')))
        {
            const double value = text.trimmed().toDouble(&okay);
            if (!okay || !std::isfinite(value))
            {
                setError(QCoreApplication::translate(
                    "StudioPresenter", "Exposure stops must be comma-separated numbers."));
                return;
            }
            request.options.exposure_ev.push_back(value);
        }
    }
    request.expected_catalog_revision = photo_merge_revision_;
    catalog_operation_ = CancellationSource{};
    request.cancellation = catalog_operation_.token();
    const auto catalog = catalog_path_;
    const auto selection = request.asset_ids;
    ++photo_merge_token_;
    photo_merge_assets_.clear();
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Merging photos…"), 0, 0,
                        true);
    const bool queued = executor_.post(
        [this, request = std::move(request), catalog, selection]() mutable
        {
            Result<PhotoMergeResult> result =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_)
                result = service_->merge().merge_selected_photos(request);
            std::optional<std::int64_t> revision;
            if (service_)
            {
                auto snap = service_->library().snapshot();
                if (snap)
                    revision = snap.value().revision;
            }
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result), catalog, selection, revision]() mutable
                {
                    if (shutdown_.token().is_cancellation_requested())
                        return;
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (catalog != catalog_path_)
                        return;
                    if (revision)
                        observed_catalog_revision_ = *revision;
                    if (!result)
                    {
                        setError(qstring_from_utf8(result.error().message));
                        if (result.error().context.contains("catalog_committed"))
                            reloadVisibleAssets();
                        return;
                    }
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Created merged TIFF: %1")
                            .arg(qstring_from_utf8(result.value().output_path)));
                    if (selection == selected_asset_ids())
                    {
                        // A derived file is outside Last Import, input folders
                        // and source collections. Show the completed result in
                        // the full library with newest imports first.
                        clearLastImportQuery();
                        library_.replaceQuery(LibraryQuery{});
                        cull_suggestion_filter_ = QStringLiteral("none");
                        cull_suggestion_asset_ids_.clear();
                        emit filterChanged();
                        emit folderChanged();
                        selected_asset_id_ = qstring_from_utf8(result.value().asset.id);
                        selected_ids_ = {result.value().asset.id};
                    }
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        setBusy(false);
        setCatalogOperation({}, 0, 0, false);
        setError(
            QCoreApplication::translate("StudioPresenter", "Photo merge executor is unavailable."));
    }
}
} // namespace ravo
