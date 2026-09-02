#include "ravo/desktop/folder_list_model.h"

#include <algorithm>
#include <utility>

#include <QVariantList>

#include "studio_qt.h"

namespace ravo
{

FolderListModel::FolderListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FolderListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(folders_.size());
}

QVariant FolderListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(folders_.size()))
    {
        return {};
    }
    const auto &row = folders_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case FolderIdRole:
        return qstring_from_utf8(row.folder.id);
    case FolderUriRole:
        return qstring_from_utf8(row.folder.uri);
    case DisplayNameRole:
        return qstring_from_utf8(row.folder.display_name);
    case DepthRole:
        return row.folder.depth;
    case AssetCountRole:
        return row.folder.asset_count;
    case HasChildrenRole:
        return row.has_children;
    case HasNextSiblingRole:
        return row.has_next_sibling;
    case AncestorLineContinuesRole:
    {
        QVariantList continues;
        continues.reserve(static_cast<qsizetype>(row.ancestor_line_continues.size()));
        for (const char value : row.ancestor_line_continues)
        {
            continues.push_back(value != 0);
        }
        return continues;
    }
    case CollapsedRole:
        return collapsed_.contains(row.folder.uri);
    case MissingRole:
        return row.folder.missing;
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderListModel::roleNames() const
{
    return {{FolderIdRole, "folderId"},
            {FolderUriRole, "folderUri"},
            {DisplayNameRole, "displayName"},
            {DepthRole, "depth"},
            {AssetCountRole, "assetCount"},
            {HasChildrenRole, "hasChildren"},
            {HasNextSiblingRole, "hasNextSibling"},
            {AncestorLineContinuesRole, "ancestorLineContinues"},
            {CollapsedRole, "collapsed"},
            {MissingRole, "missing"}};
}

void FolderListModel::setFolders(std::vector<FolderRecord> folders)
{
    std::vector<FolderRow> next;
    next.reserve(folders.size());
    for (std::size_t index = 0; index < folders.size(); ++index)
    {
        FolderRow row;
        row.has_children =
            index + 1 < folders.size() && folders[index + 1U].depth == folders[index].depth + 1;
        row.folder = std::move(folders[index]);
        next.push_back(std::move(row));
    }

    std::unordered_set<std::string> kept;
    for (const auto &row : next)
    {
        if (collapsed_.contains(row.folder.uri))
            kept.insert(row.folder.uri);
    }
    const bool collapse_unchanged = kept.size() == collapsed_.size();
    collapsed_ = std::move(kept);

    const bool structure_same =
        collapse_unchanged && next.size() == all_folders_.size() &&
        std::equal(next.begin(), next.end(), all_folders_.begin(),
                   [](const FolderRow &left, const FolderRow &right)
                   {
                       return left.has_children == right.has_children &&
                              left.folder.id == right.folder.id &&
                              left.folder.uri == right.folder.uri &&
                              left.folder.depth == right.folder.depth;
                   });
    if (structure_same)
    {
        const bool payload_same =
            std::equal(next.begin(), next.end(), all_folders_.begin(),
                       [](const FolderRow &left, const FolderRow &right)
                       {
                           return left.folder.display_name == right.folder.display_name &&
                                  left.folder.asset_count == right.folder.asset_count &&
                                  left.folder.missing == right.folder.missing;
                       });
        if (payload_same)
            return;
        all_folders_ = std::move(next);
        std::vector<FolderRow> visible;
        visible.reserve(all_folders_.size());
        for (std::size_t index = 0; index < all_folders_.size(); ++index)
        {
            if (!hidden_by_collapse(index))
                visible.push_back(all_folders_[index]);
        }
        if (visible.size() == folders_.size())
        {
            folders_ = std::move(visible);
            decorate_visible_guides();
            if (!folders_.empty())
            {
                emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                                 {DisplayNameRole, AssetCountRole, MissingRole});
            }
            return;
        }
    }

    all_folders_ = std::move(next);
    rebuild_visible();
}

void FolderListModel::toggleCollapsed(const QString &uri)
{
    const auto id = utf8_from_qstring(uri);
    bool has_children = false;
    for (const auto &row : all_folders_)
    {
        if (row.folder.uri == id)
        {
            has_children = row.has_children;
            break;
        }
    }
    if (!has_children)
    {
        return;
    }
    if (!collapsed_.insert(id).second)
    {
        collapsed_.erase(id);
    }
    rebuild_visible();
}

bool FolderListModel::hidden_by_collapse(const std::size_t index) const
{
    int depth = all_folders_[index].folder.depth;
    for (std::size_t cursor = index; cursor > 0;)
    {
        --cursor;
        if (all_folders_[cursor].folder.depth < depth)
        {
            if (collapsed_.contains(all_folders_[cursor].folder.uri))
            {
                return true;
            }
            depth = all_folders_[cursor].folder.depth;
        }
    }
    return false;
}

void FolderListModel::rebuild_visible()
{
    beginResetModel();
    folders_.clear();
    folders_.reserve(all_folders_.size());
    for (std::size_t index = 0; index < all_folders_.size(); ++index)
    {
        if (!hidden_by_collapse(index))
        {
            folders_.push_back(all_folders_[index]);
        }
    }
    decorate_visible_guides();
    endResetModel();
}

void FolderListModel::decorate_visible_guides()
{
    for (std::size_t index = 0; index < folders_.size(); ++index)
    {
        const int depth = folders_[index].folder.depth;
        folders_[index].has_next_sibling = false;
        for (std::size_t later = index + 1; later < folders_.size(); ++later)
        {
            if (folders_[later].folder.depth < depth)
            {
                break;
            }
            if (folders_[later].folder.depth == depth)
            {
                folders_[index].has_next_sibling = true;
                break;
            }
        }
        folders_[index].ancestor_line_continues.assign(static_cast<std::size_t>(std::max(0, depth)),
                                                       0);
        for (int level = 0; level < depth; ++level)
        {
            for (std::size_t later = index + 1; later < folders_.size(); ++later)
            {
                if (folders_[later].folder.depth < level)
                {
                    break;
                }
                if (folders_[later].folder.depth == level)
                {
                    folders_[index].ancestor_line_continues[static_cast<std::size_t>(level)] = 1;
                    break;
                }
            }
        }
    }
}

} // namespace ravo
