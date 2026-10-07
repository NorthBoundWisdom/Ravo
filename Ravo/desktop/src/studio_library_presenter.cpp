#include "ravo/desktop/studio_library_presenter.h"

#include <utility>
#include <algorithm>
#include <QCoreApplication>

#include "studio_qt.h"

namespace ravo
{
StudioLibraryPresenter::StudioLibraryPresenter(QObject *parent)
    : QObject(parent)
{
}

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
        if (entry.lens_make)
            value.insert(QStringLiteral("lensMake"), qstring_from_utf8(*entry.lens_make));
        if (entry.lens_model)
            value.insert(QStringLiteral("lensModel"), qstring_from_utf8(*entry.lens_model));
        if (entry.captured_local_date)
            value.insert(QStringLiteral("captureDate"),
                         qstring_from_utf8(*entry.captured_local_date));
        values.push_back(value);
    }
    return values;
}

} // namespace

QVariantList StudioLibraryPresenter::cameraFacets() const
{
    return facet_values(capture_facets_.cameras);
}

QVariantList StudioLibraryPresenter::lensFacets() const
{
    return facet_values(capture_facets_.lenses);
}

QVariantList StudioLibraryPresenter::lensNameFacets() const
{
    return facet_values(capture_facets_.lens_names);
}

QVariantList StudioLibraryPresenter::captureDateFacets() const
{
    return facet_values(capture_facets_.capture_dates);
}

QVariantList StudioLibraryPresenter::countryFacets() const
{
    return facet_values(location_facets_.countries);
}

QVariantList StudioLibraryPresenter::provinceStateFacets() const
{
    return facet_values(location_facets_.province_states);
}

QVariantList StudioLibraryPresenter::cityFacets() const
{
    return facet_values(location_facets_.cities);
}

QVariantList StudioLibraryPresenter::sublocationFacets() const
{
    return facet_values(location_facets_.sublocations);
}

bool StudioLibraryPresenter::facetCountsScoped() const noexcept
{
    return capture_facets_.scoped && location_facets_.scoped;
}

void StudioLibraryPresenter::apply(LibraryCaptureFacets capture, LibraryLocationFacets location)
{
    if (capture_facets_ == capture && location_facets_ == location)
        return;
    capture_facets_ = std::move(capture);
    location_facets_ = std::move(location);
    emit facetsChanged();
}

const LibraryQuery &StudioLibraryPresenter::query() const noexcept
{
    return query_;
}

void StudioLibraryPresenter::replaceQuery(LibraryQuery query)
{
    if (query_ == query)
        return;
    query_ = std::move(query);
    emit queryChanged();
}

void StudioLibraryPresenter::setFolderScope(std::string folder_uri)
{
    auto next = query_;
    next.folder_uri = std::move(folder_uri);
    replaceQuery(std::move(next));
}

void StudioLibraryPresenter::setCollectionScope(std::string collection_id)
{
    auto next = query_;
    next.collection_id = std::move(collection_id);
    replaceQuery(std::move(next));
}

void StudioLibraryPresenter::setImportScope(std::optional<std::int64_t> after,
                                            std::optional<std::int64_t> before)
{
    auto next = query_;
    next.imported_after_unix_ms = after;
    next.imported_before_unix_ms = before;
    replaceQuery(std::move(next));
}

void StudioLibraryPresenter::resetFilters(std::optional<std::int64_t> import_after,
                                          std::optional<std::int64_t> import_before)
{
    query_.rating_mode = RatingFilterMode::kAny;
    query_.rating_value = 0;
    query_.color_labels.clear();
    query_.reject_filter = RejectFilter::kInclude;
    query_.tag.clear();
    query_.text.clear();
    query_.media_types.clear();
    query_.edit_filter = EditFilter::kAny;
    query_.camera.clear();
    query_.camera_make_equals.reset();
    query_.camera_model_equals.reset();
    query_.lens_make_equals.reset();
    query_.lens_model_equals.reset();
    query_.focal_length_mm_equals.reset();
    query_.captured_local_date.reset();
    query_.country_equals.reset();
    query_.province_state_equals.reset();
    query_.city_equals.reset();
    query_.sublocation_equals.reset();
    query_.iso = {};
    query_.aperture = {};
    query_.focal_length_mm = {};
    query_.shutter_s = {};
    query_.aspect_ratio = {};
    query_.imported_after_unix_ms.reset();
    query_.imported_before_unix_ms.reset();
    query_.captured_after_unix_s.reset();
    query_.captured_before_unix_s.reset();
    query_.imported_after_unix_ms = import_after;
    query_.imported_before_unix_ms = import_before;
    emit queryChanged();
}

