#include "ravo/desktop/import_candidate_list_model.h"

#include <algorithm>

#include <QUrl>

#include "studio_qt.h"

namespace ravo
{

ImportCandidateListModel::ImportCandidateListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ImportCandidateListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ImportCandidateListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto &row = rows_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case SourcePathRole:
        return qstring_from_utf8(row.candidate.source_path);
    case DisplayNameRole:
        return qstring_from_utf8(row.candidate.display_name);
    case MediaTypeRole:
        return qstring_from_utf8(row.candidate.media_type);
    case WidthRole:
        return static_cast<int>(row.candidate.width.value_or(0U));
    case HeightRole:
        return static_cast<int>(row.candidate.height.value_or(0U));
    case SizeBytesRole:
        return static_cast<qulonglong>(row.candidate.size_bytes);
    case SelectedRole:
        return row.selected;
    case EligibleRole:
        return row.candidate.supported && !row.candidate.duplicate;
    case DuplicateRole:
        return row.candidate.duplicate;
    case ThumbnailUrlRole:
        return row.thumbnail.isNull() ? QUrl{} :
                                        QUrl(QStringLiteral("image://importCandidate/%1?r=%2")
                                                 .arg(index.row())
                                                 .arg(row.thumbnail_revision));
    case ErrorRole:
        return row.candidate.error ? qstring_from_utf8(row.candidate.error->message) : QString{};
    default:
        return {};
    }
}

QHash<int, QByteArray> ImportCandidateListModel::roleNames() const
{
    return {{SourcePathRole, "sourcePath"}, {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},   {WidthRole, "pixelWidth"},
            {HeightRole, "pixelHeight"},    {SizeBytesRole, "sizeBytes"},
            {SelectedRole, "selected"},     {EligibleRole, "eligible"},
            {DuplicateRole, "duplicate"},   {ThumbnailUrlRole, "thumbnailUrl"},
            {ErrorRole, "errorText"}};
}

int ImportCandidateListModel::selectedCount() const noexcept
{
    return static_cast<int>(
        std::count_if(rows_.begin(), rows_.end(), [](const Row &row) { return row.selected; }));
}

void ImportCandidateListModel::setCandidates(std::vector<ImportCandidate> candidates)
{
    beginResetModel();
    rows_.clear();
    rows_.reserve(candidates.size());
    for (auto &candidate : candidates)
        rows_.push_back({std::move(candidate), {}, false, 0U});
    for (auto &row : rows_)
        row.selected = row.candidate.supported && !row.candidate.duplicate;
    endResetModel();
    emit selectionChanged();
}

void ImportCandidateListModel::setThumbnail(const int row, QImage image)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    entry.thumbnail = std::move(image);
    ++entry.thumbnail_revision;
    emit dataChanged(index(row, 0), index(row, 0), {ThumbnailUrlRole});
}

QImage ImportCandidateListModel::thumbnail(const int row) const
{
    return row < 0 || row >= rowCount() ? QImage{} : rows_[static_cast<std::size_t>(row)].thumbnail;
}

QString ImportCandidateListModel::sourcePath(const int row) const
{
    return row < 0 || row >= rowCount() ?
               QString{} :
               qstring_from_utf8(rows_[static_cast<std::size_t>(row)].candidate.source_path);
}

QStringList ImportCandidateListModel::selectedPaths() const
{
    QStringList result;
    for (const auto &row : rows_)
        if (row.selected)
            result.push_back(qstring_from_utf8(row.candidate.source_path));
    return result;
}

void ImportCandidateListModel::toggleSelected(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (!entry.candidate.supported || entry.candidate.duplicate)
        return;
    entry.selected = !entry.selected;
    emit dataChanged(index(row, 0), index(row, 0), {SelectedRole});
    emit selectionChanged();
}

void ImportCandidateListModel::setAllSelected(const bool selected)
{
    if (rows_.empty())
        return;
    for (auto &row : rows_)
        row.selected = selected && row.candidate.supported && !row.candidate.duplicate;
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    emit selectionChanged();
}

void ImportCandidateListModel::selectRange(int first, int last, const bool additive)
{
    if (rows_.empty())
        return;
    first = std::clamp(first, 0, rowCount() - 1);
    last = std::clamp(last, 0, rowCount() - 1);
    if (first > last)
        std::swap(first, last);
    if (!additive)
        for (auto &row : rows_)
            row.selected = false;
    for (int row = first; row <= last; ++row)
    {
        auto &entry = rows_[static_cast<std::size_t>(row)];
        if (entry.candidate.supported && !entry.candidate.duplicate)
            entry.selected = true;
    }
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    emit selectionChanged();
}

} // namespace ravo
