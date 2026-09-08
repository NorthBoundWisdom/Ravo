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
    case ThumbnailLoadingRole:
        return row.thumbnail_loading;
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
            {InspectedRole, "inspected"},       {ThumbnailLoadingRole, "thumbnailLoading"}};
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

bool ImportCandidateListModel::eligible(const Row &row) noexcept
{
    return row.candidate.supported && !row.candidate.duplicate;
}

bool ImportCandidateListModel::setRowSelected(Row &row, const bool selected)
{
    ++selection_row_touches_;
    if (row.selected == selected)
        return false;
    if (selected && !eligible(row))
        return false;
    row.selected = selected;
    selected_count_ += selected ? 1 : -1;
    if (selected)
        selected_bytes_ += row.candidate.size_bytes;
    else
        selected_bytes_ -= row.candidate.size_bytes;
    return true;
}

void ImportCandidateListModel::setRowHighlighted(const int row, const bool highlighted,
                                                 std::vector<int> *changed)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (highlighted && !eligible(entry))
        return;
    if (entry.highlighted == highlighted)
        return;
    entry.highlighted = highlighted;
    if (highlighted)
        highlighted_rows_.insert(row);
    else
        highlighted_rows_.erase(row);
    if (changed)
        changed->push_back(row);
}

void ImportCandidateListModel::emitRoleRanges(const std::vector<int> &rows, const QList<int> &roles)
{
    if (rows.empty())
        return;
    std::vector<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    int begin = sorted.front();
    int prev = begin;
    for (std::size_t i = 1; i < sorted.size(); ++i)
    {
        if (sorted[i] == prev + 1)
        {
            prev = sorted[i];
            continue;
        }
        emit dataChanged(index(begin, 0), index(prev, 0), roles);
        begin = sorted[i];
        prev = begin;
    }
    emit dataChanged(index(begin, 0), index(prev, 0), roles);
}

quint64 ImportCandidateListModel::selectionRevision() const noexcept
{
    return selection_revision_;
}

void ImportCandidateListModel::notifySelectionIfChanged(const int previous_count,
                                                        const qulonglong previous_bytes,
                                                        const bool membership_changed)
{
    if (membership_changed)
        ++selection_revision_;
    if (previous_count != selected_count_ || previous_bytes != selected_bytes_ ||
        membership_changed)
        emit selectionChanged();
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
    thumbnail_bytes_ = 0;
    highlighted_rows_.clear();
    rows_.reserve(candidates.size());
    for (auto &candidate : candidates)
        rows_.push_back({std::move(candidate), {}, false, false, false, false, 0U, {}});
    for (auto &row : rows_)
        row.selected =
            select_new_candidates_ && row.candidate.supported && !row.candidate.duplicate;
    recountSelection();
    ++selection_revision_;
    endResetModel();
    emit selectionChanged();
    emit candidatesChanged();
}

void ImportCandidateListModel::appendCandidate(ImportCandidate candidate)
{
    const int row = rowCount();
    beginInsertRows({}, row, row);
    const bool selected = select_new_candidates_ && candidate.supported && !candidate.duplicate;
    rows_.push_back({std::move(candidate), {}, selected, false, false, false, 0U, {}});
    if (selected)
        ++selection_revision_;
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
    const auto previous_selected_count = selected_count_;
    const auto previous_selected_bytes = selected_bytes_;
    const auto previous_size = entry.candidate.size_bytes;
    entry.candidate = std::move(candidate);
    entry.inspected = true;
    bool membership_changed = false;
    if (!entry.candidate.supported || entry.candidate.duplicate)
    {
        if (entry.selected)
        {
            entry.selected = false;
            membership_changed = true;
            --selected_count_;
            selected_bytes_ -= previous_size;
        }
        if (entry.highlighted)
        {
            entry.highlighted = false;
            highlighted_rows_.erase(row);
        }
    }
    else if (entry.selected && previous_size != entry.candidate.size_bytes)
    {
        // Size-only totals update: notify aggregates without forging membership revision.
        selected_bytes_ = selected_bytes_ - previous_size + entry.candidate.size_bytes;
    }
    emit dataChanged(index(row, 0), index(row, 0),
                     {SourcePathRole, MediaTypeRole, WidthRole, HeightRole, SizeBytesRole,
                      SelectedRole, HighlightedRole, EligibleRole, DuplicateRole, ErrorRole,
                      InspectedRole, DisplayNameRole});
    notifySelectionIfChanged(previous_selected_count, previous_selected_bytes, membership_changed);
    static_cast<void>(was_selected);
}

