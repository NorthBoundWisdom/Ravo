#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>
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
    };

    explicit AssetListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setAssets(std::vector<AssetRecord> assets);

private:
    std::vector<AssetRecord> assets_;
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
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
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
    [[nodiscard]] QUrl previewUrl() const;
    [[nodiscard]] bool previewLoading() const noexcept;
    [[nodiscard]] QString viewMode() const;
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
    void setViewMode(const QString &mode);

signals:
    void catalogChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void viewModeChanged();

private:
    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets);
    void requestPreviewForSelection();
    [[nodiscard]] Result<void> open_on_worker(const std::string &path, bool create);

    SerialExecutor executor_;
    std::optional<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    CancellationSource shutdown_;
    AssetListModel assets_;
    QString catalog_path_;
    QString status_text_{QStringLiteral("Create or open a library to import photos.")};
    QString error_text_;
    QString selected_asset_id_;
    QUrl preview_url_;
    QString view_mode_{QStringLiteral("fit")};
    bool busy_ = false;
    bool preview_loading_ = false;
    std::uint64_t preview_revision_ = 0;
};

} // namespace ravo