QString StudioLibraryPresenter::ratingFilterMode() const
{
    return rating_mode_name(query_.rating_mode);
}

int StudioLibraryPresenter::ratingFilterValue() const noexcept
{
    return query_.rating_value;
}

QStringList StudioLibraryPresenter::colorFilters() const
{
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(query_.color_labels.size()));
    for (const auto label : query_.color_labels)
    {
        labels.push_back(qstring_from_utf8(color_label_name(label)));
    }
    return labels;
}

QString StudioLibraryPresenter::rejectFilter() const
{
    return reject_filter_name(query_.reject_filter);
}

QString StudioLibraryPresenter::pickFilter() const
{
    switch (query_.pick_filter)
    {
    case PickFilter::kExclude:
        return QStringLiteral("exclude");
    case PickFilter::kOnly:
        return QStringLiteral("only");
    case PickFilter::kInclude:
        return QStringLiteral("include");
    }
    return QStringLiteral("include");
}

QString StudioLibraryPresenter::cullFlagFilter() const
{
    switch (query_.cull_flag_filter)
    {
    case CullFlagFilter::kPicked:
        return QStringLiteral("picked");
    case CullFlagFilter::kRejected:
        return QStringLiteral("rejected");
    case CullFlagFilter::kUnreviewed:
        return QStringLiteral("unreviewed");
    case CullFlagFilter::kAny:
        return QStringLiteral("any");
    }
    return QStringLiteral("any");
}

QString StudioLibraryPresenter::filterText() const
{
    return qstring_from_utf8(query_.text);
}

QString StudioLibraryPresenter::mediaFilter() const
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

QString StudioLibraryPresenter::editFilter() const
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

QString StudioLibraryPresenter::cameraFilter() const
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

QString StudioLibraryPresenter::cameraMakeFilter() const
{
    if (!query_.camera_make_equals)
        return {};
    return qstring_from_utf8(*query_.camera_make_equals);
}

QString StudioLibraryPresenter::cameraModelFilter() const
{
    if (!query_.camera_model_equals)
        return {};
    return qstring_from_utf8(*query_.camera_model_equals);
}

QString StudioLibraryPresenter::lensFilter() const
{
    if (!query_.focal_length_mm_equals)
        return {};
    return QString::number(*query_.focal_length_mm_equals, 'g', 15);
}

QString StudioLibraryPresenter::lensMakeFilter() const
{
    if (!query_.lens_make_equals)
        return {};
    return qstring_from_utf8(*query_.lens_make_equals);
}

QString StudioLibraryPresenter::lensModelFilter() const
{
    if (!query_.lens_model_equals)
        return {};
    return qstring_from_utf8(*query_.lens_model_equals);
}

QString StudioLibraryPresenter::captureDateFilter() const
{
    if (!query_.captured_local_date)
        return {};
    return qstring_from_utf8(*query_.captured_local_date);
}

QString StudioLibraryPresenter::countryFilter() const
{
    if (!query_.country_equals)
        return {};
    return qstring_from_utf8(*query_.country_equals);
}

QString StudioLibraryPresenter::provinceStateFilter() const
{
    if (!query_.province_state_equals)
        return {};
    return qstring_from_utf8(*query_.province_state_equals);
}

QString StudioLibraryPresenter::cityFilter() const
{
    if (!query_.city_equals)
        return {};
    return qstring_from_utf8(*query_.city_equals);
}

