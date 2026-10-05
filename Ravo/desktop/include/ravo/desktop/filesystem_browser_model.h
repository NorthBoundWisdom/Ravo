#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "ravo/foundation/error.h"
#include "ravo/domain/types.h"

namespace ravo
{

struct FilesystemFolderEntry
{
    QString path;
    QString display_name;
    bool has_children = true;
};

[[nodiscard]] Result<std::vector<FilesystemFolderEntry>>
list_filesystem_folders(const QString &path);
// Call on the filesystem worker: querying mounted storage can block.
[[nodiscard]] std::vector<FilesystemFolderEntry> list_mounted_filesystem_roots();
// Import's Home-root guard also recognizes normalized and symlinked paths.
[[nodiscard]] bool import_source_recursion(const QString &source, const QString &user_directory,
                                           bool requested);

class FilesystemBrowserModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectedPathChanged)

public:
    enum Role
    {
        PathRole = Qt::UserRole + 1,
        DisplayNameRole,
        DepthRole,
        HasChildrenRole,
        CollapsedRole,
        SelectedRole,
        ErrorRole,
        ListingPendingRole,
        WillCreateRole,
    };

    explicit FilesystemBrowserModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] QString selectedPath() const;
    void resetWithRoots(std::vector<FilesystemFolderEntry> roots);
    void loadUserDirectory();
    void updateMountedRoots(std::vector<FilesystemFolderEntry> roots);
    void applyChildren(const QString &path, quint64 generation,
                       Result<std::vector<FilesystemFolderEntry>> children);
    // Read-only service plan overlay; filesystem listings retain their own identities.
    void setPreviewFolders(std::vector<ImportDestinationFolder> folders,
                           const QString &destination = {});
    Q_INVOKABLE void toggleCollapsed(const QString &path);
    Q_INVOKABLE void selectFolder(const QString &path);
    Q_INVOKABLE void activateFolder(const QString &path);
    Q_INVOKABLE void revealFolder(const QString &path);

signals:
    void selectedPathChanged();
    void directoryListingRequested(const QString &path, quint64 generation);
    void folderRevealed(int row);

private:
    struct Node
    {
        QString path;
        QString display_name;
        int depth = 0;
        bool has_children = true;
        bool collapsed = true;
        bool loaded = false;
        quint64 listing_generation = 0;
        bool listing_pending = false;
        QString error;
        bool will_create = false;
    };

    void rebuild_visible();
    void request_preview_listings();
    [[nodiscard]] int index_of_path(const QString &path) const;

    std::vector<Node> all_nodes_;
    std::vector<Node> visible_;
    std::vector<ImportDestinationFolder> preview_folders_;
    QSet<QString> preview_branches_;
    QSet<QString> collapsed_preview_branches_;
    QStringList mounted_roots_;
    QString selected_path_;
    QString reveal_path_;
    quint64 next_listing_generation_ = 0;
};

} // namespace ravo
