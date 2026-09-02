#include "ravo/desktop/library_set_list_model.h"

#include <algorithm>

#include "studio_qt.h"

namespace ravo
{

LibrarySetListModel::LibrarySetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int LibrarySetListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(sets_.size());
}

QVariant LibrarySetListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(sets_.size()))
        return {};
    const auto &set = sets_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case SetIdRole:
        return qstring_from_utf8(set.id);
    case KindRole:
        return qstring_from_utf8(std::string(library_set_kind_name(set.kind)));
    case NameRole:
        return qstring_from_utf8(set.name);
    case AssetCountRole:
        return static_cast<int>(set.asset_count);
    case SelectedRole:
        return set.id == selected_id_;
    default:
        return {};
    }
}

QHash<int, QByteArray> LibrarySetListModel::roleNames() const
{
    return {{SetIdRole, "setId"},
            {KindRole, "kind"},
            {NameRole, "name"},
            {AssetCountRole, "assetCount"},
            {SelectedRole, "selected"}};
}

void LibrarySetListModel::setSets(std::vector<LibrarySetRecord> sets, std::string selected_id)
{
    const bool same_identity =
        sets_.size() == sets.size() &&
        std::equal(sets_.begin(), sets_.end(), sets.begin(),
                   [](const LibrarySetRecord &left, const LibrarySetRecord &right)
                   { return left.id == right.id && left.kind == right.kind; });
    const bool same_payload =
        same_identity &&
        std::equal(sets_.begin(), sets_.end(), sets.begin(),
                   [](const LibrarySetRecord &left, const LibrarySetRecord &right)
                   {
                       return left.name == right.name && left.asset_count == right.asset_count &&
                              left.query == right.query;
                   });
    if (same_payload && selected_id_ == selected_id)
        return;
    if (same_identity)
    {
        sets_ = std::move(sets);
        selected_id_ = std::move(selected_id);
        if (!sets_.empty())
        {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                             same_payload ? QList<int>{SelectedRole} :
                                            QList<int>{NameRole, AssetCountRole, SelectedRole});
        }
        return;
    }
    beginResetModel();
    sets_ = std::move(sets);
    selected_id_ = std::move(selected_id);
    endResetModel();
}

} // namespace ravo
