#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <climits>

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>

#include "ravo/domain/types.h"
#include "studio_file_manager.h"
#include "studio_qt.h"

namespace ravo
{

void StudioPresenter::selectLibraryRow(const int row, const QString &mode, const bool open_loupe)
{
    if (catalog_path_.isEmpty() || busy_ || import_workspace_->importPageOpen())
        return;
    if (row < 0 || row >= assets_.rowCount() ||
        (mode != QLatin1String("single") && mode != QLatin1String("toggle") &&
         mode != QLatin1String("range")))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select a photo first."));
        return;
    }
    pending_library_selection_ = PendingLibrarySelection{
        row, library_query_generation_, selected_asset_id_, selected_ids_, mode, open_loupe};
    if (assets_.rowLoaded(row))
        completePendingLibrarySelection();
    else
        ensureLibraryRow(row);
}

void StudioPresenter::completePendingLibrarySelection()
{
    if (!pending_library_selection_)
        return;
    const auto &pending = *pending_library_selection_;
    if (pending.generation != library_query_generation_ || pending.primary != selected_asset_id_ ||
        pending.selection != selected_ids_)
    {
        pending_library_selection_.reset();
        return;
    }
    if (!assets_.rowLoaded(pending.row))
        return;
    const auto id = assets_.assetIdAt(pending.row);
    const auto mode = pending.mode;
    const bool open_loupe = pending.open_loupe;
    pending_library_selection_.reset();
    if (id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select a photo first."));
        return;
    }
    setError({});
    if (mode == QLatin1String("range"))
        selectAssetRange(id);
    else if (mode == QLatin1String("toggle"))
        toggleAssetSelected(id);
    else
        selectAsset(id);
    if (open_loupe)
        setBrowseMode(QStringLiteral("loupe"));
}

QString StudioPresenter::cullSuggestionFilter() const
{
    return cull_suggestion_filter_;
}

int StudioPresenter::visibleCount() const
{
    return libraryTotal();
}

bool StudioPresenter::filtersActive() const noexcept
{
    const auto &query = library_.query();
    return query.rating_mode != RatingFilterMode::kAny || !query.color_labels.empty() ||
           query.reject_filter != RejectFilter::kInclude ||
           query.pick_filter != PickFilter::kInclude ||
           query.cull_flag_filter != CullFlagFilter::kAny ||
           cull_suggestion_filter_ != QStringLiteral("none") || !query.tag.empty() ||
           !query.text.empty() || !query.media_types.empty() ||
           query.edit_filter != EditFilter::kAny || !query.camera.empty() ||
           query.camera_make_equals || query.camera_model_equals || query.lens_make_equals ||
           query.lens_model_equals || query.focal_length_mm_equals || query.captured_local_date ||
           query.country_equals || query.province_state_equals || query.city_equals ||
           query.sublocation_equals || query.iso.minimum || query.iso.maximum ||
           query.aperture.minimum || query.aperture.maximum || query.focal_length_mm.minimum ||
           query.focal_length_mm.maximum || query.shutter_s.minimum || query.shutter_s.maximum ||
           query.aspect_ratio.minimum || query.aspect_ratio.maximum ||
           (!last_import_selected_ &&
            (query.imported_after_unix_ms || query.imported_before_unix_ms)) ||
           query.captured_after_unix_s || query.captured_before_unix_s;
}

bool StudioPresenter::selectedHasEdits() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->has_edits;
}

QString StudioPresenter::selectedTags() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList tags;
    for (const auto &tag : asset->tags)
    {
        tags.push_back(qstring_from_utf8(tag));
    }
    return tags.join(QStringLiteral(", "));
}

QString StudioPresenter::selectedTitle() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.title ? qstring_from_utf8(*asset->metadata.title) : QString{};
}

QString StudioPresenter::selectedDescription() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.description ? qstring_from_utf8(*asset->metadata.description) :
                                                  QString{};
}

QString StudioPresenter::selectedCreator() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.creator ? qstring_from_utf8(*asset->metadata.creator) :
                                              QString{};
}

QString StudioPresenter::selectedCopyright() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.copyright ? qstring_from_utf8(*asset->metadata.copyright) :
                                                QString{};
}

QString StudioPresenter::selectedCountry() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.country ? qstring_from_utf8(*asset->metadata.country) :
                                              QString{};
}

QString StudioPresenter::selectedProvinceState() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.province_state ?
               qstring_from_utf8(*asset->metadata.province_state) :
               QString{};
}

QString StudioPresenter::selectedCity() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.city ? qstring_from_utf8(*asset->metadata.city) : QString{};
}

