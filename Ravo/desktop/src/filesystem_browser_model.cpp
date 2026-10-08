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

} // namespace

bool import_source_recursion(const QString &source, const QString &user_directory,
                             const bool requested)
{
    if (!requested)
        return false;
    if (generic_path(source) == generic_path(user_directory))
        return false;
    const auto canonical_source = QFileInfo(source).canonicalFilePath();
    const auto canonical_home = QFileInfo(user_directory).canonicalFilePath();
    return canonical_source.isEmpty() || canonical_home.isEmpty() ||
           canonical_source != canonical_home;
}

std::vector<FilesystemFolderEntry> list_mounted_filesystem_roots()
{
    std::vector<FilesystemFolderEntry> roots;
    for (const auto &storage : QStorageInfo::mountedVolumes())
    {
        if (!storage.isValid() || !storage.isReady())
            continue;
        const auto path = generic_path(storage.rootPath());
#ifdef Q_OS_MACOS
        // APFS support mounts are not photo sources. Finder-visible disks,
        // including memory cards, are mounted under /Volumes.
        if (path != QLatin1String("/") && !path.startsWith(QLatin1String("/Volumes/")))
            continue;
#endif
        const QFileInfo folder(path);
        if (!folder.isDir() || !folder.isReadable())
            continue;
        if (std::any_of(roots.begin(), roots.end(),
                        [&](const auto &root) { return root.path == path; }))
            continue;
        const auto name = storage.displayName();
        roots.push_back(
            {path, name.isEmpty() || name == path ? path : name + " (" + path + ")", true});
    }
    std::sort(roots.begin(), roots.end(),
              [](const auto &left, const auto &right) { return left.path < right.path; });
    return roots;
}

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
    case ListingPendingRole:
        return row.listing_pending;
    case WillCreateRole:
        return row.will_create;
    case PlannedPhotoCountRole:
        return static_cast<qulonglong>(row.planned_photo_count);
    default:
        return {};
    }
}

QHash<int, QByteArray> FilesystemBrowserModel::roleNames() const
{
    return {{PathRole, "path"},
            {DisplayNameRole, "displayName"},
            {DepthRole, "depth"},
            {HasChildrenRole, "hasChildren"},
            {CollapsedRole, "collapsed"},
            {SelectedRole, "selected"},
            {ErrorRole, "errorText"},
            {ListingPendingRole, "listingPending"},
            {WillCreateRole, "willCreate"},
            {PlannedPhotoCountRole, "plannedPhotoCount"}};
}

QString FilesystemBrowserModel::selectedPath() const
{
    return selected_path_;
}

void FilesystemBrowserModel::resetWithRoots(std::vector<FilesystemFolderEntry> roots)
{
    beginResetModel();
    all_nodes_.clear();
    mounted_roots_.clear();
    reveal_path_.clear();
    preview_reveal_path_.clear();
    preview_folders_.clear();
    preview_branches_.clear();
    collapsed_preview_branches_.clear();
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

void FilesystemBrowserModel::loadUserDirectory()
{
    const auto path = generic_path(QDir::homePath());
    resetWithRoots({{path, path, true}});
    toggleCollapsed(path);
}

void FilesystemBrowserModel::updateMountedRoots(std::vector<FilesystemFolderEntry> roots)
{
    const auto previous_mounted = mounted_roots_;
    bool changed = false;
    QStringList paths;
    for (auto &root : roots)
        paths.push_back(generic_path(root.path));
    for (std::size_t index = 0; index < all_nodes_.size();)
    {
        const auto &node = all_nodes_[index];
        if (node.depth != 0 || !mounted_roots_.contains(node.path) || paths.contains(node.path))
        {
            ++index;
            continue;
        }
        auto end = index + 1;
        while (end < all_nodes_.size() && all_nodes_[end].depth > 0)
            ++end;
        all_nodes_.erase(all_nodes_.begin() + static_cast<std::ptrdiff_t>(index),
                         all_nodes_.begin() + static_cast<std::ptrdiff_t>(end));
        changed = true;
    }
    mounted_roots_.clear();
    for (auto &root : roots)
    {
        const auto path = generic_path(root.path);
        if (index_of_path(path) >= 0)
        {
            // Home or a picker root remains owned by its original entry.
            if (previous_mounted.contains(path))
                mounted_roots_.push_back(path);
            continue;
        }
        Node node;
        node.path = path;
        node.display_name = root.display_name;
        node.has_children = root.has_children;
        all_nodes_.push_back(std::move(node));
        changed = true;
        mounted_roots_.push_back(path);
    }
    if (changed)
        rebuild_visible();
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
        rebuild_visible();
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
        const auto child_index = index_of_path(generic_path(child.path));
        // Home and mounted/picker roots can overlap. Each path has one node
        // identity; do not recreate another root inside an expanded subtree.
        if (child_index >= 0 && (static_cast<std::size_t>(child_index) < remove_from ||
                                 static_cast<std::size_t>(child_index) >= remove_to))
            continue;
        Node node;
        node.path = generic_path(child.path);
        node.display_name = child.display_name;
        node.depth = parent_depth + 1;
        node.has_children = child.has_children;
        node.collapsed = true;
        node.loaded = false;
        next.push_back(std::move(node));
    }
    next[static_cast<std::size_t>(parent_index)].has_children = next.size() > remove_from;
    next.insert(next.end(), all_nodes_.begin() + static_cast<std::ptrdiff_t>(remove_to),
                all_nodes_.end());
    all_nodes_ = std::move(next);
    rebuild_visible();
    if (!reveal_path_.isEmpty())
        revealFolder(reveal_path_);
    request_preview_listings();
}

