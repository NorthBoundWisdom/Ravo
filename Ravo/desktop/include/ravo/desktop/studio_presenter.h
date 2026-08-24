#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

class AssetListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        AssetIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        MediaTypeRole,
        ImportStateRole,
        ErrorRole,
        RatingRole,
        ColorLabelRole,
        RejectedRole,
        ThumbnailUrlRole,
        ThumbnailStateRole,
        WidthRole,
        HeightRole,
    };

    explicit AssetListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setAssets(std::vector<AssetRecord> assets);
    void setThumbnail(const std::string &asset_id, const QUrl &url, const QString &state);
    void updateAsset(const AssetRecord &asset);
    [[nodiscard]] int indexOf(const QString &asset_id) const;
    [[nodiscard]] std::optional<AssetRecord> assetById(const QString &asset_id) const;
    [[nodiscard]] QString assetIdAt(int row) const;

private:
    std::vector<AssetRecord> assets_;
    std::unordered_map<std::string, QUrl> thumbnail_urls_;
    std::unordered_map<std::string, QString> thumbnail_states_;
};

class StudioPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool catalogOpen READ catalogOpen NOTIFY catalogChanged)
    Q_PROPERTY(QString catalogPath READ catalogPath NOTIFY catalogChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QString selectedAssetId READ selectedAssetId NOTIFY selectionChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRating READ selectedRating NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedColorLabel READ selectedColorLabel NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedRejected READ selectedRejected NOTIFY selectionChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString browseMode READ browseMode NOTIFY browseModeChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(QString ratingFilterMode READ ratingFilterMode NOTIFY filterChanged)
    Q_PROPERTY(int ratingFilterValue READ ratingFilterValue NOTIFY filterChanged)
    Q_PROPERTY(QStringList colorFilters READ colorFilters NOTIFY filterChanged)
    Q_PROPERTY(QString rejectFilter READ rejectFilter NOTIFY filterChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY filterChanged)
    Q_PROPERTY(QString sortDirection READ sortDirection NOTIFY filterChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY filterChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filterChanged)
    Q_PROPERTY(AssetListModel *assets READ assets CONSTANT)
    Q_PROPERTY(QUrl defaultCatalogFolder READ defaultCatalogFolder CONSTANT)
    Q_PROPERTY(QUrl defaultCatalogFile READ defaultCatalogFile CONSTANT)

public:
    explicit StudioPresenter(QObject *parent = nullptr);
    ~StudioPresenter() override;

    [[nodiscard]] bool catalogOpen() const noexcept;
    [[nodiscard]] QString catalogPath() const;
    [[nodiscard]] QUrl defaultCatalogFolder() const;
    [[nodiscard]] QUrl defaultCatalogFile() const;
    Q_INVOKABLE bool defaultCatalogExists() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString selectedAssetId() const;
    [[nodiscard]] int selectedIndex() const;
    [[nodiscard]] int selectedRating() const;
    [[nodiscard]] QString selectedColorLabel() const;
    [[nodiscard]] bool selectedRejected() const noexcept;
    [[nodiscard]] QUrl previewUrl() const;
    [[nodiscard]] bool previewLoading() const noexcept;
    [[nodiscard]] QString browseMode() const;
    [[nodiscard]] QString zoomMode() const;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] int thumbnailSize() const noexcept;
    [[nodiscard]] QString ratingFilterMode() const;
    [[nodiscard]] int ratingFilterValue() const noexcept;
    [[nodiscard]] QStringList colorFilters() const;
    [[nodiscard]] QString rejectFilter() const;
    [[nodiscard]] QString sortField() const;
    [[nodiscard]] QString sortDirection() const;
    [[nodiscard]] int visibleCount() const;
    [[nodiscard]] bool filtersActive() const noexcept;
    [[nodiscard]] AssetListModel *assets() noexcept;

    Q_INVOKABLE void createCatalog(const QUrl &file_url);
    Q_INVOKABLE void openCatalog(const QUrl &file_url);
    Q_INVOKABLE void importFiles(const QList<QUrl> &files);
    Q_INVOKABLE void importFolder(const QUrl &folder_url);
    Q_INVOKABLE void createCatalogFromPath(const QString &path);
    Q_INVOKABLE void openCatalogFromPath(const QString &path);
    Q_INVOKABLE void importFilePaths(const QStringList &paths);
    Q_INVOKABLE void importFolderFromPath(const QString &path);
    Q_INVOKABLE void selectAsset(const QString &asset_id);
    Q_INVOKABLE void selectNext();
    Q_INVOKABLE void selectPrevious();
    Q_INVOKABLE void setBrowseMode(const QString &mode);
    Q_INVOKABLE void openLoupe();
    Q_INVOKABLE void returnToGrid();
    Q_INVOKABLE void setZoomMode(const QString &mode);
    Q_INVOKABLE void setZoomFactor(double factor);
    Q_INVOKABLE void adjustZoom(int wheel_delta);
    Q_INVOKABLE void setThumbnailSize(int size);
    Q_INVOKABLE void setRating(int rating);
    Q_INVOKABLE void setColorLabel(const QString &label);
    Q_INVOKABLE void toggleRejected();
    Q_INVOKABLE void setRatingFilter(const QString &mode, int value);
    Q_INVOKABLE void toggleColorFilter(const QString &label);
    Q_INVOKABLE void setRejectFilter(const QString &mode);
    Q_INVOKABLE void setSort(const QString &field, const QString &direction);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void ensureThumbnail(const QString &asset_id);

signals:
    void catalogChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void browseModeChanged();
    void zoomChanged();
    void thumbnailSizeChanged();
    void filterChanged();

private:
    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets, bool restore_selection);
    void requestPreviewForSelection();
    void reloadVisibleAssets();
    [[nodiscard]] LibraryQuery current_query() const;
    [[nodiscard]] Result<void> open_on_worker(const std::string &path, bool create);
    void mutate_selected_review(const std::function<Result<AssetRecord>(CatalogService &)> &action);

    SerialExecutor executor_;
    std::optional<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    CancellationSource shutdown_;
    AssetListModel assets_;
    LibraryQuery query_;
    QString catalog_path_;
    QString status_text_{QStringLiteral("Create or open a library to import photos.")};
    QString error_text_;
    QString selected_asset_id_;
    QUrl preview_url_;
    QString browse_mode_{QStringLiteral("grid")};
    QString zoom_mode_{QStringLiteral("fit")};
    double zoom_factor_ = 1.0;
    int thumbnail_size_ = 180;
    bool busy_ = false;
    bool preview_loading_ = false;
    std::uint64_t preview_revision_ = 0;
    std::uint64_t thumbnail_revision_ = 0;
};

} // namespace ravo
