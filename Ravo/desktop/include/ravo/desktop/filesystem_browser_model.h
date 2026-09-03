#pragma once

#include <cstdint>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>

#include "ravo/foundation/error.h"

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
[[nodiscard]] std::vector<FilesystemFolderEntry> mounted_filesystem_roots();

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
    };

    explicit FilesystemBrowserModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] QString selectedPath() const;
    void resetWithRoots(std::vector<FilesystemFolderEntry> roots);
    void loadMountedVolumes();
    void applyChildren(const QString &path, quint64 generation,
                       Result<std::vector<FilesystemFolderEntry>> children);
    Q_INVOKABLE void toggleCollapsed(const QString &path);
    Q_INVOKABLE void selectFolder(const QString &path);

signals:
    void selectedPathChanged();
    void directoryListingRequested(const QString &path, quint64 generation);

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
        QString error;
    };

    void rebuild_visible();
    [[nodiscard]] bool hidden_by_collapse(std::size_t index) const;
    [[nodiscard]] int index_of_path(const QString &path) const;

    std::vector<Node> all_nodes_;
    std::vector<Node> visible_;
    QString selected_path_;
};

} // namespace ravo
