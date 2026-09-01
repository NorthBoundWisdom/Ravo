#include "ravo/desktop/library_set_list_model.h"

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
    beginResetModel();
    sets_ = std::move(sets);
    selected_id_ = std::move(selected_id);
    endResetModel();
}

} // namespace ravo
