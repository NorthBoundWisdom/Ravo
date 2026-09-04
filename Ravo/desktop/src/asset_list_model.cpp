#include "ravo/desktop/asset_list_model.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <map>
#include <utility>

#include <QModelIndex>
#include <QStringList>
#include <QUrl>

#include "ravo/domain/types.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{

constexpr std::size_t kMaximumResidentPages = 3U;

QString compact_capture_summary(const AssetRecord &asset)
{
    QStringList parts;
    if (asset.capture.iso)
        parts.push_back(QStringLiteral("ISO %1").arg(*asset.capture.iso, 0, 'f', 0));
    if (asset.capture.aperture)
        parts.push_back(QStringLiteral("f/%1").arg(*asset.capture.aperture, 0, 'f', 1));
    if (asset.capture.focal_length_mm)
        parts.push_back(QStringLiteral("%1 mm").arg(*asset.capture.focal_length_mm, 0, 'f', 0));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

AssetListModel::AssetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AssetListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : total_count_;
}

QVariant AssetListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= total_count_)
        return {};
    const auto found = assets_.find(index.row());
    if (found == assets_.end())
    {
        switch (role)
        {
        case AssetIdRole:
        case DisplayNameRole:
        case MediaTypeRole:
        case ErrorRole:
        case CaptureSummaryRole:
            return QString{};
        case ImportStateRole:
            return QStringLiteral("loading");
        case RatingRole:
        case WidthRole:
        case HeightRole:
        case VersionOrdinalRole:
        case StackCountRole:
        case StackPositionRole:
            return 0;
        case ColorLabelRole:
            return QStringLiteral("none");
        case SourceAssetIdRole:
        case StackIdRole:
            return QString{};
        case RejectedRole:
        case PickedRole:
        case HasEditsRole:
        case SelectedRole:
        case StackPickRole:
            return false;
        case ThumbnailUrlRole:
            return QUrl{};
        case ThumbnailStateRole:
            return QStringLiteral("unloaded");
        default:
            return {};
        }
    }
    const auto &asset = found->second;
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
    case PickedRole:
        return asset.review.picked;
    case ThumbnailUrlRole:
    {
        const auto thumbnail = thumbnail_urls_.find(asset.id);
        return thumbnail == thumbnail_urls_.end() ? QUrl{} : thumbnail->second;
    }
    case ThumbnailStateRole:
    {
        const auto state = thumbnail_states_.find(asset.id);
        if (state != thumbnail_states_.end())
            return state->second;
        if (asset.import_state == kImportStateMissing)
            return QStringLiteral("missing");
        if (asset.import_state == kImportStateFailed)
            return QStringLiteral("failed");
        return QStringLiteral("pending");
    }
    case WidthRole:
        return asset.width.value_or(0);
    case HeightRole:
        return asset.height.value_or(0);
    case HasEditsRole:
        return asset.has_edits;
    case SelectedRole:
        return selected_ids_.contains(asset.id);
    case CaptureSummaryRole:
        return compact_capture_summary(asset);
    case VersionOrdinalRole:
        return asset.version_ordinal;
    case SourceAssetIdRole:
        return asset.source_asset_id ? qstring_from_utf8(*asset.source_asset_id) : QString{};
    case StackIdRole:
        return asset.stack_id ? qstring_from_utf8(*asset.stack_id) : QString{};
    case StackCountRole:
        return asset.stack_count;
    case StackPickRole:
        return asset.stack_pick;
    case StackPositionRole:
        return asset.stack_position;
    default:
        return {};
    }
}

QHash<int, QByteArray> AssetListModel::roleNames() const
{
    return {{AssetIdRole, "assetId"},
            {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},
            {ImportStateRole, "importState"},
            {ErrorRole, "errorText"},
            {RatingRole, "rating"},
            {ColorLabelRole, "colorLabel"},
            {RejectedRole, "rejected"},
            {PickedRole, "picked"},
            {ThumbnailUrlRole, "thumbnailUrl"},
            {ThumbnailStateRole, "thumbnailState"},
            {WidthRole, "pixelWidth"},
            {HeightRole, "pixelHeight"},
            {HasEditsRole, "hasEdits"},
            {SelectedRole, "selected"},
            {CaptureSummaryRole, "captureSummary"},
            {VersionOrdinalRole, "versionOrdinal"},
            {SourceAssetIdRole, "sourceAssetId"},
            {StackIdRole, "stackId"},
            {StackCountRole, "stackCount"},
            {StackPickRole, "stackPick"},
            {StackPositionRole, "stackPosition"}};
}