QString StudioLibraryPresenter::sublocationFilter() const
{
    if (!query_.sublocation_equals)
        return {};
    return qstring_from_utf8(*query_.sublocation_equals);
}

QString StudioLibraryPresenter::locationFilter() const
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

QString StudioLibraryPresenter::sortField() const
{
    return sort_field_name(query_.sort_field);
}

QString StudioLibraryPresenter::sortDirection() const
{
    return query_.sort_direction == SortDirection::kAscending ? QStringLiteral("asc") :
                                                                QStringLiteral("desc");
}

QString StudioLibraryPresenter::tagFilter() const
{
    return qstring_from_utf8(query_.tag);
}

void StudioLibraryPresenter::setTagFilter(const QString &tag)
{
    auto parsed = tag.trimmed().isEmpty() ? Result<std::string>{std::string{}} :
                                            normalize_tag_name(utf8_from_qstring(tag));
    if (!parsed)
    {
        emit errorOccurred(qstring_from_utf8(parsed.error().message));
        return;
    }
    if (query_.tag == parsed.value())
    {
        return;
    }
    query_.tag = parsed.value();
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setRatingFilter(const QString &mode, const int value)
{
    RatingFilterMode next_mode = RatingFilterMode::kAny;
    if (mode == QStringLiteral("min"))
    {
        next_mode = RatingFilterMode::kMinimum;
    }
    else if (mode == QStringLiteral("exact"))
    {
        next_mode = RatingFilterMode::kExact;
    }
    if (query_.rating_mode == next_mode && query_.rating_value == value)
    {
        return;
    }
    query_.rating_mode = next_mode;
    query_.rating_value = value;
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::toggleColorFilter(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        emit errorOccurred(qstring_from_utf8(parsed.error().message));
        return;
    }
    auto &labels = query_.color_labels;
    const auto found = std::find(labels.begin(), labels.end(), parsed.value());
    if (found == labels.end())
    {
        labels.push_back(parsed.value());
    }
    else
    {
        labels.erase(found);
    }
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setRejectFilter(const QString &mode)
{
    RejectFilter next = RejectFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = RejectFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = RejectFilter::kOnly;
    }
    if (query_.reject_filter == next)
    {
        return;
    }
    query_.reject_filter = next;
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setPickFilter(const QString &mode)
{
    PickFilter next = PickFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = PickFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = PickFilter::kOnly;
    }
    if (query_.pick_filter == next)
    {
        return;
    }
    query_.pick_filter = next;
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setCullFlagFilter(const QString &mode)
{
    CullFlagFilter next = CullFlagFilter::kAny;
    if (mode == QStringLiteral("picked"))
    {
        next = CullFlagFilter::kPicked;
    }
    else if (mode == QStringLiteral("rejected"))
    {
        next = CullFlagFilter::kRejected;
    }
    else if (mode == QStringLiteral("unreviewed"))
    {
        next = CullFlagFilter::kUnreviewed;
    }
    else if (mode != QStringLiteral("any") && !mode.isEmpty())
    {
        emit errorOccurred(QCoreApplication::translate(
            "StudioPresenter", "Cull flag filter must be any, picked, rejected, or unreviewed."));
        return;
    }
    if (query_.cull_flag_filter == next)
    {
        return;
    }
    query_.cull_flag_filter = next;
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setFilterText(const QString &text)
{
    LibraryQuery next = query_;
    next.text = utf8_from_qstring(text.trimmed());
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setMediaFilter(const QString &mode)
{
    LibraryQuery next = query_;
    next.media_types.clear();
    if (mode == QLatin1String("raw"))
        next.media_types.emplace_back(kMediaTypeRaw);
    else if (mode == QLatin1String("jpeg"))
        next.media_types.emplace_back(kMediaTypeJpeg);
    else if (mode == QLatin1String("png"))
        next.media_types.emplace_back(kMediaTypePng);
    else if (mode == QLatin1String("tiff"))
        next.media_types.emplace_back(kMediaTypeTiff);
    else if (mode != QLatin1String("any"))
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Unknown media filter mode."));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setEditFilter(const QString &mode)
{
    EditFilter next = EditFilter::kAny;
    if (mode == QLatin1String("edited"))
        next = EditFilter::kEdited;
    else if (mode == QLatin1String("unedited"))
        next = EditFilter::kUnedited;
    else if (mode != QLatin1String("any"))
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Unknown edit filter mode."));
        return;
    }
    if (query_.edit_filter == next)
        return;
    query_.edit_filter = next;
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setCameraFacetFilter(const QString &make, const QString &model)
{
    LibraryQuery next = query_;
    const auto make_utf8 = utf8_from_qstring(make.trimmed());
    const auto model_utf8 = utf8_from_qstring(model.trimmed());
    if (make_utf8.empty() && model_utf8.empty())
    {
        next.camera_make_equals.reset();
        next.camera_model_equals.reset();
    }
    else
    {
        next.camera_make_equals = make_utf8;
        next.camera_model_equals = model_utf8;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setLensFacetFilter(const QString &focal_mm)
{
    LibraryQuery next = query_;
    const auto trimmed = focal_mm.trimmed();
    if (trimmed.isEmpty())
    {
        next.focal_length_mm_equals.reset();
    }
    else
    {
        bool ok = false;
        const double value = trimmed.toDouble(&ok);
        if (!ok)
        {
            emit errorOccurred(QCoreApplication::translate(
                "StudioPresenter", "Lens facet must be a focal length in millimeters."));
            return;
        }
        next.focal_length_mm_equals = value;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setLensNameFacetFilter(const QString &make, const QString &model)
{
    LibraryQuery next = query_;
    const auto make_utf8 = utf8_from_qstring(make.trimmed());
    const auto model_utf8 = utf8_from_qstring(model.trimmed());
    if (make_utf8.empty() && model_utf8.empty())
    {
        next.lens_make_equals.reset();
        next.lens_model_equals.reset();
    }
    else
    {
        next.lens_make_equals = make_utf8;
        next.lens_model_equals = model_utf8;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setCaptureDateFacetFilter(const QString &local_date)
{
    LibraryQuery next = query_;
    const auto utf8 = utf8_from_qstring(local_date.trimmed());
    if (utf8.empty())
        next.captured_local_date.reset();
    else
        next.captured_local_date = utf8;
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setLocationFacetFilter(const QString &country,
                                                    const QString &province_state,
                                                    const QString &city, const QString &sublocation)
{
    LibraryQuery next = query_;
    const auto assign = [](std::optional<std::string> &field, const QString &text)
    {
        const auto utf8 = utf8_from_qstring(text.trimmed());
        if (utf8.empty())
            field.reset();
        else
            field = utf8;
    };
    assign(next.country_equals, country);
    assign(next.province_state_equals, province_state);
    assign(next.city_equals, city);
    assign(next.sublocation_equals, sublocation);
    auto valid = validate_library_query(next);
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit queryChanged();
    emit reloadRequested();
}

void StudioLibraryPresenter::setSort(const QString &field, const QString &direction)
{
    AssetSortField next_field = AssetSortField::kImportTime;
    if (field == QStringLiteral("name"))
    {
        next_field = AssetSortField::kDisplayName;
    }
    else if (field == QStringLiteral("rating"))
    {
        next_field = AssetSortField::kRating;
    }
    else if (field == QStringLiteral("captured"))
    {
        next_field = AssetSortField::kCaptureTime;
    }
    else if (field == QStringLiteral("size"))
    {
        next_field = AssetSortField::kFileSize;
    }
    const auto next_direction =
        direction == QStringLiteral("asc") ? SortDirection::kAscending : SortDirection::kDescending;
    if (query_.sort_field == next_field && query_.sort_direction == next_direction)
    {
        return;
    }
    query_.sort_field = next_field;
    query_.sort_direction = next_direction;
    emit queryChanged();
    emit reloadRequested();
}

} // namespace ravo
