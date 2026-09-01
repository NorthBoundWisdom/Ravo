#pragma once

#include <vector>

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>

#include "ravo/domain/types.h"

namespace ravo
{

class LibrarySetListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        SetIdRole = Qt::UserRole + 1,
        KindRole,
        NameRole,
        AssetCountRole,
        SelectedRole,
    };

    explicit LibrarySetListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    void setSets(std::vector<LibrarySetRecord> sets, std::string selected_id);

private:
    std::vector<LibrarySetRecord> sets_;
    std::string selected_id_;
};

} // namespace ravo