void AssetListModel::setAssets(std::vector<AssetRecord> assets,
                               std::unordered_map<std::string, QUrl> thumbnail_urls,
                               std::unordered_map<std::string, QString> thumbnail_states,
                               const std::size_t total_count)
{
    const auto bounded_total = std::max(total_count, assets.size());
    const int next_total =
        static_cast<int>(std::min<std::size_t>(bounded_total, static_cast<std::size_t>(INT_MAX)));
    auto old_urls = std::move(thumbnail_urls_);
    auto old_states = std::move(thumbnail_states_);
    beginResetModel();
    assets_.clear();
    pages_.clear();
    total_count_ = next_total;
    for (std::size_t offset = 0; offset < assets.size(); ++offset)
        assets_.emplace(static_cast<int>(offset), std::move(assets[offset]));
    if (!assets_.empty())
        pages_.push_back({0, static_cast<int>(assets_.size())});
    thumbnail_urls_ = std::move(thumbnail_urls);
    thumbnail_states_ = std::move(thumbnail_states);
    for (const auto &[row, asset] : assets_)
    {
        static_cast<void>(row);
        if (!thumbnail_urls_.contains(asset.id))
            if (const auto found = old_urls.find(asset.id); found != old_urls.end())
                thumbnail_urls_.emplace(found->first, found->second);
        if (!thumbnail_states_.contains(asset.id))
            if (const auto found = old_states.find(asset.id); found != old_states.end())
                thumbnail_states_.emplace(found->first, found->second);
    }
    endResetModel();
}

void AssetListModel::setPage(std::size_t offset, std::vector<AssetRecord> assets,
                             std::unordered_map<std::string, QUrl> thumbnail_urls,
                             std::unordered_map<std::string, QString> thumbnail_states,
                             const std::size_t total_count)
{
    if (offset > static_cast<std::size_t>(INT_MAX) || assets.empty())
        return;
    const int first = static_cast<int>(offset);
    const int count = static_cast<int>(std::min<std::size_t>(
        assets.size(), static_cast<std::size_t>(std::max(0, total_count_ - first))));
    if (count <= 0)
        return;
    if (total_count != static_cast<std::size_t>(total_count_))
    {
        beginResetModel();
        total_count_ =
            static_cast<int>(std::min<std::size_t>(total_count, static_cast<std::size_t>(INT_MAX)));
        endResetModel();
    }
    for (int index = 0; index < count; ++index)
        assets_.insert_or_assign(first + index, std::move(assets[static_cast<std::size_t>(index)]));
    for (auto &[id, url] : thumbnail_urls)
        thumbnail_urls_.insert_or_assign(std::move(id), std::move(url));
    for (auto &[id, state] : thumbnail_states)
        thumbnail_states_.insert_or_assign(std::move(id), std::move(state));
    std::erase_if(pages_, [first](const Page &page) { return page.first == first; });
    pages_.push_back({first, count});
    trimPages();
    emit dataChanged(this->index(first, 0), this->index(first + count - 1, 0));
}

void AssetListModel::trimPages()
{
    while (pages_.size() > kMaximumResidentPages)
    {
        auto candidate = pages_.begin();
        for (; candidate != pages_.end(); ++candidate)
        {
            bool contains_selection = false;
            for (int row = candidate->first; row < candidate->first + candidate->count; ++row)
            {
                const auto found = assets_.find(row);
                if (found != assets_.end() && selected_ids_.contains(found->second.id))
                {
                    contains_selection = true;
                    break;
                }
            }
            if (!contains_selection)
                break;
        }
        if (candidate == pages_.end())
            break;
        const Page removed = *candidate;
        pages_.erase(candidate);
        for (int row = removed.first; row < removed.first + removed.count; ++row)
        {
            const auto found = assets_.find(row);
            if (found == assets_.end())
                continue;
            thumbnail_urls_.erase(found->second.id);
            thumbnail_states_.erase(found->second.id);
            assets_.erase(found);
        }
        emit dataChanged(index(removed.first, 0),
                         index(std::min(total_count_ - 1, removed.first + removed.count - 1), 0));
    }
}

