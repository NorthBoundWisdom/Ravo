#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>

#include "ravo/domain/types.h"

namespace ravo
{

class FolderListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        FolderUriRole = Qt::UserRole + 1,
        DisplayNameRole,
        DepthRole,
        AssetCountRole,
        HasChildrenRole,
        HasNextSiblingRole,
        AncestorLineContinuesRole,
        CollapsedRole,
    };

    explicit FolderListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setFolders(std::vector<FolderRecord> folders);
    Q_INVOKABLE void toggleCollapsed(const QString &uri);

private:
    struct FolderRow
    {
        FolderRecord folder;
        bool has_children = false;
        bool has_next_sibling = false;
        std::vector<char> ancestor_line_continues;
    };

    void rebuild_visible();
    void decorate_visible_guides();
    [[nodiscard]] bool hidden_by_collapse(std::size_t index) const;

    std::vector<FolderRow> all_folders_;
    std::vector<FolderRow> folders_;
    std::unordered_set<std::string> collapsed_;
};

} // namespace ravo
