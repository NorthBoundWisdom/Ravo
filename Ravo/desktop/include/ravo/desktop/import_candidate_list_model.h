#pragma once

#include <vector>

#include <QAbstractListModel>
#include <QImage>
#include <QStringList>

#include "ravo/domain/types.h"

namespace ravo
{

class ImportCandidateListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    enum Role
    {
        SourcePathRole = Qt::UserRole + 1,
        DisplayNameRole,
        MediaTypeRole,
        WidthRole,
        HeightRole,
        SizeBytesRole,
        SelectedRole,
        EligibleRole,
        DuplicateRole,
        ThumbnailUrlRole,
        ErrorRole,
    };

    explicit ImportCandidateListModel(QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int selectedCount() const noexcept;
    void setCandidates(std::vector<ImportCandidate> candidates);
    void setThumbnail(int row, QImage image);
    [[nodiscard]] QImage thumbnail(int row) const;
    [[nodiscard]] QString sourcePath(int row) const;
    [[nodiscard]] QStringList selectedPaths() const;
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int first, int last, bool additive);
    Q_INVOKABLE void setAllSelected(bool selected);

signals:
    void selectionChanged();

private:
    struct Row
    {
        ImportCandidate candidate;
        QImage thumbnail;
        bool selected = true;
        std::uint64_t thumbnail_revision = 0U;
    };
    std::vector<Row> rows_;
};

} // namespace ravo
