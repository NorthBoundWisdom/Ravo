#pragma once

#include <vector>
#include <deque>
#include <set>

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
    Q_PROPERTY(quint64 selectionRevision READ selectionRevision NOTIFY selectionChanged)

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
    void setCandidates(std::vector<ImportCandidate> candidates, bool preserve_check_intent = false);
    void appendCandidate(ImportCandidate candidate);
    [[nodiscard]] qulonglong selectedBytes() const noexcept;
    [[nodiscard]] quint64 selectionRevision() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> selectedContentHashes() const;
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    void updateCandidate(int row, ImportCandidate candidate);
    void applyScanBatch(int first, std::vector<ImportCandidate> candidates);
    void finishThumbnail(int row, QImage image, std::optional<TaskError> error = {});
    void setThumbnail(int row, QImage image);
    [[nodiscard]] QImage thumbnail(int row) const;
    [[nodiscard]] QString sourcePath(int row) const;
    [[nodiscard]] bool inspected(int row) const;
    [[nodiscard]] QStringList selectedPaths() const;
    [[nodiscard]] quint64 selectedPathsCallCount() const noexcept
    {
        return selected_paths_calls_;
    }
    void resetSelectedPathsCallCount() noexcept
    {
        selected_paths_calls_ = 0;
    }
    // Test-only probe: counts row membership examinations/updates on selection paths.
    [[nodiscard]] quint64 selectionRowTouchCount() const noexcept
    {
        return selection_row_touches_;
    }
    void resetSelectionRowTouchCount() noexcept
    {
        selection_row_touches_ = 0;
    }
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
        std::optional<TaskError> thumbnail_error;
    };
    void recountSelection();
    [[nodiscard]] static bool eligible(const Row &row) noexcept;
    [[nodiscard]] bool setRowSelected(Row &row, bool selected);
    void setRowHighlighted(int row, bool highlighted, std::vector<int> *changed);
    void emitRoleRanges(const std::vector<int> &rows, const QList<int> &roles);
    void notifySelectionIfChanged(int previous_count, qulonglong previous_bytes,
                                  bool membership_changed);
    std::vector<Row> rows_;
    std::deque<int> thumbnail_rows_;
    std::set<int> highlighted_rows_;
    bool select_new_candidates_ = true;
    std::uint64_t generation_ = 0;
    int selected_count_ = 0;
    qulonglong selected_bytes_ = 0;
    quint64 selection_revision_ = 0;
    qulonglong thumbnail_bytes_ = 0;
    mutable quint64 selected_paths_calls_ = 0;
    mutable quint64 selection_row_touches_ = 0;
    static constexpr std::size_t maximum_cached_thumbnails = 256;
    static constexpr qulonglong maximum_cached_thumbnail_bytes = 64ULL * 1024ULL * 1024ULL;
};

} // namespace ravo