void ImportCandidateListModel::setThumbnail(const int row, QImage image)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (!entry.thumbnail.isNull())
        thumbnail_bytes_ -= static_cast<qulonglong>(entry.thumbnail.sizeInBytes());
    std::erase(thumbnail_rows_, row);
    // Single-object oversize: refuse to own the pixels (structured empty result).
    if (!image.isNull() &&
        static_cast<qulonglong>(image.sizeInBytes()) > maximum_cached_thumbnail_bytes)
    {
        entry.thumbnail = {};
        ++entry.thumbnail_revision;
        emit dataChanged(index(row, 0), index(row, 0), {ThumbnailUrlRole});
        return;
    }
    entry.thumbnail = std::move(image);
    if (!entry.thumbnail.isNull())
    {
        thumbnail_rows_.push_back(row);
        thumbnail_bytes_ += static_cast<qulonglong>(entry.thumbnail.sizeInBytes());
    }
    // Metadata may describe 100,000 candidates; owned thumbnail pixels may not.
    auto evict_front = [&]
    {
        const int evicted = thumbnail_rows_.front();
        thumbnail_rows_.pop_front();
        auto &old = rows_[static_cast<std::size_t>(evicted)];
        if (!old.thumbnail.isNull())
            thumbnail_bytes_ -= static_cast<qulonglong>(old.thumbnail.sizeInBytes());
        // Eviction drops residency and restores the historical inspected=false contract.
        // Demand completion lives in the thumbnail controller terminals; a new demand
        // generation (or resetSourceSession) re-admits scroll-back rebuilds.
        old.thumbnail = {};
        old.inspected = false;
        old.thumbnail_loading = false;
        emit dataChanged(index(evicted, 0), index(evicted, 0),
                         {ThumbnailUrlRole, InspectedRole, ThumbnailLoadingRole});
    };
    while (thumbnail_rows_.size() > maximum_cached_thumbnails)
        evict_front();
    while (thumbnail_bytes_ > maximum_cached_thumbnail_bytes && !thumbnail_rows_.empty())
        evict_front();
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
    const int previous_count = selected_count_;
    const auto previous_bytes = selected_bytes_;
    bool membership_changed = false;
    for (std::size_t offset = 0; offset < candidates.size(); ++offset)
    {
        auto &entry = rows_[static_cast<std::size_t>(first) + offset];
        ++selection_row_touches_;
        const bool was_selected = entry.selected;
        if (entry.selected)
        {
            --selected_count_;
            selected_bytes_ -= entry.candidate.size_bytes;
        }
        entry.candidate = std::move(candidates[offset]);
        if (!entry.candidate.supported || entry.candidate.duplicate)
        {
            entry.selected = false;
            if (entry.highlighted)
            {
                entry.highlighted = false;
                highlighted_rows_.erase(first + static_cast<int>(offset));
            }
        }
        if (entry.selected)
        {
            ++selected_count_;
            selected_bytes_ += entry.candidate.size_bytes;
        }
        if (was_selected != entry.selected)
            membership_changed = true;
    }
    emit dataChanged(index(first, 0), index(first + static_cast<int>(candidates.size()) - 1, 0),
                     {SourcePathRole, MediaTypeRole, WidthRole, HeightRole, SizeBytesRole,
                      SelectedRole, HighlightedRole, EligibleRole, DuplicateRole, ErrorRole,
                      DisplayNameRole});
    notifySelectionIfChanged(previous_count, previous_bytes, membership_changed);
}

void ImportCandidateListModel::finishThumbnail(const int row, QImage image,
                                               std::optional<TaskError> error)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    entry.inspected = true;
    entry.thumbnail_loading = false;
    entry.thumbnail_error = std::move(error);
    setThumbnail(row, std::move(image));
    emit dataChanged(index(row, 0), index(row, 0),
                     {InspectedRole, ErrorRole, ThumbnailLoadingRole});
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

void ImportCandidateListModel::setThumbnailLoading(const int row, const bool loading)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (entry.thumbnail_loading == loading)
        return;
    entry.thumbnail_loading = loading;
    emit dataChanged(index(row, 0), index(row, 0), {ThumbnailLoadingRole});
}

bool ImportCandidateListModel::thumbnailLoading(const int row) const
{
    return row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].thumbnail_loading;
}

QStringList ImportCandidateListModel::selectedPaths() const
{
    ++selected_paths_calls_;
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
    if (!eligible(entry))
        return;
    const int previous_count = selected_count_;
    const auto previous_bytes = selected_bytes_;
    const bool membership_changed = setRowSelected(entry, !entry.selected);
    emit dataChanged(index(row, 0), index(row, 0), {SelectedRole});
    notifySelectionIfChanged(previous_count, previous_bytes, membership_changed);
}

void ImportCandidateListModel::setAllSelected(const bool selected)
{
    select_new_candidates_ = selected;
    if (rows_.empty())
        return;
    const int previous_count = selected_count_;
    const auto previous_bytes = selected_bytes_;
    bool membership_changed = false;
    std::vector<int> changed;
    changed.reserve(rows_.size());
    for (int row = 0; row < rowCount(); ++row)
    {
        auto &entry = rows_[static_cast<std::size_t>(row)];
        const bool next = selected && eligible(entry);
        if (!setRowSelected(entry, next))
            continue;
        membership_changed = true;
        changed.push_back(row);
    }
    emitRoleRanges(changed, {SelectedRole});
    notifySelectionIfChanged(previous_count, previous_bytes, membership_changed);
}

