#include "ravo/desktop/asset_list_model.h"

#include <utility>

#include <QUrl>

#include "ravo/domain/types.h"
#include "studio_qt.h"

namespace ravo
{
AssetListModel::AssetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AssetListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(assets_.size());
}

QVariant AssetListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    const auto &asset = assets_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case AssetIdRole:
        return qstring_from_utf8(asset.id);
    case DisplayNameRole:
        return qstring_from_utf8(asset_display_name(asset));
    case MediaTypeRole:
        return qstring_from_utf8(asset.media_type);
    case ImportStateRole:
        return qstring_from_utf8(asset.import_state);
    case ErrorRole:
        return asset.error_message ? qstring_from_utf8(*asset.error_message) : QString{};
    case RatingRole:
        return asset.review.rating;
    case ColorLabelRole:
        return qstring_from_utf8(color_label_name(asset.review.color_label));
    case RejectedRole:
        return asset.review.rejected;
    case ThumbnailUrlRole:
    {
        const auto found = thumbnail_urls_.find(asset.id);
        return found == thumbnail_urls_.end() ? QUrl{} : found->second;
    }
    case ThumbnailStateRole:
    {
        const auto found = thumbnail_states_.find(asset.id);
        if (found != thumbnail_states_.end())
        {
            return found->second;
        }
        return asset.import_state == kImportStateMissing ? QStringLiteral("missing") :
                                                           QStringLiteral("pending");
    }
    case WidthRole:
        return asset.width.value_or(0);
    case HeightRole:
        return asset.height.value_or(0);
    case HasEditsRole:
        return asset.has_edits;
    case SelectedRole:
        return selected_ids_.contains(asset.id);
    default:
        return {};
    }
}

QHash<int, QByteArray> AssetListModel::roleNames() const
{
    return {{AssetIdRole, "assetId"},           {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},       {ImportStateRole, "importState"},
            {ErrorRole, "errorText"},           {RatingRole, "rating"},
            {ColorLabelRole, "colorLabel"},     {RejectedRole, "rejected"},
            {ThumbnailUrlRole, "thumbnailUrl"}, {ThumbnailStateRole, "thumbnailState"},
            {WidthRole, "pixelWidth"},          {HeightRole, "pixelHeight"},
            {HasEditsRole, "hasEdits"},         {SelectedRole, "selected"}};
}

void AssetListModel::setAssets(std::vector<AssetRecord> assets)
{
    beginResetModel();
    assets_ = std::move(assets);
    std::unordered_map<std::string, QUrl> kept_urls;
    std::unordered_map<std::string, QString> kept_states;
    for (const auto &asset : assets_)
    {
        if (const auto found = thumbnail_urls_.find(asset.id); found != thumbnail_urls_.end())
        {
            kept_urls.emplace(found->first, found->second);
        }
        if (const auto found = thumbnail_states_.find(asset.id); found != thumbnail_states_.end())
        {
            kept_states.emplace(found->first, found->second);
        }
    }
    thumbnail_urls_ = std::move(kept_urls);
    thumbnail_states_ = std::move(kept_states);
    std::unordered_set<std::string> kept_selected;
    for (const auto &asset : assets_)
    {
        if (selected_ids_.contains(asset.id))
        {
            kept_selected.insert(asset.id);
        }
    }
    selected_ids_ = std::move(kept_selected);
    endResetModel();
}

void AssetListModel::setThumbnail(const std::string &asset_id, const QUrl &url,
                                  const QString &state)
{
    thumbnail_urls_[asset_id] = url;
    thumbnail_states_[asset_id] = state;
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ThumbnailUrlRole, ThumbnailStateRole});
}

void AssetListModel::updateAsset(const AssetRecord &asset)
{
    const auto row = indexOf(qstring_from_utf8(asset.id));
    if (row < 0)
    {
        return;
    }
    assets_[static_cast<std::size_t>(row)] = asset;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index);
}

void AssetListModel::markOriginalMissing(const std::string &asset_id)
{
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    auto &asset = assets_[static_cast<std::size_t>(row)];
    asset.import_state = std::string(kImportStateMissing);
    thumbnail_states_[asset_id] = QStringLiteral("missing");
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ImportStateRole, ThumbnailStateRole});
}

void AssetListModel::setSelectedIds(std::unordered_set<std::string> ids)
{
    std::unordered_set<std::string> changed = selected_ids_;
    for (const auto &id : ids)
    {
        changed.insert(id);
    }
    selected_ids_ = std::move(ids);
    for (const auto &id : changed)
    {
        const auto row = indexOf(qstring_from_utf8(id));
        if (row < 0)
        {
            continue;
        }
        const auto model_index = index(row, 0);
        emit dataChanged(model_index, model_index, {SelectedRole});
    }
}

bool AssetListModel::isSelected(const std::string &asset_id) const
{
    return selected_ids_.contains(asset_id);
}

int AssetListModel::indexOf(const QString &asset_id) const
{
    const auto id = utf8_from_qstring(asset_id);
    for (int row = 0; row < static_cast<int>(assets_.size()); ++row)
    {
        if (assets_[static_cast<std::size_t>(row)].id == id)
        {
            return row;
        }
    }
    return -1;
}

std::optional<AssetRecord> AssetListModel::assetById(const QString &asset_id) const
{
    const auto row = indexOf(asset_id);
    if (row < 0)
    {
        return std::nullopt;
    }
    return assets_[static_cast<std::size_t>(row)];
}

QString AssetListModel::assetIdAt(const int row) const
{
    if (row < 0 || row >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    return qstring_from_utf8(assets_[static_cast<std::size_t>(row)].id);
}

} // namespace ravo
