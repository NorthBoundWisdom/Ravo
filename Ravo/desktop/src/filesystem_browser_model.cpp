#include "ravo/desktop/filesystem_browser_model.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] QString generic_path(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

[[nodiscard]] std::filesystem::path filesystem_path(const QString &path)
{
    const QByteArray bytes = path.toUtf8();
    const auto *data = reinterpret_cast<const char8_t *>(bytes.constData());
    return std::filesystem::path(std::u8string(data, data + bytes.size()));
}

[[nodiscard]] QString from_filesystem_path(const std::filesystem::path &path)
{
    const auto utf8 = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] bool skip_volume(const QStorageInfo &volume)
{
    if (!volume.isValid() || !volume.isReady() || volume.rootPath().isEmpty())
        return true;
    const auto fs = volume.fileSystemType().toLower();
    if (fs == QLatin1String("proc") || fs == QLatin1String("sysfs") ||
        fs == QLatin1String("devtmpfs") || fs == QLatin1String("tmpfs") || fs.contains("cgroup"))
        return true;
    const auto root = generic_path(volume.rootPath());
    return root == QLatin1String("/proc") || root == QLatin1String("/sys") ||
           root == QLatin1String("/dev") || root == QLatin1String("/run") ||
           root.startsWith(QLatin1String("/proc/")) || root.startsWith(QLatin1String("/sys/"));
}

} // namespace

Result<std::vector<FilesystemFolderEntry>> list_filesystem_folders(const QString &path)
{
    const auto root = generic_path(path);
    if (root.isEmpty())
        return make_error(ErrorCode::kInvalidArgument, "Folder path is empty",
                          {{"reason", "filesystem_folder_path_empty"}});
    std::error_code error;
    const auto fs_path = filesystem_path(root);
    if (!std::filesystem::is_directory(fs_path, error) || error)
        return make_error(ErrorCode::kIo, "Path is not a readable folder",
                          {{"path", utf8_from_qstring(root)},
                           {"reason", "filesystem_folder_not_directory"},
                           {"os_error", error ? error.message() : std::string{}}});
    std::filesystem::directory_iterator it(fs_path, error);
    if (error)
        return make_error(ErrorCode::kIo, "Unable to list folder",
                          {{"path", utf8_from_qstring(root)},
                           {"reason", "filesystem_folder_list_failed"},
                           {"os_error", error.message()}});
    std::vector<FilesystemFolderEntry> entries;
    for (; it != std::filesystem::directory_iterator(); it.increment(error))
    {
        if (error)
            return make_error(ErrorCode::kIo, "Unable to list folder",
                              {{"path", utf8_from_qstring(root)},
                               {"reason", "filesystem_folder_list_failed"},
                               {"os_error", error.message()}});
        std::error_code dir_error;
        if (!it->is_directory(dir_error) || dir_error)
            continue;
        const auto name = from_filesystem_path(it->path().filename());
        if (name.isEmpty() || name.startsWith(QLatin1Char('.')))
            continue;
        FilesystemFolderEntry entry;
        entry.path = generic_path(from_filesystem_path(it->path()));
        entry.display_name = name;
        entry.has_children = true;
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const FilesystemFolderEntry &left, const FilesystemFolderEntry &right)
              { return QString::localeAwareCompare(left.display_name, right.display_name) < 0; });
    return entries;
}

std::vector<FilesystemFolderEntry> mounted_filesystem_roots()
{
    std::vector<FilesystemFolderEntry> roots;
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const auto &volume : volumes)
    {
        if (skip_volume(volume))
            continue;
        FilesystemFolderEntry entry;
        entry.path = generic_path(volume.rootPath());
        entry.display_name = volume.displayName().isEmpty() ? QFileInfo(entry.path).fileName() :
                                                              volume.displayName();
        if (entry.display_name.isEmpty())
            entry.display_name = entry.path;
        entry.has_children = true;
        const bool duplicate =
            std::any_of(roots.begin(), roots.end(), [&](const FilesystemFolderEntry &existing)
                        { return existing.path == entry.path; });
        if (!duplicate)
            roots.push_back(std::move(entry));
    }
    std::sort(roots.begin(), roots.end(),
              [](const FilesystemFolderEntry &left, const FilesystemFolderEntry &right)
              { return QString::localeAwareCompare(left.display_name, right.display_name) < 0; });
    return roots;
}

FilesystemBrowserModel::FilesystemBrowserModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FilesystemBrowserModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(visible_.size());
}

QVariant FilesystemBrowserModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(visible_.size()))
        return {};
    const auto &row = visible_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case PathRole:
        return row.path;
    case DisplayNameRole:
        return row.display_name;
    case DepthRole:
        return row.depth;
    case HasChildrenRole:
        return row.has_children;
    case CollapsedRole:
        return row.collapsed;
    case SelectedRole:
        return row.path == selected_path_;
    case ErrorRole:
        return row.error;
    default:
        return {};
    }
}

QHash<int, QByteArray> FilesystemBrowserModel::roleNames() const
{
    return {{PathRole, "path"},           {DisplayNameRole, "displayName"},
            {DepthRole, "depth"},         {HasChildrenRole, "hasChildren"},
            {CollapsedRole, "collapsed"}, {SelectedRole, "selected"},
            {ErrorRole, "errorText"}};
}

QString FilesystemBrowserModel::selectedPath() const
{
    return selected_path_;
}

