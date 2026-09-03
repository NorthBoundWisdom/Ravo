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

namespace
{

QVariantList facet_values(const std::vector<LibraryFacetEntry> &entries)
{
    QVariantList values;
    values.reserve(static_cast<qsizetype>(entries.size()));
    for (const auto &entry : entries)
    {
        QVariantMap value{{QStringLiteral("key"), qstring_from_utf8(entry.key)},
                          {QStringLiteral("label"), qstring_from_utf8(entry.label)},
                          {QStringLiteral("count"), QVariant::fromValue<qulonglong>(entry.count)}};
        if (entry.camera_make)
            value.insert(QStringLiteral("cameraMake"), qstring_from_utf8(*entry.camera_make));
        if (entry.camera_model)
            value.insert(QStringLiteral("cameraModel"), qstring_from_utf8(*entry.camera_model));
        if (entry.focal_length_mm)
            value.insert(QStringLiteral("focalLengthMm"), *entry.focal_length_mm);
        if (entry.captured_local_date)
            value.insert(QStringLiteral("captureDate"),
                         qstring_from_utf8(*entry.captured_local_date));
        values.push_back(value);
    }
    return values;
}

} // namespace

QVariantList StudioPresenter::cameraFacets() const
{
    return facet_values(capture_facets_.cameras);
}

QVariantList StudioPresenter::lensFacets() const
{
    return facet_values(capture_facets_.lenses);
}

QVariantList StudioPresenter::captureDateFacets() const
{
    return facet_values(capture_facets_.capture_dates);
}

QVariantList StudioPresenter::countryFacets() const
{
    return facet_values(location_facets_.countries);
}

QVariantList StudioPresenter::provinceStateFacets() const
{
    return facet_values(location_facets_.province_states);
}

QVariantList StudioPresenter::cityFacets() const
{
    return facet_values(location_facets_.cities);
}

QVariantList StudioPresenter::sublocationFacets() const
{
    return facet_values(location_facets_.sublocations);
}

bool StudioPresenter::facetCountsScoped() const noexcept
{
    return capture_facets_.scoped && location_facets_.scoped;
}

void StudioPresenter::applyFacets(LibraryCaptureFacets capture, LibraryLocationFacets location)
{
    if (capture_facets_ == capture && location_facets_ == location)
        return;
    capture_facets_ = std::move(capture);
    location_facets_ = std::move(location);
    emit facetsChanged();
}

QString StudioPresenter::ratingFilterMode() const
{
    return rating_mode_name(query_.rating_mode);
}

int StudioPresenter::ratingFilterValue() const noexcept
{
    return query_.rating_value;
}

QStringList StudioPresenter::colorFilters() const
{
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(query_.color_labels.size()));
    for (const auto label : query_.color_labels)
    {
        labels.push_back(qstring_from_utf8(color_label_name(label)));
    }
    return labels;
}

QString StudioPresenter::rejectFilter() const
{
    return reject_filter_name(query_.reject_filter);
}

QString StudioPresenter::filterText() const
{
    return qstring_from_utf8(query_.text);
}

QString StudioPresenter::mediaFilter() const
{
    if (query_.media_types.empty())
        return QStringLiteral("any");
    const std::string_view type = query_.media_types.front();
    return type == kMediaTypeRaw  ? QStringLiteral("raw") :
           type == kMediaTypeJpeg ? QStringLiteral("jpeg") :
           type == kMediaTypePng  ? QStringLiteral("png") :
           type == kMediaTypeTiff ? QStringLiteral("tiff") :
                                    qstring_from_utf8(type);
}

QString StudioPresenter::editFilter() const
{
    switch (query_.edit_filter)
    {
    case EditFilter::kEdited:
        return QStringLiteral("edited");
    case EditFilter::kUnedited:
        return QStringLiteral("unedited");
    case EditFilter::kAny:
        return QStringLiteral("any");
    }
    return QStringLiteral("any");
}

QString StudioPresenter::cameraFilter() const
{
    if (!query_.camera_make_equals && !query_.camera_model_equals)
        return {};
    QString label;
    if (query_.camera_make_equals && !query_.camera_make_equals->empty())
        label = qstring_from_utf8(*query_.camera_make_equals);
    if (query_.camera_model_equals && !query_.camera_model_equals->empty())
    {
        if (!label.isEmpty())
            label.append(QLatin1Char(' '));
        label.append(qstring_from_utf8(*query_.camera_model_equals));
    }
    return label;
}

