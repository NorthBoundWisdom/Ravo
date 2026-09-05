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
    Q_PROPERTY(int candidateCount READ rowCount NOTIFY candidatesChanged)
    Q_PROPERTY(qulonglong selectedBytes READ selectedBytes NOTIFY selectionChanged)

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
        HighlightedRole,
        EligibleRole,
        DuplicateRole,
        ThumbnailUrlRole,
        ErrorRole,
        InspectedRole,
    };

    explicit ImportCandidateListModel(QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int selectedCount() const noexcept;
    void setCandidates(std::vector<ImportCandidate> candidates);
    void appendCandidate(ImportCandidate candidate);
    [[nodiscard]] qulonglong selectedBytes() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> selectedContentHashes() const;
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    void updateCandidate(int row, ImportCandidate candidate);
    void setThumbnail(int row, QImage image);
    [[nodiscard]] QImage thumbnail(int row) const;
    [[nodiscard]] QString sourcePath(int row) const;
    [[nodiscard]] bool inspected(int row) const;
    [[nodiscard]] QStringList selectedPaths() const;
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int first, int last, bool additive);
    Q_INVOKABLE void setAllSelected(bool selected);
    Q_INVOKABLE void highlightExclusive(int row);
    Q_INVOKABLE void highlightToggle(int row);
    Q_INVOKABLE void highlightRange(int first, int last, bool additive);
    Q_INVOKABLE void highlightAll();
    Q_INVOKABLE void applyCheck(int row);
    [[nodiscard]] bool highlighted(int row) const;

signals:
    void selectionChanged();
    void candidatesChanged();

private:
    struct Row
    {
        ImportCandidate candidate;
        QImage thumbnail;
        bool selected = true;
        bool highlighted = false;
        bool inspected = false;
        std::uint64_t thumbnail_revision = 0U;
    };
    std::vector<Row> rows_;
    bool select_new_candidates_ = true;
    std::uint64_t generation_ = 0;
};

} // namespace ravo
