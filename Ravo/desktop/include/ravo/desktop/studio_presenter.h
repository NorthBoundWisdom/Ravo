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
#include <QVariant>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/recipe/develop.h"
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
        HasEditsRole,
    };

    explicit AssetListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setAssets(std::vector<AssetRecord> assets);
    void setThumbnail(const std::string &asset_id, const QUrl &url, const QString &state);
    void updateAsset(const AssetRecord &asset);
    void markOriginalMissing(const std::string &asset_id);
    [[nodiscard]] int indexOf(const QString &asset_id) const;
    [[nodiscard]] std::optional<AssetRecord> assetById(const QString &asset_id) const;
    [[nodiscard]] QString assetIdAt(int row) const;

private:
    std::vector<AssetRecord> assets_;
    std::unordered_map<std::string, QUrl> thumbnail_urls_;
    std::unordered_map<std::string, QString> thumbnail_states_;
};

class FolderListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        FolderUriRole = Qt::UserRole + 1,
        DisplayNameRole,
        DepthRole,
        AssetCountRole,
    };

    explicit FolderListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setFolders(std::vector<FolderRecord> folders);

private:
    std::vector<FolderRecord> folders_;
};

class StudioPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool catalogOpen READ catalogOpen NOTIFY catalogChanged)
    Q_PROPERTY(bool nightMode READ nightMode WRITE setNightMode NOTIFY nightModeChanged)
    Q_PROPERTY(QString catalogPath READ catalogPath NOTIFY catalogChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QString selectedAssetId READ selectedAssetId NOTIFY selectionChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRating READ selectedRating NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedColorLabel READ selectedColorLabel NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedRejected READ selectedRejected NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedImportState READ selectedImportState NOTIFY selectionChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString browseMode READ browseMode NOTIFY browseModeChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(
        int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(QString ratingFilterMode READ ratingFilterMode NOTIFY filterChanged)
    Q_PROPERTY(int ratingFilterValue READ ratingFilterValue NOTIFY filterChanged)
    Q_PROPERTY(QStringList colorFilters READ colorFilters NOTIFY filterChanged)
    Q_PROPERTY(QString rejectFilter READ rejectFilter NOTIFY filterChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY filterChanged)
    Q_PROPERTY(QString sortDirection READ sortDirection NOTIFY filterChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY filterChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filterChanged)
    Q_PROPERTY(bool selectedHasEdits READ selectedHasEdits NOTIFY selectionChanged)
    Q_PROPERTY(bool beforeAfter READ beforeAfter NOTIFY editChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editChanged)
    Q_PROPERTY(double editTemperature READ editTemperature NOTIFY editChanged)
    Q_PROPERTY(double editTint READ editTint NOTIFY editChanged)
    Q_PROPERTY(double editExposure READ editExposure NOTIFY editChanged)
    Q_PROPERTY(double editContrast READ editContrast NOTIFY editChanged)
    Q_PROPERTY(double editHighlights READ editHighlights NOTIFY editChanged)
    Q_PROPERTY(double editShadows READ editShadows NOTIFY editChanged)
    Q_PROPERTY(double editWhites READ editWhites NOTIFY editChanged)
    Q_PROPERTY(double editBlacks READ editBlacks NOTIFY editChanged)
    Q_PROPERTY(double editVibrance READ editVibrance NOTIFY editChanged)
    Q_PROPERTY(double editSaturation READ editSaturation NOTIFY editChanged)
    Q_PROPERTY(int editRotateQuarters READ editRotateQuarters NOTIFY editChanged)
    Q_PROPERTY(double editCropX READ editCropX NOTIFY editChanged)
    Q_PROPERTY(double editCropY READ editCropY NOTIFY editChanged)
    Q_PROPERTY(double editCropWidth READ editCropWidth NOTIFY editChanged)
    Q_PROPERTY(double editCropHeight READ editCropHeight NOTIFY editChanged)
    Q_PROPERTY(AssetListModel *assets READ assets CONSTANT)
    Q_PROPERTY(FolderListModel *folders READ folders CONSTANT)
    Q_PROPERTY(QString selectedFolderUri READ selectedFolderUri NOTIFY folderChanged)
    Q_PROPERTY(QString selectedDisplayName READ selectedDisplayName NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedFolderPath READ selectedFolderPath NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedMediaType READ selectedMediaType NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedDimensions READ selectedDimensions NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedFileSize READ selectedFileSize NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedUri READ selectedUri NOTIFY selectionChanged)
    Q_PROPERTY(QUrl defaultCatalogFolder READ defaultCatalogFolder CONSTANT)
    Q_PROPERTY(QUrl defaultCatalogFile READ defaultCatalogFile CONSTANT)

public:
    explicit StudioPresenter(QObject *parent = nullptr);
    ~StudioPresenter() override;

    [[nodiscard]] bool catalogOpen() const noexcept;
    [[nodiscard]] bool nightMode() const noexcept;
    void setNightMode(bool night);
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
    [[nodiscard]] QString selectedImportState() const;
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
    [[nodiscard]] bool selectedHasEdits() const noexcept;
    [[nodiscard]] bool beforeAfter() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] double editTemperature() const noexcept;
    [[nodiscard]] double editTint() const noexcept;
    [[nodiscard]] double editExposure() const noexcept;
    [[nodiscard]] double editContrast() const noexcept;
    [[nodiscard]] double editHighlights() const noexcept;
    [[nodiscard]] double editShadows() const noexcept;
    [[nodiscard]] double editWhites() const noexcept;
    [[nodiscard]] double editBlacks() const noexcept;
    [[nodiscard]] double editVibrance() const noexcept;
    [[nodiscard]] double editSaturation() const noexcept;
    [[nodiscard]] int editRotateQuarters() const noexcept;
    [[nodiscard]] double editCropX() const noexcept;
    [[nodiscard]] double editCropY() const noexcept;
    [[nodiscard]] double editCropWidth() const noexcept;
    [[nodiscard]] double editCropHeight() const noexcept;
    [[nodiscard]] AssetListModel *assets() noexcept;
    [[nodiscard]] FolderListModel *folders() noexcept;
    [[nodiscard]] QString selectedFolderUri() const;
    [[nodiscard]] QString selectedDisplayName() const;
    [[nodiscard]] QString selectedFolderPath() const;
    [[nodiscard]] QString selectedMediaType() const;
    [[nodiscard]] QString selectedDimensions() const;
    [[nodiscard]] QString selectedFileSize() const;
    [[nodiscard]] QString selectedUri() const;

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
    Q_INVOKABLE void openDevelop();
    Q_INVOKABLE void returnToGrid();
    Q_INVOKABLE void setDevelopNumber(const QString &name, double value);
    Q_INVOKABLE void rotateLeft();
    Q_INVOKABLE void rotateRight();
    Q_INVOKABLE void resetControl(const QString &name);
    Q_INVOKABLE void resetSection(const QString &section);
    Q_INVOKABLE void resetAllEdits();
    Q_INVOKABLE void undoEdit();
    Q_INVOKABLE void redoEdit();
    Q_INVOKABLE void toggleBeforeAfter();
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
    Q_INVOKABLE void selectFolder(const QString &folder_uri);
    Q_INVOKABLE void ensureThumbnail(const QString &asset_id);
    Q_INVOKABLE void executeCommand(const QString &id, const QVariant &argument = QVariant());

signals:
    void catalogChanged();
    void nightModeChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void browseModeChanged();
    void zoomChanged();
    void thumbnailSizeChanged();
    void filterChanged();
    void folderChanged();
    void editChanged();
    void uiCommandRequested(const QString &id);

private:
    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets, bool restore_selection);
    void applyFolders(std::vector<FolderRecord> folders);
    void requestPreviewForSelection();
    void reloadVisibleAssets();
    void load_develop_for_selection();
    void commit_develop(DevelopParams params, bool push_history);
    [[nodiscard]] LibraryQuery current_query() const;
    [[nodiscard]] Result<std::unique_ptr<CatalogService>>
    make_catalog_service(const std::string &path, bool create);
    void mutate_selected_review(const std::function<Result<AssetRecord>(CatalogService &)> &action);
    void remove_selected_from_catalog();

    SerialExecutor executor_;
    std::optional<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    CancellationSource shutdown_;
    AssetListModel assets_;
    FolderListModel folders_;
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
    bool night_mode_ = false;
    bool preview_loading_ = false;
    std::uint64_t preview_revision_ = 0;
    std::uint64_t thumbnail_revision_ = 0;
    std::unordered_map<std::string, std::uint64_t> thumbnail_requests_;
    DevelopParams develop_{};
    std::vector<DevelopParams> undo_stack_;
    std::vector<DevelopParams> redo_stack_;
    bool before_after_ = false;
};

} // namespace ravo