QString StudioPresenter::cameraMakeFilter() const
{
    if (!query_.camera_make_equals)
        return {};
    return qstring_from_utf8(*query_.camera_make_equals);
}

QString StudioPresenter::cameraModelFilter() const
{
    if (!query_.camera_model_equals)
        return {};
    return qstring_from_utf8(*query_.camera_model_equals);
}

QString StudioPresenter::lensFilter() const
{
    if (!query_.focal_length_mm_equals)
        return {};
    return QString::number(*query_.focal_length_mm_equals, 'g', 15);
}

QString StudioPresenter::captureDateFilter() const
{
    if (!query_.captured_local_date)
        return {};
    return qstring_from_utf8(*query_.captured_local_date);
}

QString StudioPresenter::countryFilter() const
{
    if (!query_.country_equals)
        return {};
    return qstring_from_utf8(*query_.country_equals);
}

QString StudioPresenter::provinceStateFilter() const
{
    if (!query_.province_state_equals)
        return {};
    return qstring_from_utf8(*query_.province_state_equals);
}

QString StudioPresenter::cityFilter() const
{
    if (!query_.city_equals)
        return {};
    return qstring_from_utf8(*query_.city_equals);
}

QString StudioPresenter::sublocationFilter() const
{
    if (!query_.sublocation_equals)
        return {};
    return qstring_from_utf8(*query_.sublocation_equals);
}

QString StudioPresenter::locationFilter() const
{
    QStringList parts;
    if (query_.country_equals && !query_.country_equals->empty())
        parts.push_back(qstring_from_utf8(*query_.country_equals));
    if (query_.province_state_equals && !query_.province_state_equals->empty())
        parts.push_back(qstring_from_utf8(*query_.province_state_equals));
    if (query_.city_equals && !query_.city_equals->empty())
        parts.push_back(qstring_from_utf8(*query_.city_equals));
    if (query_.sublocation_equals && !query_.sublocation_equals->empty())
        parts.push_back(qstring_from_utf8(*query_.sublocation_equals));
    return parts.join(QLatin1Char('/'));
}

QString StudioPresenter::sortField() const
{
    return sort_field_name(query_.sort_field);
}

QString StudioPresenter::sortDirection() const
{
    return query_.sort_direction == SortDirection::kAscending ? QStringLiteral("asc") :
                                                                QStringLiteral("desc");
}

int StudioPresenter::visibleCount() const
{
    return libraryTotal();
}

bool StudioPresenter::filtersActive() const noexcept
{
    return query_.rating_mode != RatingFilterMode::kAny || !query_.color_labels.empty() ||
           query_.reject_filter != RejectFilter::kInclude || !query_.tag.empty() ||
           !query_.text.empty() || !query_.media_types.empty() ||
           query_.edit_filter != EditFilter::kAny || !query_.camera.empty() ||
           query_.camera_make_equals || query_.camera_model_equals ||
           query_.focal_length_mm_equals || query_.captured_local_date || query_.country_equals ||
           query_.province_state_equals || query_.city_equals || query_.sublocation_equals ||
           query_.iso.minimum || query_.iso.maximum || query_.aperture.minimum ||
           query_.aperture.maximum || query_.focal_length_mm.minimum ||
           query_.focal_length_mm.maximum || query_.shutter_s.minimum || query_.shutter_s.maximum ||
           query_.aspect_ratio.minimum || query_.aspect_ratio.maximum ||
           (!last_import_selected_ &&
            (query_.imported_after_unix_ms || query_.imported_before_unix_ms)) ||
           query_.captured_after_unix_s || query_.captured_before_unix_s;
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

QString StudioPresenter::tagFilter() const
{
    return qstring_from_utf8(query_.tag);
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
    return qstring_from_utf8(query_.collection_id);
}

QString StudioPresenter::selectedFolderUri() const
{
    return qstring_from_utf8(query_.folder_uri);
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
    if (!asset)
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select a photo first."));
        return;
    }
    const auto path = local_file_path_from_asset_uri(qstring_from_utf8(asset->normalized_uri));
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