void ImportCandidateListModel::selectRange(int first, int last, const bool additive)
{
    if (rows_.empty())
        return;
    first = std::clamp(first, 0, rowCount() - 1);
    last = std::clamp(last, 0, rowCount() - 1);
    if (first > last)
        std::swap(first, last);
    const int previous_count = selected_count_;
    const auto previous_bytes = selected_bytes_;
    bool membership_changed = false;
    std::vector<int> changed;
    // Exact final membership delta: avoid clear-then-set no-op revision bumps.
    if (!additive)
    {
        changed.reserve(static_cast<std::size_t>(rowCount()));
        for (int row = 0; row < rowCount(); ++row)
        {
            auto &entry = rows_[static_cast<std::size_t>(row)];
            const bool next = row >= first && row <= last && eligible(entry);
            if (!setRowSelected(entry, next))
                continue;
            membership_changed = true;
            changed.push_back(row);
        }
    }
    else
    {
        for (int row = first; row <= last; ++row)
        {
            auto &entry = rows_[static_cast<std::size_t>(row)];
            if (!eligible(entry))
                continue;
            if (!setRowSelected(entry, true))
                continue;
            membership_changed = true;
            changed.push_back(row);
        }
    }
    emitRoleRanges(changed, {SelectedRole});
    notifySelectionIfChanged(previous_count, previous_bytes, membership_changed);
}

void ImportCandidateListModel::highlightExclusive(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    if (!eligible(rows_[static_cast<std::size_t>(row)]))
        return;
    std::vector<int> changed;
    changed.reserve(highlighted_rows_.size() + 1);
    const std::vector<int> previous(highlighted_rows_.begin(), highlighted_rows_.end());
    for (const int current : previous)
        if (current != row)
            setRowHighlighted(current, false, &changed);
    setRowHighlighted(row, true, &changed);
    emitRoleRanges(changed, {HighlightedRole});
}

void ImportCandidateListModel::highlightToggle(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &entry = rows_[static_cast<std::size_t>(row)];
    if (!eligible(entry))
        return;
    std::vector<int> changed;
    setRowHighlighted(row, !entry.highlighted, &changed);
    emitRoleRanges(changed, {HighlightedRole});
}

void ImportCandidateListModel::highlightRange(int first, int last, const bool additive)
{
    if (rows_.empty())
        return;
    first = std::clamp(first, 0, rowCount() - 1);
    last = std::clamp(last, 0, rowCount() - 1);
    if (first > last)
        std::swap(first, last);
    std::vector<int> changed;
    if (!additive)
    {
        const std::vector<int> previous(highlighted_rows_.begin(), highlighted_rows_.end());
        for (const int row : previous)
            if (row < first || row > last)
                setRowHighlighted(row, false, &changed);
    }
    for (int row = first; row <= last; ++row)
        setRowHighlighted(row, true, &changed);
    emitRoleRanges(changed, {HighlightedRole});
}

void ImportCandidateListModel::highlightAll()
{
    if (rows_.empty())
        return;
    std::vector<int> changed;
    changed.reserve(static_cast<std::size_t>(rowCount()));
    for (int row = 0; row < rowCount(); ++row)
        setRowHighlighted(row, eligible(rows_[static_cast<std::size_t>(row)]), &changed);
    emitRoleRanges(changed, {HighlightedRole});
}

void ImportCandidateListModel::applyCheck(const int row)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &clicked = rows_[static_cast<std::size_t>(row)];
    if (!eligible(clicked))
        return;
    const bool next = !clicked.selected;
    const int previous_count = selected_count_;
    const auto previous_bytes = selected_bytes_;
    bool membership_changed = false;
    std::vector<int> changed;
    if (clicked.highlighted)
    {
        const std::vector<int> targets(highlighted_rows_.begin(), highlighted_rows_.end());
        changed.reserve(targets.size());
        for (const int target : targets)
        {
            auto &entry = rows_[static_cast<std::size_t>(target)];
            if (!eligible(entry))
                continue;
            if (!setRowSelected(entry, next))
                continue;
            membership_changed = true;
            changed.push_back(target);
        }
    }
    else if (setRowSelected(clicked, next))
    {
        membership_changed = true;
        changed.push_back(row);
    }
    emitRoleRanges(changed, {SelectedRole});
    notifySelectionIfChanged(previous_count, previous_bytes, membership_changed);
}

bool ImportCandidateListModel::highlighted(const int row) const
{
    return row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].highlighted;
}

} // namespace ravo
