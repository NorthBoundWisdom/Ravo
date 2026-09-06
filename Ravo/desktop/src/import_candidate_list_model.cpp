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
    case HighlightedRole:
        return row.highlighted;
    case EligibleRole:
        return row.candidate.supported && !row.candidate.duplicate;
    case DuplicateRole:
        return row.candidate.duplicate;
    case ThumbnailUrlRole:
        return row.thumbnail.isNull() ? QUrl{} :
                                        QUrl(QStringLiteral("image://importCandidate/%1?r=%2&g=%3")
                                                 .arg(index.row())
                                                 .arg(row.thumbnail_revision)
                                                 .arg(generation_));
    case ErrorRole:
        return row.candidate.error ? qstring_from_utf8(row.candidate.error->message) :
               row.thumbnail_error ? qstring_from_utf8(row.thumbnail_error->message) :
                                     QString{};
    case InspectedRole:
        return row.inspected;
    default:
        return {};
    }
}

QHash<int, QByteArray> ImportCandidateListModel::roleNames() const
{
    return {{SourcePathRole, "sourcePath"},     {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},       {WidthRole, "pixelWidth"},
            {HeightRole, "pixelHeight"},        {SizeBytesRole, "sizeBytes"},
            {SelectedRole, "selected"},         {HighlightedRole, "highlighted"},
            {EligibleRole, "eligible"},         {DuplicateRole, "duplicate"},
            {ThumbnailUrlRole, "thumbnailUrl"}, {ErrorRole, "errorText"},
            {InspectedRole, "inspected"}};
}

int ImportCandidateListModel::selectedCount() const noexcept
{
    return selected_count_;
}

void ImportCandidateListModel::recountSelection()
{
    selected_count_ = 0;
    selected_bytes_ = 0;
    for (const auto &row : rows_)
        if (row.selected)
        {
            ++selected_count_;
            selected_bytes_ += row.candidate.size_bytes;
        }
}

void ImportCandidateListModel::setCandidates(std::vector<ImportCandidate> candidates,
                                             const bool preserve_check_intent)
{
    beginResetModel();
    ++generation_;
    if (!preserve_check_intent)
        select_new_candidates_ = true;
    rows_.clear();
    thumbnail_rows_.clear();
    rows_.reserve(candidates.size());
    for (auto &candidate : candidates)
        rows_.push_back({std::move(candidate), {}, false, false, false, 0U, {}});
    for (auto &row : rows_)
        row.selected =
            select_new_candidates_ && row.candidate.supported && !row.candidate.duplicate;
    recountSelection();
    endResetModel();
    emit selectionChanged();
    emit candidatesChanged();
}

void ImportCandidateListModel::appendCandidate(ImportCandidate candidate)
{
    const int row = rowCount();
    beginInsertRows({}, row, row);
    const bool selected = select_new_candidates_ && candidate.supported && !candidate.duplicate;
    rows_.push_back({std::move(candidate), {}, selected, false, false, 0U, {}});
    if (selected)
    {
        ++selected_count_;
        selected_bytes_ += rows_.back().candidate.size_bytes;
    }
    endInsertRows();
    emit candidatesChanged();
    emit selectionChanged();
}

qulonglong ImportCandidateListModel::selectedBytes() const noexcept
{
    return selected_bytes_;
}

std::vector<std::pair<std::string, std::string>>
ImportCandidateListModel::selectedContentHashes() const
{
    std::vector<std::pair<std::string, std::string>> hashes;
    for (const auto &row : rows_)
        if (row.selected)
            hashes.emplace_back(row.candidate.source_path, row.candidate.content_sha256);
    return hashes;
}

void ImportCandidateListModel::updateCandidate(const int row, ImportCandidate candidate)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (entry.candidate.source_path != candidate.source_path)
        return;
    const bool was_selected = entry.selected;
    if (candidate.display_name.empty())
        candidate.display_name = entry.candidate.display_name;
    if (candidate.relative_path.empty())
        candidate.relative_path = entry.candidate.relative_path;
    candidate.content_sha256 = entry.candidate.content_sha256;
    // The scan owns content/batch duplicate classification. Thumbnail inspection
    // checks a single path and cannot revoke that classification in this generation.
    if (entry.candidate.duplicate)
    {
        candidate.duplicate = true;
        candidate.duplicate_reason = entry.candidate.duplicate_reason;
        candidate.duplicate_asset_id = entry.candidate.duplicate_asset_id;
    }
    entry.candidate = std::move(candidate);
    entry.inspected = true;
    if (!entry.candidate.supported || entry.candidate.duplicate)
    {
        entry.selected = false;
        entry.highlighted = false;
    }
    recountSelection();
    emit dataChanged(index(row, 0), index(row, 0),
                     {SourcePathRole, MediaTypeRole, WidthRole, HeightRole, SizeBytesRole,
                      SelectedRole, HighlightedRole, EligibleRole, DuplicateRole, ErrorRole,
                      InspectedRole, DisplayNameRole});
    if (was_selected != entry.selected)
        emit selectionChanged();
}