void FilesystemBrowserModel::resetWithRoots(std::vector<FilesystemFolderEntry> roots)
{
    beginResetModel();
    all_nodes_.clear();
    all_nodes_.reserve(roots.size());
    for (auto &root : roots)
    {
        Node node;
        node.path = generic_path(root.path);
        node.display_name = root.display_name;
        node.depth = 0;
        node.has_children = root.has_children;
        node.collapsed = true;
        node.loaded = false;
        all_nodes_.push_back(std::move(node));
    }
    visible_ = all_nodes_;
    endResetModel();
    emit selectedPathChanged();
}

void FilesystemBrowserModel::loadMountedVolumes()
{
    resetWithRoots(mounted_filesystem_roots());
}

void FilesystemBrowserModel::applyChildren(const QString &path, const quint64 generation,
                                           Result<std::vector<FilesystemFolderEntry>> children)
{
    const auto parent_index = index_of_path(generic_path(path));
    if (parent_index < 0)
        return;
    auto &parent = all_nodes_[static_cast<std::size_t>(parent_index)];
    if (parent.listing_generation != generation)
        return;
    parent.listing_pending = false;
    if (!children)
    {
        parent.error = qstring_from_utf8(children.error().message);
        parent.loaded = false;
        parent.collapsed = true;
        if (!visible_.empty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {ErrorRole, CollapsedRole});
        return;
    }
    parent.error.clear();
    parent.loaded = true;
    parent.collapsed = false;
    parent.has_children = !children.value().empty();
    const int parent_depth = parent.depth;
    std::size_t remove_from = static_cast<std::size_t>(parent_index) + 1U;
    std::size_t remove_to = remove_from;
    while (remove_to < all_nodes_.size() && all_nodes_[remove_to].depth > parent_depth)
        ++remove_to;
    std::vector<Node> next;
    next.reserve(all_nodes_.size() - (remove_to - remove_from) + children.value().size());
    next.insert(next.end(), all_nodes_.begin(),
                all_nodes_.begin() + static_cast<std::ptrdiff_t>(remove_from));
    for (auto &child : children.value())
    {
        Node node;
        node.path = generic_path(child.path);
        node.display_name = child.display_name;
        node.depth = parent_depth + 1;
        node.has_children = child.has_children;
        node.collapsed = true;
        node.loaded = false;
        next.push_back(std::move(node));
    }
    next.insert(next.end(), all_nodes_.begin() + static_cast<std::ptrdiff_t>(remove_to),
                all_nodes_.end());
    all_nodes_ = std::move(next);
    rebuild_visible();
    if (!reveal_path_.isEmpty())
        revealFolder(reveal_path_);
}

void FilesystemBrowserModel::toggleCollapsed(const QString &path)
{
    const auto node_index = index_of_path(generic_path(path));
    if (node_index < 0)
        return;
    auto &node = all_nodes_[static_cast<std::size_t>(node_index)];
    if (!node.has_children)
        return;
    if (!node.collapsed)
    {
        node.collapsed = true;
        rebuild_visible();
        return;
    }
    if (node.loaded)
    {
        node.collapsed = false;
        node.error.clear();
        rebuild_visible();
        return;
    }
    node.listing_generation = ++next_listing_generation_;
    node.listing_pending = true;
    emit directoryListingRequested(node.path, node.listing_generation);
}

void FilesystemBrowserModel::selectFolder(const QString &path)
{
    const auto next = generic_path(path);
    if (next.isEmpty() || selected_path_ == next)
        return;
    selected_path_ = next;
    if (!visible_.empty())
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    emit selectedPathChanged();
}

void FilesystemBrowserModel::revealFolder(const QString &path)
{
    if (path.isEmpty())
    {
        reveal_path_.clear();
        return;
    }
    reveal_path_ = generic_path(path);
    selectFolder(path);
    for (auto &node : all_nodes_)
    {
        if (node.path == reveal_path_)
        {
            reveal_path_.clear();
            rebuild_visible();
            for (int row = 0; row < rowCount(); ++row)
                if (visible_[static_cast<std::size_t>(row)].path == selected_path_)
                {
                    emit folderRevealed(row);
                    break;
                }
            return;
        }
        const auto prefix = node.path.endsWith('/') ? node.path : node.path + '/';
        if (!reveal_path_.startsWith(prefix))
            continue;
        if (node.loaded)
        {
            node.collapsed = false;
            continue;
        }
        if (!node.listing_pending)
        {
            node.listing_generation = ++next_listing_generation_;
            node.listing_pending = true;
            emit directoryListingRequested(node.path, node.listing_generation);
        }
        rebuild_visible();
        return;
    }
    rebuild_visible();
}

void FilesystemBrowserModel::rebuild_visible()
{
    beginResetModel();
    visible_.clear();
    visible_.reserve(all_nodes_.size());
    for (std::size_t index = 0; index < all_nodes_.size(); ++index)
    {
        if (!hidden_by_collapse(index))
            visible_.push_back(all_nodes_[index]);
    }
    endResetModel();
}

bool FilesystemBrowserModel::hidden_by_collapse(const std::size_t index) const
{
    int depth = all_nodes_[index].depth;
    for (std::size_t cursor = index; cursor > 0;)
    {
        --cursor;
        if (all_nodes_[cursor].depth < depth)
        {
            if (all_nodes_[cursor].collapsed)
                return true;
            depth = all_nodes_[cursor].depth;
        }
    }
    return false;
}

int FilesystemBrowserModel::index_of_path(const QString &path) const
{
    for (std::size_t index = 0; index < all_nodes_.size(); ++index)
    {
        if (all_nodes_[index].path == path)
            return static_cast<int>(index);
    }
    return -1;
}

} // namespace ravo