void AssetListModel::insertAsset(const int row, AssetRecord asset)
{
    const int clamped = std::clamp(row, 0, total_count_);
    beginInsertRows(QModelIndex{}, clamped, clamped);
    std::map<int, AssetRecord> shifted;
    for (auto &[existing_row, existing] : assets_)
        shifted.emplace(existing_row >= clamped ? existing_row + 1 : existing_row,
                        std::move(existing));
    shifted.insert_or_assign(clamped, std::move(asset));
    assets_ = std::move(shifted);
    for (auto &page : pages_)
    {
        if (page.first >= clamped)
            ++page.first;
        else if (clamped < page.first + page.count)
            ++page.count;
    }
    pages_.push_back({clamped, 1});
    ++total_count_;
    endInsertRows();
    trimPages();
}

void AssetListModel::replaceAssetAt(const int row, AssetRecord asset)
{
    if (row < 0 || row >= total_count_)
        return;
    assets_.insert_or_assign(row, std::move(asset));
    emit dataChanged(index(row, 0), index(row, 0));
}

void AssetListModel::setThumbnail(const std::string &asset_id, const QUrl &url,
                                  const QString &state)
{
    thumbnail_urls_[asset_id] = url;
    thumbnail_states_[asset_id] = state;
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
        return;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ThumbnailUrlRole, ThumbnailStateRole});
}

std::vector<AssetRecord> AssetListModel::records() const
{
    std::vector<AssetRecord> records;
    records.reserve(assets_.size());
    for (const auto &[row, asset] : assets_)
    {
        static_cast<void>(row);
        records.push_back(asset);
    }
    return records;
}

QString AssetListModel::thumbnailState(const std::string &asset_id) const
{
    const auto found = thumbnail_states_.find(asset_id);
    return found == thumbnail_states_.end() ? QStringLiteral("pending") : found->second;
}

void AssetListModel::updateAsset(const AssetRecord &asset)
{
    const auto row = indexOf(qstring_from_utf8(asset.id));
    if (row < 0)
        return;
    assets_.insert_or_assign(row, asset);
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index);
}

void AssetListModel::markOriginalMissing(const std::string &asset_id)
{
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
        return;
    auto found = assets_.find(row);
    found->second.import_state = std::string(kImportStateMissing);
    if (thumbnail_states_[asset_id] != QStringLiteral("proxy"))
        thumbnail_states_[asset_id] = QStringLiteral("missing");
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ImportStateRole, ThumbnailStateRole});
}

void AssetListModel::setSelectedIds(std::unordered_set<std::string> ids)
{
    std::unordered_set<std::string> changed = selected_ids_;
    for (const auto &id : ids)
        changed.insert(id);
    selected_ids_ = std::move(ids);
    for (const auto &id : changed)
    {
        const auto row = indexOf(qstring_from_utf8(id));
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, 0), {SelectedRole});
    }
}

bool AssetListModel::isSelected(const std::string &asset_id) const
{
    return selected_ids_.contains(asset_id);
}

int AssetListModel::indexOf(const QString &asset_id) const
{
    const auto id = utf8_from_qstring(asset_id);
    for (const auto &[row, asset] : assets_)
        if (asset.id == id)
            return row;
    return -1;
}

std::optional<AssetRecord> AssetListModel::assetById(const QString &asset_id) const
{
    const auto row = indexOf(asset_id);
    if (row < 0)
        return std::nullopt;
    return assets_.at(row);
}

QString AssetListModel::assetIdAt(const int row) const
{
    const auto found = assets_.find(row);
    return found == assets_.end() ? QString{} : qstring_from_utf8(found->second.id);
}

bool AssetListModel::rowLoaded(const int row) const noexcept
{
    return assets_.contains(row);
}

int AssetListModel::loadedCount() const noexcept
{
    return static_cast<int>(assets_.size());
}

} // namespace ravo