void ImportCandidateListModel::setThumbnail(const int row, QImage image)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    entry.thumbnail = std::move(image);
    std::erase(thumbnail_rows_, row);
    if (!entry.thumbnail.isNull())
        thumbnail_rows_.push_back(row);
    // Metadata may describe 100,000 candidates; owned thumbnail pixels may not.
    constexpr std::size_t maximum_cached_thumbnails = 256;
    while (thumbnail_rows_.size() > maximum_cached_thumbnails)
    {
        const int evicted = thumbnail_rows_.front();
        thumbnail_rows_.pop_front();
        auto &old = rows_[static_cast<std::size_t>(evicted)];
        old.thumbnail = {};
        old.inspected = false;
        emit dataChanged(index(evicted, 0), index(evicted, 0), {ThumbnailUrlRole, InspectedRole});
    }
    ++entry.thumbnail_revision;
    emit dataChanged(index(row, 0), index(row, 0), {ThumbnailUrlRole});
}

void ImportCandidateListModel::applyScanBatch(const int first,
                                              std::vector<ImportCandidate> candidates)
{
    if (first < 0 || first + static_cast<int>(candidates.size()) > rowCount() || candidates.empty())
        return;
    // Classification and thumbnail completion are independent. Preserve both
    // the user's check/highlight intent and pixels already delivered by decode.
    for (std::size_t offset = 0; offset < candidates.size(); ++offset)
    {
        auto &entry = rows_[static_cast<std::size_t>(first) + offset];
        if (entry.selected)
        {
            --selected_count_;
            selected_bytes_ -= entry.candidate.size_bytes;
        }
        entry.candidate = std::move(candidates[offset]);
        if (!entry.candidate.supported || entry.candidate.duplicate)
        {
            entry.selected = false;
            entry.highlighted = false;
        }
        if (entry.selected)
        {
            ++selected_count_;
            selected_bytes_ += entry.candidate.size_bytes;
        }
    }
    emit dataChanged(index(first, 0), index(first + static_cast<int>(candidates.size()) - 1, 0),
                     {SourcePathRole, MediaTypeRole, WidthRole, HeightRole, SizeBytesRole,
                      SelectedRole, HighlightedRole, EligibleRole, DuplicateRole, ErrorRole,
                      DisplayNameRole});
    emit selectionChanged();
}

void ImportCandidateListModel::finishThumbnail(const int row, QImage image,
                                               std::optional<TaskError> error)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    entry.inspected = true;
    entry.thumbnail_error = std::move(error);
    setThumbnail(row, std::move(image));
    emit dataChanged(index(row, 0), index(row, 0), {InspectedRole, ErrorRole});
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

bool ImportCandidateListModel::inspected(const int row) const
{
    return row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].inspected;
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
    selected_count_ += entry.selected ? 1 : -1;
    if (entry.selected)
        selected_bytes_ += entry.candidate.size_bytes;
    else
        selected_bytes_ -= entry.candidate.size_bytes;
    emit dataChanged(index(row, 0), index(row, 0), {SelectedRole});
    emit selectionChanged();
}

void ImportCandidateListModel::setAllSelected(const bool selected)
{
    select_new_candidates_ = selected;
    if (rows_.empty())
        return;
    for (auto &row : rows_)
        row.selected = selected && row.candidate.supported && !row.candidate.duplicate;
    recountSelection();
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
    recountSelection();
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    emit selectionChanged();
}

void ImportCandidateListModel::highlightExclusive(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    if (!rows_[static_cast<std::size_t>(row)].candidate.supported ||
        rows_[static_cast<std::size_t>(row)].candidate.duplicate)
        return;
    for (int current = 0; current < rowCount(); ++current)
        rows_[static_cast<std::size_t>(current)].highlighted = current == row;
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {HighlightedRole});
}

void ImportCandidateListModel::highlightToggle(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (!entry.candidate.supported || entry.candidate.duplicate)
        return;
    entry.highlighted = !entry.highlighted;
    emit dataChanged(index(row, 0), index(row, 0), {HighlightedRole});
}

void ImportCandidateListModel::highlightRange(int first, int last, const bool additive)
{
    if (rows_.empty())
        return;
    first = std::clamp(first, 0, rowCount() - 1);
    last = std::clamp(last, 0, rowCount() - 1);
    if (first > last)
        std::swap(first, last);
    if (!additive)
        for (auto &row : rows_)
            row.highlighted = false;
    for (int row = first; row <= last; ++row)
    {
        auto &entry = rows_[static_cast<std::size_t>(row)];
        if (entry.candidate.supported && !entry.candidate.duplicate)
            entry.highlighted = true;
    }
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {HighlightedRole});
}

void ImportCandidateListModel::highlightAll()
{
    if (rows_.empty())
        return;
    for (auto &row : rows_)
        row.highlighted = row.candidate.supported && !row.candidate.duplicate;
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {HighlightedRole});
}

void ImportCandidateListModel::applyCheck(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &clicked = rows_[static_cast<std::size_t>(row)];
    if (!clicked.candidate.supported || clicked.candidate.duplicate)
        return;
    const bool next = !clicked.selected;
    if (clicked.highlighted)
    {
        for (auto &entry : rows_)
            if (entry.highlighted && entry.candidate.supported && !entry.candidate.duplicate)
                entry.selected = next;
        recountSelection();
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {SelectedRole});
    }
    else
    {
        clicked.selected = next;
        recountSelection();
        emit dataChanged(index(row, 0), index(row, 0), {SelectedRole});
    }
    emit selectionChanged();
}

bool ImportCandidateListModel::highlighted(const int row) const
{
    return row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].highlighted;
}

} // namespace ravo
