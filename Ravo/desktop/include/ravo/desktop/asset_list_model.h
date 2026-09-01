#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QUrl>
#include <QVariant>

#include "ravo/domain/types.h"

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
        SelectedRole,
        CaptureSummaryRole,
    };

    explicit AssetListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setAssets(std::vector<AssetRecord> assets,
                   std::unordered_map<std::string, QUrl> thumbnail_urls = {},
                   std::unordered_map<std::string, QString> thumbnail_states = {},
                   std::size_t total_count = 0U);
    void setPage(std::size_t offset, std::vector<AssetRecord> assets,
                 std::unordered_map<std::string, QUrl> thumbnail_urls,
                 std::unordered_map<std::string, QString> thumbnail_states,
                 std::size_t total_count);
    void insertAsset(int row, AssetRecord asset);
    void setThumbnail(const std::string &asset_id, const QUrl &url, const QString &state);
    [[nodiscard]] std::vector<AssetRecord> records() const;
    [[nodiscard]] QString thumbnailState(const std::string &asset_id) const;
    void updateAsset(const AssetRecord &asset);
    void markOriginalMissing(const std::string &asset_id);
    void setSelectedIds(std::unordered_set<std::string> ids);
    [[nodiscard]] bool isSelected(const std::string &asset_id) const;
    [[nodiscard]] int indexOf(const QString &asset_id) const;
    [[nodiscard]] std::optional<AssetRecord> assetById(const QString &asset_id) const;
    [[nodiscard]] QString assetIdAt(int row) const;
    [[nodiscard]] bool rowLoaded(int row) const noexcept;
    [[nodiscard]] int loadedCount() const noexcept;

private:
    struct Page
    {
        int first = 0;
        int count = 0;
    };

    void trimPages();

    std::map<int, AssetRecord> assets_;
    std::deque<Page> pages_;
    int total_count_ = 0;
    std::unordered_map<std::string, QUrl> thumbnail_urls_;
    std::unordered_map<std::string, QString> thumbnail_states_;
    std::unordered_set<std::string> selected_ids_;
};

} // namespace ravo