void FilesystemBrowserModel::setPreviewFolders(std::vector<ImportDestinationFolder> folders,
                                               const QString &destination)
{
    if (folders.empty() && preview_folders_.empty())
        return;
    // The service canonicalizes filesystem aliases. Keep the plan anchored to
    // the explicit destination spelling used by the browser, without disk I/O.
    if (!destination.isEmpty())
    {
        const auto root =
            std::find_if(folders.begin(), folders.end(), [](const ImportDestinationFolder &folder)
                         { return folder.depth == 0 && !folder.second_copy; });
        if (root != folders.end())
        {
            const auto canonical = generic_path(qstring_from_utf8(root->path));
            const auto prefix = canonical.endsWith('/') ? canonical : canonical + '/';
            const auto chosen = generic_path(destination);
            for (auto &folder : folders)
            {
                const auto path = generic_path(qstring_from_utf8(folder.path));
                if (!folder.second_copy && (path == canonical || path.startsWith(prefix)))
                    folder.path = utf8_from_qstring(
                        path == canonical ?
                            chosen :
                            generic_path(QDir(chosen).filePath(path.mid(prefix.size()))));
            }
        }
    }
    preview_folders_ = std::move(folders);
    preview_reveal_path_.clear();
    // Reveal the first planned branch's leaf, not just the selected destination.
    // The root may still be awaiting an ancestor listing; retain the intent until
    // the overlay becomes visible. This never selects a virtual directory.
    auto branch = std::find_if(preview_folders_.begin(), preview_folders_.end(),
                               [](const ImportDestinationFolder &folder)
                               { return !folder.second_copy && folder.will_create; });
    if (branch == preview_folders_.end())
        branch = preview_folders_.begin();
    std::size_t preview_depth = 0;
    for (; branch != preview_folders_.end(); ++branch)
    {
        const auto &folder = *branch;
        if (folder.second_copy)
            continue;
        if (!preview_reveal_path_.isEmpty() && folder.depth <= preview_depth)
            break;
        preview_reveal_path_ = generic_path(qstring_from_utf8(folder.path));
        preview_depth = folder.depth;
    }
    const auto collapsed = collapsed_preview_branches_;
    for (const auto &path : collapsed)
        if (preview_reveal_path_.startsWith(path.endsWith('/') ? path : path + '/'))
            collapsed_preview_branches_.remove(path);
    if (preview_folders_.empty())
        collapsed_preview_branches_.clear();
    if (!destination.isEmpty() && !preview_folders_.empty())
        revealFolder(destination);
    rebuild_visible();
    request_preview_listings();
}

void FilesystemBrowserModel::toggleCollapsed(const QString &path)
{
    const auto normalized = generic_path(path);
    if (preview_branches_.contains(normalized))
    {
        if (!collapsed_preview_branches_.remove(normalized))
            collapsed_preview_branches_.insert(normalized);
        rebuild_visible();
        request_preview_listings();
        return;
    }
    const auto node_index = index_of_path(normalized);
    if (node_index < 0)
        return;
    auto &node = all_nodes_[static_cast<std::size_t>(node_index)];
    if (node.listing_pending)
        return;
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
    rebuild_visible();
    emit directoryListingRequested(node.path, node.listing_generation);
}

void FilesystemBrowserModel::activateFolder(const QString &path)
{
    // A planned folder is not a destination until the import creates it.
    const auto normalized = generic_path(path);
    for (const auto &folder : preview_folders_)
        if (!folder.second_copy && folder.will_create &&
            generic_path(qstring_from_utf8(folder.path)) == normalized &&
            index_of_path(normalized) < 0)
            return;
    selectFolder(path);
    if (preview_branches_.contains(generic_path(path)))
    {
        if (collapsed_preview_branches_.contains(generic_path(path)))
            toggleCollapsed(path);
        return;
    }
    const auto node_index = index_of_path(generic_path(path));
    if (node_index >= 0 && all_nodes_[static_cast<std::size_t>(node_index)].collapsed)
        toggleCollapsed(path);
}