QString StudioPresenter::selectedSublocation() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.sublocation ? qstring_from_utf8(*asset->metadata.sublocation) :
                                                  QString{};
}

QString StudioPresenter::selectedHeadline() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.headline ? qstring_from_utf8(*asset->metadata.headline) :
                                               QString{};
}

QString StudioPresenter::selectedCredit() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.credit ? qstring_from_utf8(*asset->metadata.credit) : QString{};
}

QString StudioPresenter::selectedSource() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.source ? qstring_from_utf8(*asset->metadata.source) : QString{};
}

QString StudioPresenter::selectedInstructions() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.instructions ?
               qstring_from_utf8(*asset->metadata.instructions) :
               QString{};
}

QString StudioPresenter::selectedUsageTerms() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.usage_terms ? qstring_from_utf8(*asset->metadata.usage_terms) :
                                                  QString{};
}

QString StudioPresenter::selectedJobId() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.job_id ? qstring_from_utf8(*asset->metadata.job_id) : QString{};
}

QString StudioPresenter::selectedCaptureSummary() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList parts;
    if (asset->capture.camera_make)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_make));
    }
    if (asset->capture.camera_model)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_model));
    }
    if (asset->capture.iso)
    {
        parts.push_back(QStringLiteral("ISO %1").arg(*asset->capture.iso, 0, 'f', 0));
    }
    if (asset->capture.aperture)
    {
        parts.push_back(QStringLiteral("f/%1").arg(*asset->capture.aperture, 0, 'f', 1));
    }
    if (asset->capture.focal_length_mm)
    {
        parts.push_back(QStringLiteral("%1 mm").arg(*asset->capture.focal_length_mm, 0, 'f', 0));
    }
    return parts.join(QStringLiteral(" · "));
}

QUrl StudioPresenter::selectedThumbnailUrl() const
{
    const int row = assets_.indexOf(selected_asset_id_);
    if (row < 0)
    {
        return {};
    }
    return assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
}

QString StudioPresenter::selectedLibrarySetId() const
{
    return qstring_from_utf8(library_.query().collection_id);
}

QString StudioPresenter::selectedFolderUri() const
{
    return qstring_from_utf8(library_.query().folder_uri);
}

bool StudioPresenter::lastImportAvailable() const noexcept
{
    return last_import_count_ > 0U && last_import_after_unix_ms_ && last_import_before_unix_ms_;
}

bool StudioPresenter::lastImportSelected() const noexcept
{
    return last_import_selected_;
}

int StudioPresenter::lastImportCount() const noexcept
{
    return static_cast<int>(
        std::min<std::size_t>(last_import_count_, static_cast<std::size_t>(INT_MAX)));
}

QString StudioPresenter::selectedDisplayName() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset_display_name(*asset)) : QString{};
}

QString StudioPresenter::selectedFolderPath() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    const QUrl file = QUrl(qstring_from_utf8(asset->normalized_uri));
    return QFileInfo(file.toLocalFile()).absolutePath();
}

QString StudioPresenter::selectedMediaType() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->media_type) : QString{};
}

QString StudioPresenter::selectedDimensions() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->width || !asset->height)
    {
        return {};
    }
    return QStringLiteral("%1 × %2").arg(*asset->width).arg(*asset->height);
}

QString StudioPresenter::selectedFileSize() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || asset->size_bytes == 0)
    {
        return {};
    }
    const auto bytes = static_cast<double>(asset->size_bytes);
    if (bytes < 1024.0)
    {
        return QStringLiteral("%1 B").arg(asset->size_bytes);
    }
    if (bytes < 1024.0 * 1024.0)
    {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString StudioPresenter::selectedUri() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->normalized_uri) : QString{};
}

void StudioPresenter::revealSelectedPhotoInFileManager()
{
    const auto asset = assets_.assetById(selected_asset_id_);
    const QString import_path = import_workspace_->importContextPath();
    if (import_workspace_->importPageOpen() ? import_path.isEmpty() : !asset)
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select a photo first."));
        return;
    }
    const auto path = local_file_path_from_asset_uri(
        import_workspace_->importPageOpen() ? QUrl::fromLocalFile(import_path).toString() :
                                              qstring_from_utf8(asset->normalized_uri));
    if (!path)
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "The selected photo has no local file path."));
        return;
    }
    const auto launch = file_manager_reveal_launch(path.value());
    if (!launch)
    {
        setError(QCoreApplication::translate(
            "StudioPresenter",
            "The original file is missing and cannot be shown in the file manager."));
        return;
    }
    if (!start_file_manager_reveal(launch.value()))
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "The file manager could not be opened."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Showing the original file."));
}

} // namespace ravo
