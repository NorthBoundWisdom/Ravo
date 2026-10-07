#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>

#include "ravo/domain/types.h"

namespace ravo
{

// GUI-thread query owner and projection of accepted immutable facet snapshots.
// Catalog/query generation checks belong to the listing owner before apply().
class StudioLibraryPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString ratingFilterMode READ ratingFilterMode NOTIFY queryChanged)
    Q_PROPERTY(int ratingFilterValue READ ratingFilterValue NOTIFY queryChanged)
    Q_PROPERTY(QStringList colorFilters READ colorFilters NOTIFY queryChanged)
    Q_PROPERTY(QString rejectFilter READ rejectFilter NOTIFY queryChanged)
    Q_PROPERTY(QString pickFilter READ pickFilter NOTIFY queryChanged)
    Q_PROPERTY(QString cullFlagFilter READ cullFlagFilter NOTIFY queryChanged)
    Q_PROPERTY(QString filterText READ filterText NOTIFY queryChanged)
    Q_PROPERTY(QString mediaFilter READ mediaFilter NOTIFY queryChanged)
    Q_PROPERTY(QString editFilter READ editFilter NOTIFY queryChanged)
    Q_PROPERTY(QString cameraFilter READ cameraFilter NOTIFY queryChanged)
    Q_PROPERTY(QString cameraMakeFilter READ cameraMakeFilter NOTIFY queryChanged)
    Q_PROPERTY(QString cameraModelFilter READ cameraModelFilter NOTIFY queryChanged)
    Q_PROPERTY(QString lensFilter READ lensFilter NOTIFY queryChanged)
    Q_PROPERTY(QString lensMakeFilter READ lensMakeFilter NOTIFY queryChanged)
    Q_PROPERTY(QString lensModelFilter READ lensModelFilter NOTIFY queryChanged)
    Q_PROPERTY(QString captureDateFilter READ captureDateFilter NOTIFY queryChanged)
    Q_PROPERTY(QString countryFilter READ countryFilter NOTIFY queryChanged)
    Q_PROPERTY(QString provinceStateFilter READ provinceStateFilter NOTIFY queryChanged)
    Q_PROPERTY(QString cityFilter READ cityFilter NOTIFY queryChanged)
    Q_PROPERTY(QString sublocationFilter READ sublocationFilter NOTIFY queryChanged)
    Q_PROPERTY(QString locationFilter READ locationFilter NOTIFY queryChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY queryChanged)
    Q_PROPERTY(QString sortDirection READ sortDirection NOTIFY queryChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter NOTIFY queryChanged)
    Q_PROPERTY(QVariantList cameraFacets READ cameraFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList lensFacets READ lensFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList lensNameFacets READ lensNameFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList captureDateFacets READ captureDateFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList countryFacets READ countryFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList provinceStateFacets READ provinceStateFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList cityFacets READ cityFacets NOTIFY facetsChanged)
    Q_PROPERTY(QVariantList sublocationFacets READ sublocationFacets NOTIFY facetsChanged)
    Q_PROPERTY(bool facetCountsScoped READ facetCountsScoped NOTIFY facetsChanged)

public:
    explicit StudioLibraryPresenter(QObject *parent = nullptr);

    [[nodiscard]] QVariantList cameraFacets() const;
    [[nodiscard]] QVariantList lensFacets() const;
    [[nodiscard]] QVariantList lensNameFacets() const;
    [[nodiscard]] QVariantList captureDateFacets() const;
    [[nodiscard]] QVariantList countryFacets() const;
    [[nodiscard]] QVariantList provinceStateFacets() const;
    [[nodiscard]] QVariantList cityFacets() const;
    [[nodiscard]] QVariantList sublocationFacets() const;
    [[nodiscard]] bool facetCountsScoped() const noexcept;

    void apply(LibraryCaptureFacets capture, LibraryLocationFacets location);

    // Explicit snapshots from catalog open/resume/import and scope navigation.
    // Replacement notifies bindings but does not enqueue a second listing.
    [[nodiscard]] const LibraryQuery &query() const noexcept;
    void replaceQuery(LibraryQuery query);
    void setFolderScope(std::string folder_uri);
    void setCollectionScope(std::string collection_id);
    void setImportScope(std::optional<std::int64_t> after, std::optional<std::int64_t> before);
    void resetFilters(std::optional<std::int64_t> import_after,
                      std::optional<std::int64_t> import_before);

    [[nodiscard]] QString ratingFilterMode() const;
    [[nodiscard]] int ratingFilterValue() const noexcept;
    [[nodiscard]] QStringList colorFilters() const;
    [[nodiscard]] QString rejectFilter() const;
    [[nodiscard]] QString filterText() const;
    [[nodiscard]] QString mediaFilter() const;
    [[nodiscard]] QString editFilter() const;
    [[nodiscard]] QString cameraFilter() const;
    [[nodiscard]] QString cameraMakeFilter() const;
    [[nodiscard]] QString cameraModelFilter() const;
    [[nodiscard]] QString lensFilter() const;
    [[nodiscard]] QString lensMakeFilter() const;
    [[nodiscard]] QString lensModelFilter() const;
    [[nodiscard]] QString captureDateFilter() const;
    [[nodiscard]] QString countryFilter() const;
    [[nodiscard]] QString provinceStateFilter() const;
    [[nodiscard]] QString cityFilter() const;
    [[nodiscard]] QString sublocationFilter() const;
    [[nodiscard]] QString locationFilter() const;
    [[nodiscard]] QString sortField() const;
    [[nodiscard]] QString sortDirection() const;
    [[nodiscard]] QString tagFilter() const;
    Q_INVOKABLE void setTagFilter(const QString &tag);
    Q_INVOKABLE void setRatingFilter(const QString &mode, int value);
    Q_INVOKABLE void toggleColorFilter(const QString &label);
    Q_INVOKABLE void setRejectFilter(const QString &mode);
    Q_INVOKABLE void setPickFilter(const QString &mode);
    Q_INVOKABLE void setCullFlagFilter(const QString &mode);
    [[nodiscard]] QString pickFilter() const;
    [[nodiscard]] QString cullFlagFilter() const;
    Q_INVOKABLE void setFilterText(const QString &text);
    Q_INVOKABLE void setMediaFilter(const QString &mode);
    Q_INVOKABLE void setEditFilter(const QString &mode);
    Q_INVOKABLE void setCameraFacetFilter(const QString &make, const QString &model);
    Q_INVOKABLE void setLensFacetFilter(const QString &focal_mm);
    Q_INVOKABLE void setLensNameFacetFilter(const QString &make, const QString &model);
    Q_INVOKABLE void setCaptureDateFacetFilter(const QString &local_date);
    Q_INVOKABLE void setLocationFacetFilter(const QString &country, const QString &province_state,
                                            const QString &city, const QString &sublocation);
    Q_INVOKABLE void setSort(const QString &field, const QString &direction);

signals:
    void queryChanged();
    void reloadRequested();
    void errorOccurred(QString error);
    void facetsChanged();

private:
    LibraryQuery query_;
    LibraryCaptureFacets capture_facets_;
    LibraryLocationFacets location_facets_;
};

} // namespace ravo