void FilesystemBrowserModel::selectFolder(const QString &path)
{
    const auto next = generic_path(path);
    for (const auto &folder : preview_folders_)
        if (!folder.second_copy && folder.will_create &&
            generic_path(qstring_from_utf8(folder.path)) == next && index_of_path(next) < 0)
            return;
    if (next != reveal_path_)
        reveal_path_.clear();
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
    selectFolder(path);
    reveal_path_ = generic_path(path);
    const bool within_roots =
        std::any_of(all_nodes_.begin(), all_nodes_.end(),
                    [&](const Node &node)
                    {
                        const auto prefix = node.path.endsWith('/') ? node.path : node.path + '/';
                        return node.depth == 0 &&
                               (reveal_path_ == node.path || reveal_path_.startsWith(prefix));
                    });
    if (!within_roots)
    {
        // Explicit picker selections outside Home remain reachable without a disk-root tree.
        Node node;
        node.path = reveal_path_;
        node.display_name = reveal_path_;
        all_nodes_.push_back(std::move(node));
    }
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
    // Compose disposable rows rather than inserting virtual directories into
    // all_nodes_: late listings can never erase or materialize the service plan.
    auto merged = all_nodes_;
    preview_branches_.clear();
    for (const auto &folder : preview_folders_)
    {
        if (folder.second_copy)
            continue;
        const auto path = generic_path(qstring_from_utf8(folder.path));
        const auto existing = std::find_if(merged.begin(), merged.end(),
                                           [&](const Node &node) { return node.path == path; });
        if (existing != merged.end())
            existing->planned_photo_count = folder.photo_count;
        if (folder.depth == 0)
            continue;
        const auto parent_path = generic_path(QDir(path).filePath(QStringLiteral("..")));
        const auto parent = std::find_if(merged.begin(), merged.end(), [&](const Node &node)
                                         { return node.path == parent_path; });
        if (parent == merged.end())
            continue; // The selected root's asynchronous reveal may still be pending.
        const int depth = parent->depth + 1;
        parent->has_children = true;
        preview_branches_.insert(parent_path);
        if (std::any_of(merged.begin(), merged.end(),
                        [&](const Node &node) { return node.path == path; }))
            continue;
        Node node;
        node.path = path;
        node.display_name = qstring_from_utf8(folder.name);
        node.depth = depth;
        node.has_children = false;
        node.collapsed = false;
        node.loaded = true;
        node.will_create = folder.will_create;
        node.planned_photo_count = folder.photo_count;
        auto position = parent + 1;
        while (position != merged.end() && position->depth >= depth)
        {
            if (position->depth == depth &&
                QString::localeAwareCompare(node.display_name, position->display_name) < 0)
                break;
            ++position;
        }
        merged.insert(position, std::move(node));
    }
    for (auto &node : merged)
        if (preview_branches_.contains(node.path) && node.error.isEmpty())
            node.collapsed = collapsed_preview_branches_.contains(node.path);

    beginResetModel();
    visible_.clear();
    visible_.reserve(merged.size());
    int collapsed_depth = -1;
    for (const auto &node : merged)
    {
        if (collapsed_depth >= 0 && node.depth > collapsed_depth)
            continue;
        visible_.push_back(node);
        collapsed_depth = node.collapsed ? node.depth : -1;
    }
    endResetModel();
}

void FilesystemBrowserModel::request_preview_listings()
{
    std::vector<std::pair<QString, quint64>> requests;
    for (auto &node : all_nodes_)
    {
        if (!preview_branches_.contains(node.path) ||
            collapsed_preview_branches_.contains(node.path) || node.loaded ||
            node.listing_pending || !node.error.isEmpty())
            continue;
        node.listing_pending = true;
        node.listing_generation = ++next_listing_generation_;
        requests.emplace_back(node.path, node.listing_generation);
    }
    if (!requests.empty())
        rebuild_visible();
    for (const auto &[path, generation] : requests)
        emit directoryListingRequested(path, generation);
    // Late real-directory listings reset the rows again. Consume the one-shot
    // scroll intent only after the destination and all planned branches settle.
    if (!reveal_path_.isEmpty() ||
        std::any_of(all_nodes_.begin(), all_nodes_.end(), [&](const Node &node)
                    { return preview_branches_.contains(node.path) && node.listing_pending; }))
        return;
    if (!preview_reveal_path_.isEmpty())
        for (int row = 0; row < rowCount(); ++row)
            if (visible_[static_cast<std::size_t>(row)].path == preview_reveal_path_)
            {
                preview_reveal_path_.clear();
                emit folderRevealed(row);
                break;
            }
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
