#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_commands.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QVariantList tone_curve_to_variant(const std::vector<ToneCurvePoint> &points)
{
    std::vector<ToneCurvePoint> display = points;
    if (tone_curve_is_identity(display))
    {
        display = {{0.0, 0.0}, {1.0, 1.0}};
    }
    QVariantList list;
    list.reserve(static_cast<qsizetype>(display.size()));
    for (const auto &point : display)
    {
        QVariantMap item;
        item.insert(QStringLiteral("x"), point.x);
        item.insert(QStringLiteral("y"), point.y);
        list.push_back(item);
    }
    return list;
}

[[nodiscard]] std::vector<ToneCurvePoint> tone_curve_from_variant(const QVariantList &list)
{
    std::vector<ToneCurvePoint> points;
    points.reserve(static_cast<std::size_t>(std::max<qsizetype>(0, list.size())));
    for (const auto &item : list)
    {
        const auto map = item.toMap();
        points.push_back(
            {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()});
    }
    clamp_tone_curve(points);
    return points;
}

[[nodiscard]] QVariantList tone_curve_sample_list(const std::vector<ToneCurvePoint> &points)
{
    constexpr int kSamples = 65;
    QVariantList samples;
    samples.reserve(kSamples);
    for (int index = 0; index < kSamples; ++index)
    {
        const double x = static_cast<double>(index) / static_cast<double>(kSamples - 1);
        samples.push_back(evaluate_tone_curve(points, x));
    }
    return samples;
}

[[nodiscard]] std::string utf8_from_qstring(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] std::string preview_root_for(const std::string &database_path)
{
    return database_path + ".preview";
}

[[nodiscard]] QString pictures_directory()
{
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty())
    {
        pictures = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    return pictures;
}

[[nodiscard]] QUrl url_from_dialog_path(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
    {
        return {};
    }
    if (trimmed.startsWith(QStringLiteral("file:")))
    {
        return QUrl(trimmed);
    }
    return QUrl::fromLocalFile(trimmed);
}

[[nodiscard]] Result<ExportFormat> export_format_from_ui(const QString &path, const QString &filter)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("png"))
    {
        return ExportFormat::kPng;
    }
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
    {
        return ExportFormat::kJpeg;
    }
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff"))
    {
        return ExportFormat::kTiff;
    }
    const QString lowered = filter.toLower();
    if (lowered.contains(QLatin1String("original")))
    {
        return ExportFormat::kOriginalCopy;
    }
    if (lowered.contains(QLatin1String("jpeg")) || lowered.contains(QLatin1String("jpg")))
    {
        return ExportFormat::kJpeg;
    }
    if (lowered.contains(QLatin1String("png")))
    {
        return ExportFormat::kPng;
    }
    if (lowered.contains(QLatin1String("tif")))
    {
        return ExportFormat::kTiff;
    }
    if (suffix.isEmpty())
    {
        return ExportFormat::kPng;
    }
    return make_error(ErrorCode::kValidation, "Unable to infer export format",
                      {{"path", utf8_from_qstring(path)}, {"filter", utf8_from_qstring(filter)}});
}

[[nodiscard]] QString describe_import(const std::vector<ImportItemResult> &results)
{
    int imported = 0;
    int duplicate = 0;
    int unsupported = 0;
    int failed = 0;
    for (const auto &item : results)
    {
        switch (item.status)
        {
        case ImportItemStatus::kImported:
            ++imported;
            break;
        case ImportItemStatus::kDuplicate:
            ++duplicate;
            break;
        case ImportItemStatus::kUnsupported:
            ++unsupported;
            break;
        case ImportItemStatus::kFailed:
            ++failed;
            break;
        }
    }
    return QStringLiteral("Imported %1, duplicate %2, unsupported %3, failed %4")
        .arg(imported)
        .arg(duplicate)
        .arg(unsupported)
        .arg(failed);
}

[[nodiscard]] QString rating_mode_name(const RatingFilterMode mode)
{
    switch (mode)
    {
    case RatingFilterMode::kMinimum:
        return QStringLiteral("min");
    case RatingFilterMode::kExact:
        return QStringLiteral("exact");
    case RatingFilterMode::kAny:
        break;
    }
    return QStringLiteral("any");
}

[[nodiscard]] QString reject_filter_name(const RejectFilter filter)
{
    switch (filter)
    {
    case RejectFilter::kExclude:
        return QStringLiteral("exclude");
    case RejectFilter::kOnly:
        return QStringLiteral("only");
    case RejectFilter::kInclude:
        break;
    }
    return QStringLiteral("include");
}

[[nodiscard]] QString sort_field_name(const AssetSortField field)
{
    switch (field)
    {
    case AssetSortField::kDisplayName:
        return QStringLiteral("name");
    case AssetSortField::kRating:
        return QStringLiteral("rating");
    case AssetSortField::kImportTime:
        break;
    }
    return QStringLiteral("imported");
}

} // namespace

AssetListModel::AssetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AssetListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(assets_.size());
}

QVariant AssetListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    const auto &asset = assets_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case AssetIdRole:
        return qstring_from_utf8(asset.id);
    case DisplayNameRole:
        return qstring_from_utf8(asset_display_name(asset));
    case MediaTypeRole:
        return qstring_from_utf8(asset.media_type);
    case ImportStateRole:
        return qstring_from_utf8(asset.import_state);
    case ErrorRole:
        return asset.error_message ? qstring_from_utf8(*asset.error_message) : QString{};
    case RatingRole:
        return asset.review.rating;
    case ColorLabelRole:
        return qstring_from_utf8(color_label_name(asset.review.color_label));
    case RejectedRole:
        return asset.review.rejected;
    case ThumbnailUrlRole:
    {
        const auto found = thumbnail_urls_.find(asset.id);
        return found == thumbnail_urls_.end() ? QUrl{} : found->second;
    }
    case ThumbnailStateRole:
    {
        const auto found = thumbnail_states_.find(asset.id);
        if (found != thumbnail_states_.end())
        {
            return found->second;
        }
        return asset.import_state == kImportStateMissing ? QStringLiteral("missing") :
                                                           QStringLiteral("pending");
    }
    case WidthRole:
        return asset.width.value_or(0);
    case HeightRole:
        return asset.height.value_or(0);
    case HasEditsRole:
        return asset.has_edits;
    case SelectedRole:
        return selected_ids_.contains(asset.id);
    default:
        return {};
    }
}

QHash<int, QByteArray> AssetListModel::roleNames() const
{
    return {{AssetIdRole, "assetId"},           {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},       {ImportStateRole, "importState"},
            {ErrorRole, "errorText"},           {RatingRole, "rating"},
            {ColorLabelRole, "colorLabel"},     {RejectedRole, "rejected"},
            {ThumbnailUrlRole, "thumbnailUrl"}, {ThumbnailStateRole, "thumbnailState"},
            {WidthRole, "pixelWidth"},          {HeightRole, "pixelHeight"},
            {HasEditsRole, "hasEdits"},         {SelectedRole, "selected"}};
}

void AssetListModel::setAssets(std::vector<AssetRecord> assets)
{
    beginResetModel();
    assets_ = std::move(assets);
    std::unordered_map<std::string, QUrl> kept_urls;
    std::unordered_map<std::string, QString> kept_states;
    for (const auto &asset : assets_)
    {
        if (const auto found = thumbnail_urls_.find(asset.id); found != thumbnail_urls_.end())
        {
            kept_urls.emplace(found->first, found->second);
        }
        if (const auto found = thumbnail_states_.find(asset.id); found != thumbnail_states_.end())
        {
            kept_states.emplace(found->first, found->second);
        }
    }
    thumbnail_urls_ = std::move(kept_urls);
    thumbnail_states_ = std::move(kept_states);
    std::unordered_set<std::string> kept_selected;
    for (const auto &asset : assets_)
    {
        if (selected_ids_.contains(asset.id))
        {
            kept_selected.insert(asset.id);
        }
    }
    selected_ids_ = std::move(kept_selected);
    endResetModel();
}

void AssetListModel::setThumbnail(const std::string &asset_id, const QUrl &url,
                                  const QString &state)
{
    thumbnail_urls_[asset_id] = url;
    thumbnail_states_[asset_id] = state;
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ThumbnailUrlRole, ThumbnailStateRole});
}

void AssetListModel::updateAsset(const AssetRecord &asset)
{
    const auto row = indexOf(qstring_from_utf8(asset.id));
    if (row < 0)
    {
        return;
    }
    assets_[static_cast<std::size_t>(row)] = asset;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index);
}

void AssetListModel::markOriginalMissing(const std::string &asset_id)
{
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    auto &asset = assets_[static_cast<std::size_t>(row)];
    asset.import_state = std::string(kImportStateMissing);
    thumbnail_states_[asset_id] = QStringLiteral("missing");
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ImportStateRole, ThumbnailStateRole});
}

void AssetListModel::setSelectedIds(std::unordered_set<std::string> ids)
{
    std::unordered_set<std::string> changed = selected_ids_;
    for (const auto &id : ids)
    {
        changed.insert(id);
    }
    selected_ids_ = std::move(ids);
    for (const auto &id : changed)
    {
        const auto row = indexOf(qstring_from_utf8(id));
        if (row < 0)
        {
            continue;
        }
        const auto model_index = index(row, 0);
        emit dataChanged(model_index, model_index, {SelectedRole});
    }
}

bool AssetListModel::isSelected(const std::string &asset_id) const
{
    return selected_ids_.contains(asset_id);
}

int AssetListModel::indexOf(const QString &asset_id) const
{
    const auto id = utf8_from_qstring(asset_id);
    for (int row = 0; row < static_cast<int>(assets_.size()); ++row)
    {
        if (assets_[static_cast<std::size_t>(row)].id == id)
        {
            return row;
        }
    }
    return -1;
}

std::optional<AssetRecord> AssetListModel::assetById(const QString &asset_id) const
{
    const auto row = indexOf(asset_id);
    if (row < 0)
    {
        return std::nullopt;
    }
    return assets_[static_cast<std::size_t>(row)];
}

QString AssetListModel::assetIdAt(const int row) const
{
    if (row < 0 || row >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    return qstring_from_utf8(assets_[static_cast<std::size_t>(row)].id);
}

FolderListModel::FolderListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FolderListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(folders_.size());
}

QVariant FolderListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(folders_.size()))
    {
        return {};
    }
    const auto &row = folders_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case FolderUriRole:
        return qstring_from_utf8(row.folder.uri);
    case DisplayNameRole:
        return qstring_from_utf8(row.folder.display_name);
    case DepthRole:
        return row.folder.depth;
    case AssetCountRole:
        return row.folder.asset_count;
    case HasChildrenRole:
        return row.has_children;
    case HasNextSiblingRole:
        return row.has_next_sibling;
    case AncestorLineContinuesRole:
    {
        QVariantList continues;
        continues.reserve(static_cast<qsizetype>(row.ancestor_line_continues.size()));
        for (const char value : row.ancestor_line_continues)
        {
            continues.push_back(value != 0);
        }
        return continues;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderListModel::roleNames() const
{
    return {{FolderUriRole, "folderUri"},
            {DisplayNameRole, "displayName"},
            {DepthRole, "depth"},
            {AssetCountRole, "assetCount"},
            {HasChildrenRole, "hasChildren"},
            {HasNextSiblingRole, "hasNextSibling"},
            {AncestorLineContinuesRole, "ancestorLineContinues"}};
}

void FolderListModel::setFolders(std::vector<FolderRecord> folders)
{
    beginResetModel();
    folders_.clear();
    folders_.reserve(folders.size());
    for (std::size_t index = 0; index < folders.size(); ++index)
    {
        FolderRow row;
        row.folder = folders[index];
        const int depth = row.folder.depth;
        if (index + 1 < folders.size() && folders[index + 1U].depth == depth + 1)
        {
            row.has_children = true;
        }
        for (std::size_t later = index + 1; later < folders.size(); ++later)
        {
            if (folders[later].depth < depth)
            {
                break;
            }
            if (folders[later].depth == depth)
            {
                row.has_next_sibling = true;
                break;
            }
        }
        row.ancestor_line_continues.assign(static_cast<std::size_t>(std::max(0, depth)), 0);
        for (int level = 0; level < depth; ++level)
        {
            for (std::size_t later = index + 1; later < folders.size(); ++later)
            {
                if (folders[later].depth < level)
                {
                    break;
                }
                if (folders[later].depth == level)
                {
                    row.ancestor_line_continues[static_cast<std::size_t>(level)] = 1;
                    break;
                }
            }
        }
        folders_.push_back(std::move(row));
    }
    endResetModel();
}

StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent)
    , assets_(this)
    , folders_(this)
{
    const auto created = executor_.submit(
        [this]() -> Result<void>
        {
            auto engine = EngineFacade::create_phase1();
            if (!engine)
            {
                return engine.error();
            }
            engine_ = std::move(engine).value();
            return {};
        });
    if (!created)
    {
        error_text_ = qstring_from_utf8(created.error().message);
        status_text_ = QStringLiteral("Engine failed to start.");
    }
}

StudioPresenter::~StudioPresenter()
{
    static_cast<void>(shutdown_.cancel("window_closed"));
    executor_.submit(
        [this]()
        {
            service_.reset();
            engine_.reset();
        });
    executor_.request_stop();
    executor_.wait();
}

bool StudioPresenter::catalogOpen() const noexcept
{
    return !catalog_path_.isEmpty();
}

QString StudioPresenter::catalogPath() const
{
    return catalog_path_;
}

QUrl StudioPresenter::defaultCatalogFolder() const
{
    return QUrl::fromLocalFile(pictures_directory());
}

QUrl StudioPresenter::defaultCatalogFile() const
{
    return QUrl::fromLocalFile(
        QDir(pictures_directory()).filePath(QStringLiteral("Ravo Library.sqlite")));
}

bool StudioPresenter::defaultCatalogExists() const
{
    const QFileInfo info(defaultCatalogFile().toLocalFile());
    return info.exists() && info.isFile();
}

bool StudioPresenter::busy() const noexcept
{
    return busy_;
}

QString StudioPresenter::statusText() const
{
    return status_text_;
}

QString StudioPresenter::errorText() const
{
    return error_text_;
}

QString StudioPresenter::selectedAssetId() const
{
    return selected_asset_id_;
}

int StudioPresenter::selectedIndex() const
{
    return assets_.indexOf(selected_asset_id_);
}

int StudioPresenter::selectedCount() const noexcept
{
    return static_cast<int>(selected_ids_.size());
}

bool StudioPresenter::isAssetSelected(const QString &asset_id) const
{
    return selected_ids_.contains(utf8_from_qstring(asset_id));
}

int StudioPresenter::selectedRating() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? asset->review.rating : 0;
}

QString StudioPresenter::selectedColorLabel() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(color_label_name(asset->review.color_label)) :
                   QStringLiteral("none");
}

bool StudioPresenter::selectedRejected() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->review.rejected;
}

QString StudioPresenter::selectedImportState() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->import_state) : QString{};
}

bool StudioPresenter::canDeleteFromDisk() const
{
    if (selected_ids_.empty())
    {
        return false;
    }
    for (const auto &id : selected_ids_)
    {
        const auto asset = assets_.assetById(qstring_from_utf8(id));
        if (!asset || asset->import_state == kImportStateMissing)
        {
            return false;
        }
    }
    return true;
}

QUrl StudioPresenter::previewUrl() const
{
    return preview_url_;
}

QImage StudioPresenter::previewImage() const
{
    const QMutexLocker lock(&preview_image_mutex_);
    return preview_image_;
}

void StudioPresenter::clear_displayed_preview()
{
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = QImage();
        preview_url_.clear();
    }
    clear_scopes();
}

QVariantList StudioPresenter::histogram_channel_list(
    const std::array<std::uint32_t, kRgbHistogramBins> &channel)
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(kRgbHistogramBins));
    for (const auto count : channel)
    {
        list.push_back(QVariant::fromValue(count));
    }
    return list;
}

void StudioPresenter::clear_scopes()
{
    scope_histogram_ = {};
    scope_parade_image_ = QImage();
    scope_parade_url_.clear();
    ++scope_revision_;
    emit scopesChanged();
}

void StudioPresenter::refresh_scopes(const QImage &image)
{
    if (image.isNull())
    {
        clear_scopes();
        return;
    }
    QImage rgb = image;
    if (rgb.format() != QImage::Format_RGB888)
    {
        rgb = rgb.convertToFormat(QImage::Format_RGB888);
    }
    RasterBuffer raster;
    raster.width = static_cast<std::uint32_t>(rgb.width());
    raster.height = static_cast<std::uint32_t>(rgb.height());
    raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    for (std::uint32_t y = 0; y < raster.height; ++y)
    {
        const auto *row = rgb.constScanLine(static_cast<int>(y));
        std::copy_n(row, static_cast<std::size_t>(raster.width) * 3U,
                    raster.srgb.begin() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * raster.width * 3U));
    }
    auto histogram = collect_rgb_histogram(raster);
    auto parade = collect_rgb_parade(raster);
    if (!histogram || !parade)
    {
        clear_scopes();
        return;
    }
    scope_histogram_ = std::move(histogram).value();
    const auto &parade_value = parade.value();
    if (parade_value.bins == 0 || parade_value.tones == 0 ||
        parade_value.rgb.size() !=
            static_cast<std::size_t>(parade_value.bins) * 3U * parade_value.tones * 3U)
    {
        scope_parade_image_ = QImage();
    }
    else
    {
        const QImage view(parade_value.rgb.data(), static_cast<int>(parade_value.bins * 3U),
                          static_cast<int>(parade_value.tones),
                          static_cast<int>(parade_value.bins * 9U), QImage::Format_RGB888);
        scope_parade_image_ = view.copy();
    }
    ++scope_revision_;
    scope_parade_url_ =
        QUrl(QStringLiteral("image://studioScope/parade?r=%1").arg(scope_revision_));
    emit scopesChanged();
}

void StudioPresenter::show_preview_result(const PreviewResult &preview,
                                          const std::uint64_t revision)
{
    if (!preview.srgb.empty())
    {
        const auto expected = static_cast<std::size_t>(preview.width) * preview.height * 3U;
        if (preview.width == 0 || preview.height == 0 || preview.srgb.size() != expected)
        {
            setError(QStringLiteral("Interactive preview pixels are invalid"));
            return;
        }
        const QImage view(preview.srgb.data(), static_cast<int>(preview.width),
                          static_cast<int>(preview.height), static_cast<int>(preview.width * 3U),
                          QImage::Format_RGB888);
        const QImage owned = view.copy();
        {
            const QMutexLocker lock(&preview_image_mutex_);
            preview_image_ = owned;
        }
        preview_url_ = QUrl(QStringLiteral("image://studioPreview/live?r=%1").arg(revision));
        refresh_scopes(owned);
        return;
    }
    const QImage cached = QImage(qstring_from_utf8(preview.cache_path));
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = cached;
    }
    preview_url_ = QUrl::fromLocalFile(qstring_from_utf8(preview.cache_path));
    refresh_scopes(cached);
}

bool StudioPresenter::previewLoading() const noexcept
{
    return preview_loading_;
}

QString StudioPresenter::scopeMode() const
{
    return scope_mode_;
}

void StudioPresenter::setScopeMode(const QString &mode)
{
    const QString next = mode == QLatin1String("parade") ? QStringLiteral("parade") :
                                                           QStringLiteral("histogram");
    if (scope_mode_ == next)
    {
        return;
    }
    scope_mode_ = next;
    emit scopesChanged();
}

QVariantList StudioPresenter::scopeHistogramRed() const
{
    return histogram_channel_list(scope_histogram_.red);
}

QVariantList StudioPresenter::scopeHistogramGreen() const
{
    return histogram_channel_list(scope_histogram_.green);
}

QVariantList StudioPresenter::scopeHistogramBlue() const
{
    return histogram_channel_list(scope_histogram_.blue);
}

double StudioPresenter::scopeHistogramMax() const noexcept
{
    return static_cast<double>(scope_histogram_.max_count);
}

QUrl StudioPresenter::scopeParadeUrl() const
{
    return scope_parade_url_;
}

QImage StudioPresenter::scopeParadeImage() const
{
    return scope_parade_image_;
}

QString StudioPresenter::browseMode() const
{
    return browse_mode_;
}

QString StudioPresenter::zoomMode() const
{
    return zoom_mode_;
}

double StudioPresenter::zoomFactor() const noexcept
{
    return zoom_factor_;
}

int StudioPresenter::thumbnailSize() const noexcept
{
    return thumbnail_size_;
}

QString StudioPresenter::ratingFilterMode() const
{
    return rating_mode_name(query_.rating_mode);
}

int StudioPresenter::ratingFilterValue() const noexcept
{
    return query_.rating_value;
}

QStringList StudioPresenter::colorFilters() const
{
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(query_.color_labels.size()));
    for (const auto label : query_.color_labels)
    {
        labels.push_back(qstring_from_utf8(color_label_name(label)));
    }
    return labels;
}

QString StudioPresenter::rejectFilter() const
{
    return reject_filter_name(query_.reject_filter);
}

QString StudioPresenter::sortField() const
{
    return sort_field_name(query_.sort_field);
}

QString StudioPresenter::sortDirection() const
{
    return query_.sort_direction == SortDirection::kAscending ? QStringLiteral("asc") :
                                                                QStringLiteral("desc");
}

int StudioPresenter::visibleCount() const
{
    return assets_.rowCount();
}

bool StudioPresenter::filtersActive() const noexcept
{
    return query_.rating_mode != RatingFilterMode::kAny || !query_.color_labels.empty() ||
           query_.reject_filter != RejectFilter::kInclude || !query_.tag.empty();
}

bool StudioPresenter::selectedHasEdits() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->has_edits;
}

bool StudioPresenter::beforeAfter() const noexcept
{
    return before_after_;
}

bool StudioPresenter::canUndo() const noexcept
{
    return !undo_stack_.empty();
}

bool StudioPresenter::canRedo() const noexcept
{
    return !redo_stack_.empty();
}

double StudioPresenter::editTemperature() const noexcept
{
    return develop_.temperature;
}
double StudioPresenter::editTint() const noexcept
{
    return develop_.tint;
}
double StudioPresenter::editExposure() const noexcept
{
    return develop_.exposure_ev;
}
double StudioPresenter::editContrast() const noexcept
{
    return develop_.contrast;
}
double StudioPresenter::editHighlights() const noexcept
{
    return develop_.highlights;
}
double StudioPresenter::editShadows() const noexcept
{
    return develop_.shadows;
}
double StudioPresenter::editWhites() const noexcept
{
    return develop_.whites;
}
double StudioPresenter::editBlacks() const noexcept
{
    return develop_.blacks;
}
double StudioPresenter::editVibrance() const noexcept
{
    return develop_.vibrance;
}
double StudioPresenter::editSaturation() const noexcept
{
    return develop_.saturation;
}
int StudioPresenter::editRotateQuarters() const noexcept
{
    return static_cast<int>(develop_.rotate_quarters);
}
double StudioPresenter::editCropX() const noexcept
{
    return develop_.crop_x;
}
double StudioPresenter::editCropY() const noexcept
{
    return develop_.crop_y;
}
double StudioPresenter::editCropWidth() const noexcept
{
    return develop_.crop_width;
}
double StudioPresenter::editCropHeight() const noexcept
{
    return develop_.crop_height;
}
double StudioPresenter::editStraighten() const noexcept
{
    return develop_.straighten_degrees;
}
QString StudioPresenter::cropAspect() const
{
    return crop_aspect_;
}
double StudioPresenter::cropAspectRatio() const noexcept
{
    if (crop_aspect_ == QLatin1String("1:1"))
    {
        return 1.0;
    }
    if (crop_aspect_ == QLatin1String("3:2"))
    {
        return 1.5;
    }
    if (crop_aspect_ == QLatin1String("4:3"))
    {
        return 4.0 / 3.0;
    }
    if (crop_aspect_ == QLatin1String("5:4"))
    {
        return 1.25;
    }
    if (crop_aspect_ == QLatin1String("16:9"))
    {
        return 16.0 / 9.0;
    }
    return 0.0;
}
void StudioPresenter::valid_crop_rect(double &x, double &y, double &width, double &height) const
{
    const double working_aspect = selected_working_aspect();
    const double ratio = cropAspectRatio() > 0.0 ?
                             cropAspectRatio() / std::max(working_aspect, 1e-6) :
                             develop_.crop_width / std::max(develop_.crop_height, 1e-6);
    inscribed_crop_for_straighten(develop_.straighten_degrees, working_aspect, ratio, x, y, width,
                                  height);
}
double StudioPresenter::validCropX() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return x;
}
double StudioPresenter::validCropY() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return y;
}
double StudioPresenter::validCropWidth() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return width;
}
double StudioPresenter::validCropHeight() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return height;
}
bool StudioPresenter::editFlipHorizontal() const noexcept
{
    return develop_.flip_horizontal != 0;
}
bool StudioPresenter::editFlipVertical() const noexcept
{
    return develop_.flip_vertical != 0;
}
double StudioPresenter::editSharpen() const noexcept
{
    return develop_.sharpen;
}
double StudioPresenter::editSharpenRadius() const noexcept
{
    return develop_.sharpen_radius;
}
double StudioPresenter::editClarity() const noexcept
{
    return develop_.clarity;
}
double StudioPresenter::editVignette() const noexcept
{
    return develop_.vignette;
}
double StudioPresenter::editGrain() const noexcept
{
    return develop_.grain;
}
double StudioPresenter::editBloom() const noexcept
{
    return develop_.bloom;
}
double StudioPresenter::editSoften() const noexcept
{
    return develop_.soften;
}
double StudioPresenter::editDehaze() const noexcept
{
    return develop_.dehaze;
}
double StudioPresenter::editVelvia() const noexcept
{
    return develop_.velvia;
}
double StudioPresenter::editLift() const noexcept
{
    return develop_.lift;
}
double StudioPresenter::editColorGamma() const noexcept
{
    return develop_.color_gamma;
}
double StudioPresenter::editGain() const noexcept
{
    return develop_.gain;
}
double StudioPresenter::editColorContrast() const noexcept
{
    return develop_.color_contrast;
}
double StudioPresenter::editMonochrome() const noexcept
{
    return develop_.monochrome;
}
double StudioPresenter::editSplitShadowsHue() const noexcept
{
    return develop_.split_shadows_hue;
}
double StudioPresenter::editSplitHighlightsHue() const noexcept
{
    return develop_.split_highlights_hue;
}
double StudioPresenter::editSplitBalance() const noexcept
{
    return develop_.split_balance;
}
double StudioPresenter::editSplitAmount() const noexcept
{
    return develop_.split_amount;
}
double StudioPresenter::editGamma() const noexcept
{
    return develop_.gamma;
}
QVariantList StudioPresenter::editToneCurve() const
{
    return tone_curve_to_variant(develop_.tone_curve);
}
QVariantList StudioPresenter::editToneCurveSamples() const
{
    return tone_curve_sample_list(develop_.tone_curve);
}
bool StudioPresenter::editSigmoidEnabled() const noexcept
{
    return develop_.sigmoid_enabled;
}
double StudioPresenter::editSigmoidContrast() const noexcept
{
    return develop_.sigmoid_contrast;
}
double StudioPresenter::editSigmoidSkew() const noexcept
{
    return develop_.sigmoid_skew;
}
double StudioPresenter::editSigmoidHuePreservation() const noexcept
{
    return develop_.sigmoid_hue_preservation;
}
double StudioPresenter::editRawHighlights() const noexcept
{
    return develop_.raw_highlights;
}
double StudioPresenter::editDenoise() const noexcept
{
    return develop_.denoise;
}
double StudioPresenter::editDenoiseChroma() const noexcept
{
    return develop_.denoise_chroma;
}
double StudioPresenter::editDenoiseRadius() const noexcept
{
    return develop_.denoise_radius;
}
double StudioPresenter::editLensK1() const noexcept
{
    return develop_.lens_k1;
}
double StudioPresenter::editLensVignetting() const noexcept
{
    return develop_.lens_vignetting;
}
double StudioPresenter::editLensMode() const noexcept
{
    return develop_.lens_mode == kLensModeLookup ? 1.0 : 0.0;
}
int StudioPresenter::editColorEqBand() const noexcept
{
    return static_cast<int>(develop_.color_eq_band);
}
double StudioPresenter::editColorEqHue() const noexcept
{
    return develop_.color_eq_hue[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}
double StudioPresenter::editColorEqSat() const noexcept
{
    return develop_.color_eq_sat[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}
double StudioPresenter::editColorEqLight() const noexcept
{
    return develop_.color_eq_light[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}
double StudioPresenter::editGraduatedDensity() const noexcept
{
    return develop_.graduated_density;
}
double StudioPresenter::editGraduatedHardness() const noexcept
{
    return develop_.graduated_hardness;
}
double StudioPresenter::editGraduatedRotation() const noexcept
{
    return develop_.graduated_rotation;
}
double StudioPresenter::editGraduatedOffset() const noexcept
{
    return develop_.graduated_offset;
}
double StudioPresenter::editToneEqBlacks() const noexcept
{
    return develop_.tone_eq_blacks;
}
double StudioPresenter::editToneEqShadows() const noexcept
{
    return develop_.tone_eq_shadows;
}
double StudioPresenter::editToneEqMidtones() const noexcept
{
    return develop_.tone_eq_midtones;
}
double StudioPresenter::editToneEqHighlights() const noexcept
{
    return develop_.tone_eq_highlights;
}
double StudioPresenter::editToneEqWhites() const noexcept
{
    return develop_.tone_eq_whites;
}
QString StudioPresenter::selectedTags() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList tags;
    for (const auto &tag : asset->tags)
    {
        tags.push_back(qstring_from_utf8(tag));
    }
    return tags.join(QStringLiteral(", "));
}
QString StudioPresenter::selectedTitle() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.title ? qstring_from_utf8(*asset->metadata.title) : QString{};
}
QString StudioPresenter::selectedDescription() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.description ? qstring_from_utf8(*asset->metadata.description) :
                                                  QString{};
}
QString StudioPresenter::selectedCreator() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.creator ? qstring_from_utf8(*asset->metadata.creator) : QString{};
}
QString StudioPresenter::selectedCopyright() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.copyright ? qstring_from_utf8(*asset->metadata.copyright) :
                                                QString{};
}
QString StudioPresenter::selectedCaptureSummary() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList parts;
    if (asset->capture.camera_make)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_make));
    }
    if (asset->capture.camera_model)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_model));
    }
    if (asset->capture.iso)
    {
        parts.push_back(QStringLiteral("ISO %1").arg(*asset->capture.iso, 0, 'f', 0));
    }
    if (asset->capture.aperture)
    {
        parts.push_back(QStringLiteral("f/%1").arg(*asset->capture.aperture, 0, 'f', 1));
    }
    if (asset->capture.focal_length_mm)
    {
        parts.push_back(QStringLiteral("%1 mm").arg(*asset->capture.focal_length_mm, 0, 'f', 0));
    }
    return parts.join(QStringLiteral(" · "));
}
QVariantList StudioPresenter::recipeHistory() const
{
    return recipe_history_;
}
QString StudioPresenter::tagFilter() const
{
    return qstring_from_utf8(query_.tag);
}
bool StudioPresenter::cropToolActive() const noexcept
{
    return crop_tool_active_;
}
bool StudioPresenter::cropGuideReady() const noexcept
{
    return crop_guide_ready_;
}

AssetListModel *StudioPresenter::assets() noexcept
{
    return &assets_;
}

FolderListModel *StudioPresenter::folders() noexcept
{
    return &folders_;
}

QUrl StudioPresenter::selectedThumbnailUrl() const
{
    const int row = assets_.indexOf(selected_asset_id_);
    if (row < 0)
    {
        return {};
    }
    return assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
}

QString StudioPresenter::selectedFolderUri() const
{
    return qstring_from_utf8(query_.folder_uri);
}

QString StudioPresenter::selectedDisplayName() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset_display_name(*asset)) : QString{};
}

QString StudioPresenter::selectedFolderPath() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    const QUrl file = QUrl(qstring_from_utf8(asset->normalized_uri));
    return QFileInfo(file.toLocalFile()).absolutePath();
}

QString StudioPresenter::selectedMediaType() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->media_type) : QString{};
}

QString StudioPresenter::selectedDimensions() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->width || !asset->height)
    {
        return {};
    }
    return QStringLiteral("%1 × %2").arg(*asset->width).arg(*asset->height);
}

QString StudioPresenter::selectedFileSize() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || asset->size_bytes == 0)
    {
        return {};
    }
    const auto bytes = static_cast<double>(asset->size_bytes);
    if (bytes < 1024.0)
    {
        return QStringLiteral("%1 B").arg(asset->size_bytes);
    }
    if (bytes < 1024.0 * 1024.0)
    {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString StudioPresenter::selectedUri() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->normalized_uri) : QString{};
}

LibraryQuery StudioPresenter::current_query() const
{
    return query_;
}

void StudioPresenter::setBusy(const bool busy)
{
    if (busy_ == busy)
    {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void StudioPresenter::setStatus(QString text)
{
    if (status_text_ == text)
    {
        return;
    }
    status_text_ = std::move(text);
    emit statusChanged();
}

void StudioPresenter::setError(QString text)
{
    if (error_text_ == text)
    {
        return;
    }
    error_text_ = std::move(text);
    emit errorChanged();
}

void StudioPresenter::applyAssets(std::vector<AssetRecord> assets, const bool restore_selection)
{
    const QString previous = selected_asset_id_;
    assets_.setAssets(std::move(assets));
    std::unordered_set<std::string> kept;
    for (const auto &id : selected_ids_)
    {
        if (assets_.indexOf(qstring_from_utf8(id)) >= 0)
        {
            kept.insert(id);
        }
    }
    selected_ids_ = std::move(kept);
    assets_.setSelectedIds(selected_ids_);
    emit filterChanged();
    emit selectionChanged();
    if (!restore_selection)
    {
        return;
    }
    if (!previous.isEmpty() && assets_.indexOf(previous) >= 0)
    {
        if (selected_asset_id_ != previous)
        {
            selectAsset(previous);
        }
        else
        {
            publish_selection();
        }
        return;
    }
    if (assets_.rowCount() == 0)
    {
        selected_asset_id_.clear();
        selection_anchor_id_.clear();
        selected_ids_.clear();
        assets_.setSelectedIds({});
        clear_displayed_preview();
        preview_loading_ = false;
        emit selectionChanged();
        emit previewChanged();
        return;
    }
    if (!selected_ids_.empty())
    {
        const auto remaining = selected_asset_ids();
        activate_primary(qstring_from_utf8(remaining.front()), true);
        return;
    }
    if (selected_asset_id_.isEmpty() || assets_.indexOf(selected_asset_id_) < 0)
    {
        selectAsset(assets_.assetIdAt(0));
    }
}

void StudioPresenter::applyFolders(std::vector<FolderRecord> folders)
{
    folders_.setFolders(std::move(folders));
    emit folderChanged();
}

void StudioPresenter::selectFolder(const QString &folder_uri)
{
    const auto next = utf8_from_qstring(folder_uri);
    if (query_.folder_uri == next)
    {
        return;
    }
    query_.folder_uri = next;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::reloadVisibleAssets()
{
    if (catalog_path_.isEmpty())
    {
        return;
    }
    executor_.post(
        [this, query = current_query()]()
        {
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                listed = service_->list_assets(query);
                folders = service_->list_folders();
            }
            QMetaObject::invokeMethod(
                this,
                [this, listed = std::move(listed), folders = std::move(folders)]() mutable
                {
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), true);
                },
                Qt::QueuedConnection);
        });
}

Result<std::unique_ptr<CatalogService>>
StudioPresenter::make_catalog_service(const std::string &path, const bool create)
{
    if (!engine_)
    {
        return make_error(ErrorCode::kInternal, "Engine is not available");
    }
    auto repository =
        create ? SqliteCatalogRepository::create(path) : SqliteCatalogRepository::open(path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(preview_root_for(path));
    if (!cache)
    {
        return cache.error();
    }
    auto raster = std::make_unique<QtRasterDecoder>();
    return std::make_unique<CatalogService>(*engine_, std::move(repository).value(),
                                            std::move(raster), std::move(cache).value());
}

void StudioPresenter::createCatalog(const QUrl &file_url)
{
    if (busy_)
    {
        return;
    }
    const QString local = file_url.toLocalFile();
    if (local.isEmpty())
    {
        setError(QStringLiteral("Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Creating library…"));
    const auto path = utf8_from_qstring(local);
    executor_.post(
        [this, path]()
        {
            QString failure;
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            auto built = make_catalog_service(path, true);
            if (!built)
            {
                failure = qstring_from_utf8(built.error().message);
            }
            else
            {
                listed = built.value()->list_assets(query_);
                if (!listed)
                {
                    failure = qstring_from_utf8(listed.error().message);
                }
                else
                {
                    folders = built.value()->list_folders();
                    if (!folders)
                    {
                        failure = qstring_from_utf8(folders.error().message);
                    }
                    else
                    {
                        service_ = std::move(built).value();
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, failure = std::move(failure), listed = std::move(listed),
                 folders = std::move(folders)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QStringLiteral("Create failed."));
                        return;
                    }
                    catalog_path_ = qstring_from_utf8(path);
                    thumbnail_requests_.clear();
                    emit catalogChanged();
                    setError({});
                    setStatus(QStringLiteral("Library created. Import photos or a folder."));
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), true);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::openCatalog(const QUrl &file_url)
{
    if (busy_)
    {
        return;
    }
    const QString local = file_url.toLocalFile();
    if (local.isEmpty())
    {
        setError(QStringLiteral("Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Opening library…"));
    const auto path = utf8_from_qstring(local);
    executor_.post(
        [this, path]()
        {
            QString failure;
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            auto built = make_catalog_service(path, false);
            if (!built)
            {
                failure = qstring_from_utf8(built.error().message);
            }
            else
            {
                listed = built.value()->list_assets(query_);
                if (!listed)
                {
                    failure = qstring_from_utf8(listed.error().message);
                }
                else
                {
                    folders = built.value()->list_folders();
                    if (!folders)
                    {
                        failure = qstring_from_utf8(folders.error().message);
                    }
                    else
                    {
                        service_ = std::move(built).value();
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, failure = std::move(failure), listed = std::move(listed),
                 folders = std::move(folders)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QStringLiteral("Open failed."));
                        return;
                    }
                    catalog_path_ = qstring_from_utf8(path);
                    selected_asset_id_.clear();
                    clear_displayed_preview();
                    thumbnail_requests_.clear();
                    emit catalogChanged();
                    emit selectionChanged();
                    emit previewChanged();
                    setError({});
                    setStatus(QStringLiteral("Library opened."));
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), true);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::importFolder(const QUrl &folder_url)
{
    importFiles(QList<QUrl>{folder_url});
}

void StudioPresenter::createCatalogFromPath(const QString &path)
{
    createCatalog(url_from_dialog_path(path));
}

void StudioPresenter::openCatalogFromPath(const QString &path)
{
    openCatalog(url_from_dialog_path(path));
}

void StudioPresenter::importFilePaths(const QStringList &paths)
{
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const auto &path : paths)
    {
        const QUrl url = url_from_dialog_path(path);
        if (url.isValid() && !url.isEmpty())
        {
            urls.push_back(url);
        }
    }
    importFiles(urls);
}

void StudioPresenter::importFolderFromPath(const QString &path)
{
    importFolder(url_from_dialog_path(path));
}

void StudioPresenter::exportSelectedToPath(const QString &path, const QString &filter)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
    {
        return;
    }
    QString output = path.trimmed();
    if (output.startsWith(QStringLiteral("file:")))
    {
        output = QUrl(output).toLocalFile();
    }
    if (output.isEmpty())
    {
        setError(QStringLiteral("Export path must not be empty."));
        return;
    }
    auto format = export_format_from_ui(output, filter);
    if (!format)
    {
        setError(qstring_from_utf8(format.error().message));
        return;
    }
    if (QFileInfo(output).suffix().isEmpty() && format.value() != ExportFormat::kOriginalCopy)
    {
        output += QString::fromUtf8(
            export_format_extension(format.value()).data(),
            static_cast<qsizetype>(export_format_extension(format.value()).size()));
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Exporting…"));
    executor_.post(
        [this, asset_id = utf8_from_qstring(selected_asset_id_),
         output_path = utf8_from_qstring(output), export_format = format.value()]()
        {
            Result<ExportResult> exported = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                ExportRequest request;
                request.asset_id = asset_id;
                request.output_path = output_path;
                request.format = export_format;
                request.cancellation = shutdown_.token();
                exported = service_->export_asset(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported)]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        setStatus(QStringLiteral("Export failed."));
                        return;
                    }
                    setStatus(QStringLiteral("Exported %1 (%2×%3)")
                                  .arg(QFileInfo(qstring_from_utf8(exported.value().output_path))
                                           .fileName())
                                  .arg(exported.value().width)
                                  .arg(exported.value().height));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::importFiles(const QList<QUrl> &files)
{
    if (busy_ || catalog_path_.isEmpty())
    {
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto &file : files)
    {
        const QString local = file.toLocalFile();
        if (!local.isEmpty())
        {
            paths.push_back(utf8_from_qstring(local));
        }
    }
    if (paths.empty())
    {
        setError(QStringLiteral("No local files selected."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Importing…"));
    executor_.post(
        [this, paths = std::move(paths), query = current_query()]()
        {
            Result<std::vector<ImportItemResult>> imported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                imported = service_->import_inputs(
                    paths, shutdown_.token(),
                    [this](const std::size_t completed, const std::size_t total)
                    {
                        const auto completed_count = static_cast<int>(completed);
                        const auto total_count = static_cast<int>(total);
                        QMetaObject::invokeMethod(
                            this,
                            [this, completed_count, total_count]()
                            {
                                setStatus(QStringLiteral("Importing %1 / %2…")
                                              .arg(completed_count)
                                              .arg(total_count));
                            },
                            Qt::QueuedConnection);
                    });
            }
            std::vector<ImportItemResult> results;
            QString first_error;
            if (!imported)
            {
                first_error = qstring_from_utf8(imported.error().message);
            }
            else
            {
                results = std::move(imported).value();
                for (const auto &item : results)
                {
                    if (first_error.isEmpty() && item.error)
                    {
                        first_error = qstring_from_utf8(item.error->message);
                    }
                }
            }
            Result<std::vector<AssetRecord>> listed = std::vector<AssetRecord>{};
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                listed = service_->list_assets(query);
                folders = service_->list_folders();
            }
            QMetaObject::invokeMethod(
                this,
                [this, results = std::move(results), listed = std::move(listed),
                 folders = std::move(folders), first_error = std::move(first_error)]() mutable
                {
                    setBusy(false);
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        setStatus(QStringLiteral("Import failed."));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        setStatus(QStringLiteral("Import failed."));
                        return;
                    }
                    setError(first_error);
                    setStatus(describe_import(results));
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), true);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::publish_selection()
{
    assets_.setSelectedIds(selected_ids_);
    emit selectionChanged();
}

void StudioPresenter::activate_primary(const QString &asset_id, const bool reload_preview)
{
    const bool same = selected_asset_id_ == asset_id;
    selected_asset_id_ = asset_id;
    if (!reload_preview && same && !preview_url_.isEmpty())
    {
        publish_selection();
        return;
    }
    clear_displayed_preview();
    preview_loading_ = !asset_id.isEmpty();
    before_after_ = false;
    crop_tool_active_ = false;
    pending_preview_.reset();
    load_develop_for_selection();
    publish_selection();
    emit previewChanged();
    emit editChanged();
    requestPreviewForSelection();
}

std::vector<std::string> StudioPresenter::selected_asset_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(selected_ids_.size());
    for (int row = 0; row < assets_.rowCount(); ++row)
    {
        const auto id = utf8_from_qstring(assets_.assetIdAt(row));
        if (selected_ids_.contains(id))
        {
            ids.push_back(id);
        }
    }
    return ids;
}

void StudioPresenter::selectAsset(const QString &asset_id)
{
    if (selected_asset_id_ == asset_id && selected_ids_.size() == 1U && !preview_url_.isEmpty())
    {
        return;
    }
    selected_ids_.clear();
    if (!asset_id.isEmpty())
    {
        selected_ids_.insert(utf8_from_qstring(asset_id));
    }
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectAssetRange(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const int clicked = assets_.indexOf(asset_id);
    if (clicked < 0)
    {
        return;
    }
    int anchor = assets_.indexOf(selection_anchor_id_);
    if (anchor < 0)
    {
        anchor = assets_.indexOf(selected_asset_id_);
    }
    if (anchor < 0)
    {
        selectAsset(asset_id);
        return;
    }
    const int begin = std::min(anchor, clicked);
    const int end = std::max(anchor, clicked);
    selected_ids_.clear();
    for (int row = begin; row <= end; ++row)
    {
        selected_ids_.insert(utf8_from_qstring(assets_.assetIdAt(row)));
    }
    activate_primary(asset_id, true);
}

void StudioPresenter::toggleAssetSelected(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    if (selected_ids_.contains(id))
    {
        selected_ids_.erase(id);
        selection_anchor_id_ = asset_id;
        if (selected_asset_id_ == asset_id)
        {
            const auto remaining = selected_asset_ids();
            activate_primary(remaining.empty() ? QString{} : qstring_from_utf8(remaining.back()),
                             true);
            return;
        }
        publish_selection();
        return;
    }
    selected_ids_.insert(id);
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectNext()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row < 0 || row + 1 >= assets_.rowCount())
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row + 1));
}

void StudioPresenter::selectPrevious()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row <= 0)
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row - 1));
}

void StudioPresenter::setBrowseMode(const QString &mode)
{
    QString normalized = QStringLiteral("grid");
    if (mode == QStringLiteral("loupe"))
    {
        normalized = QStringLiteral("loupe");
    }
    else if (mode == QStringLiteral("develop"))
    {
        normalized = QStringLiteral("develop");
    }
    if (browse_mode_ == normalized)
    {
        return;
    }
    browse_mode_ = normalized;
    emit browseModeChanged();
}

void StudioPresenter::openLoupe()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("loupe"));
}

void StudioPresenter::openDevelop()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("develop"));
}

void StudioPresenter::returnToGrid()
{
    setBrowseMode(QStringLiteral("grid"));
}

void StudioPresenter::setZoomMode(const QString &mode)
{
    QString normalized = QStringLiteral("fit");
    double factor = zoom_factor_;
    if (mode == QStringLiteral("fill"))
    {
        normalized = QStringLiteral("fill");
    }
    else if (mode == QStringLiteral("actual") || mode == QStringLiteral("100"))
    {
        normalized = QStringLiteral("actual");
        factor = 1.0;
    }
    else if (mode == QStringLiteral("custom"))
    {
        normalized = QStringLiteral("custom");
    }
    if (zoom_mode_ == normalized && zoom_factor_ == factor)
    {
        return;
    }
    zoom_mode_ = normalized;
    zoom_factor_ = factor;
    emit zoomChanged();
}

void StudioPresenter::setZoomFactor(const double factor)
{
    const double clamped = std::clamp(factor, 0.1, 8.0);
    if (zoom_mode_ == QStringLiteral("custom") && zoom_factor_ == clamped)
    {
        return;
    }
    zoom_mode_ = QStringLiteral("custom");
    zoom_factor_ = clamped;
    emit zoomChanged();
}

void StudioPresenter::adjustZoom(const int wheel_delta)
{
    const double step = wheel_delta > 0 ? 1.1 : 1.0 / 1.1;
    const double current = zoom_mode_ == QStringLiteral("actual") ? 1.0 : zoom_factor_;
    setZoomFactor(current * step);
}

void StudioPresenter::setThumbnailSize(const int size)
{
    const int clamped = std::clamp(size, 96, 320);
    if (thumbnail_size_ == clamped)
    {
        return;
    }
    thumbnail_size_ = clamped;
    emit thumbnailSizeChanged();
}

void StudioPresenter::mutate_selected_review(
    const std::function<Result<AssetRecord>(CatalogService &, std::string_view)> &action)
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    executor_.post(
        [this, action, ids]()
        {
            std::vector<AssetRecord> updated;
            TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool ok = false;
            if (service_ != nullptr)
            {
                ok = true;
                for (const auto &asset_id : ids)
                {
                    auto result = action(*service_, asset_id);
                    if (!result)
                    {
                        error = result.error();
                        ok = false;
                        break;
                    }
                    updated.push_back(std::move(result).value());
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, error = std::move(error), updated = std::move(updated)]() mutable
                {
                    if (!ok)
                    {
                        setError(qstring_from_utf8(error.message));
                        return;
                    }
                    for (const auto &asset : updated)
                    {
                        assets_.updateAsset(asset);
                    }
                    emit selectionChanged();
                    if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setRating(const int rating)
{
    mutate_selected_review([rating](CatalogService &service, const std::string_view asset_id)
                           { return service.set_rating(asset_id, rating); });
}

void StudioPresenter::setColorLabel(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    const auto color = parsed.value();
    mutate_selected_review([color](CatalogService &service, const std::string_view asset_id)
                           { return service.set_color_label(asset_id, color); });
}

void StudioPresenter::toggleRejected()
{
    const bool next = !selectedRejected();
    mutate_selected_review([next](CatalogService &service, const std::string_view asset_id)
                           { return service.set_rejected(asset_id, next); });
}

void StudioPresenter::setAssetTags(const QString &text)
{
    auto parsed = parse_tag_list(utf8_from_qstring(text));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    const auto tags = parsed.value();
    mutate_selected_review([tags](CatalogService &service, const std::string_view asset_id)
                           { return service.set_tags(asset_id, tags); });
}

void StudioPresenter::setMetadataField(const QString &name, const QString &value)
{
    const auto field = utf8_from_qstring(name);
    const auto text = utf8_from_qstring(value);
    mutate_selected_review(
        [field, text](CatalogService &service, const std::string_view asset_id) -> Result<AssetRecord>
        {
            auto listed = service.list_assets();
            if (!listed)
            {
                return listed.error();
            }
            WritableMetadata metadata;
            for (const auto &asset : listed.value())
            {
                if (asset.id == asset_id)
                {
                    metadata = asset.metadata;
                    break;
                }
            }
            if (field == "title")
            {
                metadata.title = text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "description")
            {
                metadata.description =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "creator")
            {
                metadata.creator =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "copyright")
            {
                metadata.copyright =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else
            {
                return make_error(ErrorCode::kInvalidArgument, "Writable metadata field is unknown",
                                  {{"field", field}});
            }
            return service.set_writable_metadata(asset_id, metadata);
        });
}

void StudioPresenter::createSnapshot(const QString &label)
{
    const auto text = utf8_from_qstring(label);
    mutate_selected_review([text](CatalogService &service, const std::string_view asset_id)
                           { return service.create_recipe_snapshot(asset_id, text); });
    load_develop_for_selection();
}

void StudioPresenter::restoreHistory(const int history_id)
{
    mutate_selected_review(
        [history_id](CatalogService &service, const std::string_view asset_id)
        { return service.restore_recipe_history(asset_id, history_id); });
    load_develop_for_selection();
}

void StudioPresenter::setTagFilter(const QString &tag)
{
    auto parsed = tag.trimmed().isEmpty() ? Result<std::string>{std::string{}} :
                                            normalize_tag_name(utf8_from_qstring(tag));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    if (query_.tag == parsed.value())
    {
        return;
    }
    query_.tag = parsed.value();
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRatingFilter(const QString &mode, const int value)
{
    RatingFilterMode next_mode = RatingFilterMode::kAny;
    if (mode == QStringLiteral("min"))
    {
        next_mode = RatingFilterMode::kMinimum;
    }
    else if (mode == QStringLiteral("exact"))
    {
        next_mode = RatingFilterMode::kExact;
    }
    if (query_.rating_mode == next_mode && query_.rating_value == value)
    {
        return;
    }
    query_.rating_mode = next_mode;
    query_.rating_value = value;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::toggleColorFilter(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    auto &labels = query_.color_labels;
    const auto found = std::find(labels.begin(), labels.end(), parsed.value());
    if (found == labels.end())
    {
        labels.push_back(parsed.value());
    }
    else
    {
        labels.erase(found);
    }
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRejectFilter(const QString &mode)
{
    RejectFilter next = RejectFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = RejectFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = RejectFilter::kOnly;
    }
    if (query_.reject_filter == next)
    {
        return;
    }
    query_.reject_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setSort(const QString &field, const QString &direction)
{
    AssetSortField next_field = AssetSortField::kImportTime;
    if (field == QStringLiteral("name"))
    {
        next_field = AssetSortField::kDisplayName;
    }
    else if (field == QStringLiteral("rating"))
    {
        next_field = AssetSortField::kRating;
    }
    const auto next_direction =
        direction == QStringLiteral("asc") ? SortDirection::kAscending : SortDirection::kDescending;
    if (query_.sort_field == next_field && query_.sort_direction == next_direction)
    {
        return;
    }
    query_.sort_field = next_field;
    query_.sort_direction = next_direction;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::clearFilters()
{
    if (!filtersActive())
    {
        return;
    }
    query_.rating_mode = RatingFilterMode::kAny;
    query_.rating_value = 0;
    query_.color_labels.clear();
    query_.reject_filter = RejectFilter::kInclude;
    query_.tag.clear();
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto existing = assets_.assetById(asset_id);
    if (!existing)
    {
        return;
    }
    const auto revision = ++thumbnail_revision_;
    const auto id = utf8_from_qstring(asset_id);
    thumbnail_requests_[id] = revision;
    executor_.post(
        [this, id, revision]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kThumbnailMaxEdge;
                request.request_revision = revision;
                request.cancellation = shutdown_.token();
                preview = service_->request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = thumbnail_requests_.find(id);
                    if (latest == thumbnail_requests_.end() || latest->second != revision)
                    {
                        return;
                    }
                    if (catalog_path_.isEmpty() || !assets_.assetById(qstring_from_utf8(id)))
                    {
                        return;
                    }
                    if (preview)
                    {
                        assets_.setThumbnail(
                            id, QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path)),
                            preview.value().original_missing ? QStringLiteral("missing") :
                                                               QStringLiteral("ready"));
                        if (utf8_from_qstring(selected_asset_id_) == id)
                        {
                            emit selectionChanged();
                        }
                        if (preview.value().original_missing)
                        {
                            assets_.markOriginalMissing(id);
                        }
                        return;
                    }
                    if (preview.error().code == ErrorCode::kNotFound)
                    {
                        assets_.markOriginalMissing(id);
                        assets_.setThumbnail(id, {}, QStringLiteral("missing"));
                        if (selected_ids_.contains(id))
                        {
                            emit selectionChanged();
                        }
                        return;
                    }
                    assets_.setThumbnail(id, {}, QStringLiteral("failed"));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::executeCommand(const QString &id, const QVariant &argument)
{
    using namespace command_id;
    static const QStringList kWindowCommands{
        QLatin1String(kLibraryCreate),      QLatin1String(kLibraryOpen),
        QLatin1String(kLibraryImportFiles), QLatin1String(kLibraryImportFolder),
        QLatin1String(kLibraryExport),      QLatin1String(kWindowSettings),
        QLatin1String(kWindowClose),        QLatin1String(kWindowQuit),
        QLatin1String(kWindowAbout),
    };
    if (kWindowCommands.contains(id))
    {
        emit uiCommandRequested(id);
        return;
    }

    if (id == QLatin1String(kLibraryExportWrite))
    {
        const auto fields = argument.toMap();
        exportSelectedToPath(fields.value(QStringLiteral("path"), argument.toString()).toString(),
                             fields.value(QStringLiteral("filter")).toString());
    }
    else if (id == QLatin1String(kPhotoSelect))
    {
        QString asset_id = argument.toString();
        QString mode = QStringLiteral("single");
        const auto fields = argument.toMap();
        if (!fields.isEmpty())
        {
            asset_id = fields.value(QStringLiteral("id")).toString();
            mode = fields.value(QStringLiteral("mode"), QStringLiteral("single")).toString();
        }
        if (mode == QStringLiteral("range"))
        {
            selectAssetRange(asset_id);
        }
        else if (mode == QStringLiteral("toggle"))
        {
            toggleAssetSelected(asset_id);
        }
        else
        {
            selectAsset(asset_id);
        }
    }
    else if (id == QLatin1String(kPhotoRate))
    {
        setRating(argument.toInt());
    }
    else if (id == QLatin1String(kPhotoColor))
    {
        setColorLabel(argument.toString());
    }
    else if (id == QLatin1String(kPhotoReject))
    {
        toggleRejected();
    }
    else if (id == QLatin1String(kPhotoRemove))
    {
        remove_selected_from_catalog();
    }
    else if (id == QLatin1String(kPhotoRemoveFromDisk))
    {
        remove_selected_from_disk();
    }
    else if (id == QLatin1String(kPhotoPrevious))
    {
        if (argument.toString() == QStringLiteral("range") && !selected_asset_id_.isEmpty())
        {
            const auto row = assets_.indexOf(selected_asset_id_);
            if (row > 0)
            {
                selectAssetRange(assets_.assetIdAt(row - 1));
            }
        }
        else
        {
            selectPrevious();
        }
    }
    else if (id == QLatin1String(kPhotoNext))
    {
        if (argument.toString() == QStringLiteral("range") && !selected_asset_id_.isEmpty())
        {
            const auto row = assets_.indexOf(selected_asset_id_);
            if (row >= 0 && row + 1 < assets_.rowCount())
            {
                selectAssetRange(assets_.assetIdAt(row + 1));
            }
        }
        else
        {
            selectNext();
        }
    }
    else if (id == QLatin1String(kViewGrid))
    {
        returnToGrid();
    }
    else if (id == QLatin1String(kViewLoupe))
    {
        openLoupe();
    }
    else if (id == QLatin1String(kViewDevelop))
    {
        openDevelop();
    }
    else if (id == QLatin1String(kViewFit))
    {
        setZoomMode(QStringLiteral("fit"));
    }
    else if (id == QLatin1String(kViewFill))
    {
        setZoomMode(QStringLiteral("fill"));
    }
    else if (id == QLatin1String(kViewActual))
    {
        setZoomMode(QStringLiteral("actual"));
    }
    else if (id == QLatin1String(kEditUndo))
    {
        undoEdit();
    }
    else if (id == QLatin1String(kEditRedo))
    {
        redoEdit();
    }
    else if (id == QLatin1String(kEditResetAll))
    {
        resetAllEdits();
    }
    else if (id == QLatin1String(kEditResetSection))
    {
        resetSection(argument.toString());
    }
    else if (id == QLatin1String(kEditResetControl))
    {
        resetControl(argument.toString());
    }
    else if (id == QLatin1String(kEditSetNumber))
    {
        const auto fields = argument.toMap();
        const auto name = fields.value(QStringLiteral("name")).toString();
        const auto value = fields.value(QStringLiteral("value")).toDouble();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewDevelopNumber(name, value);
        }
        else
        {
            setDevelopNumber(name, value);
        }
    }
    else if (id == QLatin1String(kEditSetToneCurve))
    {
        const auto fields = argument.toMap();
        const auto points = fields.value(QStringLiteral("points")).toList();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewToneCurve(points);
        }
        else
        {
            setToneCurve(points);
        }
    }
    else if (id == QLatin1String(kEditSetCrop))
    {
        const auto fields = argument.toMap();
        const auto x = fields.value(QStringLiteral("x")).toDouble();
        const auto y = fields.value(QStringLiteral("y")).toDouble();
        const auto width = fields.value(QStringLiteral("width")).toDouble();
        const auto height = fields.value(QStringLiteral("height")).toDouble();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewCropRect(x, y, width, height);
        }
        else
        {
            setCropRect(x, y, width, height);
        }
    }
    else if (id == QLatin1String(kEditSetCropAspect))
    {
        setCropAspect(argument.toString());
    }
    else if (id == QLatin1String(kEditRotateLeft))
    {
        rotateLeft();
    }
    else if (id == QLatin1String(kEditRotateRight))
    {
        rotateRight();
    }
    else if (id == QLatin1String(kEditFlipHorizontal))
    {
        flipHorizontal();
    }
    else if (id == QLatin1String(kEditFlipVertical))
    {
        flipVertical();
    }
    else if (id == QLatin1String(kEditCropTool))
    {
        setCropToolActive(argument.isValid() ? argument.toBool() : !crop_tool_active_);
    }
    else if (id == QLatin1String(kEditBeforeAfter))
    {
        toggleBeforeAfter();
    }
    else if (id == QLatin1String(kPhotoSetTags))
    {
        setAssetTags(argument.toString());
    }
    else if (id == QLatin1String(kPhotoSetMetadata))
    {
        const auto fields = argument.toMap();
        setMetadataField(fields.value(QStringLiteral("name")).toString(),
                         fields.value(QStringLiteral("value")).toString());
    }
    else if (id == QLatin1String(kPhotoCreateSnapshot))
    {
        createSnapshot(argument.toString());
    }
    else if (id == QLatin1String(kPhotoRestoreHistory))
    {
        restoreHistory(argument.toInt());
    }
    else if (id == QLatin1String(kLibrarySetTagFilter))
    {
        setTagFilter(argument.toString());
    }
    else
    {
        setError(QStringLiteral("Unknown command: %1").arg(id));
    }
}

void StudioPresenter::remove_selected_from_catalog()
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_from_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), false);
                    if (assets_.rowCount() == 0)
                    {
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                        clear_displayed_preview();
                        preview_loading_ = false;
                        emit selectionChanged();
                        emit previewChanged();
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(
                        count == 1 ?
                            QStringLiteral("Removed from catalog. Original file was not deleted.") :
                            QStringLiteral(
                                "Removed %1 photos from catalog. Original files were not deleted.")
                                .arg(count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::remove_selected_from_disk()
{
    if (!canDeleteFromDisk() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_original_and_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), false);
                    if (assets_.rowCount() == 0)
                    {
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                        clear_displayed_preview();
                        preview_loading_ = false;
                        emit selectionChanged();
                        emit previewChanged();
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(count == 1 ?
                                  QStringLiteral("Deleted original file and catalog record.") :
                                  QStringLiteral("Deleted %1 original files and catalog records.")
                                      .arg(count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::load_develop_for_selection()
{
    develop_ = {};
    saved_develop_ = {};
    undo_stack_.clear();
    redo_stack_.clear();
    recipe_history_.clear();
    if (selected_asset_id_.isEmpty())
    {
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<Recipe> loaded = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                loaded = service_->load_recipe(asset_id);
                history = service_->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, loaded = std::move(loaded),
                 history = std::move(history)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    recipe_history_.clear();
                    if (history)
                    {
                        for (const auto &entry : history.value())
                        {
                            QVariantMap row;
                            row.insert(QStringLiteral("id"), QVariant::fromValue(entry.id));
                            row.insert(QStringLiteral("kind"), qstring_from_utf8(entry.kind));
                            row.insert(QStringLiteral("label"),
                                       entry.label ? qstring_from_utf8(*entry.label) : QString{});
                            row.insert(QStringLiteral("seq"), QVariant::fromValue(entry.seq));
                            recipe_history_.push_back(row);
                        }
                    }
                    if (!loaded)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        emit editChanged();
                        setError(qstring_from_utf8(loaded.error().message));
                        return;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        emit editChanged();
                        setError(qstring_from_utf8(params.error().message));
                        return;
                    }
                    develop_ = params.value();
                    saved_develop_ = develop_;
                    emit editChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::commit_develop(DevelopParams params, const bool push_history,
                                     const bool refresh_preview)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    clamp_develop(params);
    const auto previous = saved_develop_;
    if (push_history && params != saved_develop_)
    {
        undo_stack_.push_back(saved_develop_);
        if (undo_stack_.size() > 40U)
        {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    develop_ = params;
    emit editChanged();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    preview_loading_ = refresh_preview;
    emit previewChanged();
    ++preview_revision_;
    pending_save_ = PendingDevelopWork{
        .save = true,
        .params = params,
        .previous = previous,
        .push_history = push_history,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
        .refresh_preview = refresh_preview,
    };
    pending_preview_.reset();
    kick_develop_work();
}

void StudioPresenter::preview_develop(DevelopParams params)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    clamp_develop(params);
    if (params == develop_)
    {
        return;
    }
    develop_ = params;
    emit editChanged();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    ++preview_revision_;
    pending_preview_ = PendingDevelopWork{
        .interactive = true,
        .params = params,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
    };
    kick_develop_work();
}

void StudioPresenter::enqueue_preview()
{
    if (selected_asset_id_.isEmpty())
    {
        preview_loading_ = false;
        emit previewChanged();
        return;
    }
    preview_loading_ = true;
    emit previewChanged();
    ++preview_revision_;
    const bool crop_guides = crop_tool_active_ && !before_after_;
    pending_preview_ = PendingDevelopWork{
        .params = develop_,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
    };
    kick_develop_work();
}

void StudioPresenter::kick_develop_work()
{
    if (develop_job_in_flight_)
    {
        return;
    }
    PendingDevelopWork job;
    if (pending_save_.has_value())
    {
        job = *pending_save_;
        pending_save_.reset();
    }
    else if (pending_preview_.has_value())
    {
        job = *pending_preview_;
        pending_preview_.reset();
    }
    else
    {
        return;
    }
    develop_job_in_flight_ = true;
    const auto revision = preview_revision_;
    executor_.post(
        [this, job, revision]()
        {
            Result<AssetRecord> saved = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool save_ok = !job.save;
            if (service_ != nullptr)
            {
                if (job.save)
                {
                    saved = service_->save_develop(job.asset_id, job.params);
                    save_ok = static_cast<bool>(saved);
                }
                if (save_ok && job.refresh_preview)
                {
                    PreviewRequest request;
                    request.asset_id = job.asset_id;
                    request.max_edge =
                        job.interactive ? kInteractivePreviewMaxEdge : kDefaultPreviewMaxEdge;
                    request.request_revision = revision;
                    request.ignore_edits = job.ignore_edits;
                    request.ignore_crop = job.ignore_crop;
                    request.ignore_straighten = job.ignore_straighten;
                    request.persist_preview_record = !job.interactive;
                    request.cancellation = shutdown_.token();
                    preview = service_->request_preview(
                        request, job.interactive ? std::optional<DevelopParams>{job.params} :
                                                   std::optional<DevelopParams>{});
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, job, revision, saved = std::move(saved),
                 preview = std::move(preview)]() mutable
                {
                    develop_job_in_flight_ = false;
                    const bool selected_matches =
                        utf8_from_qstring(selected_asset_id_) == job.asset_id;
                    if (job.save)
                    {
                        if (!saved)
                        {
                            if (selected_matches && !pending_save_.has_value() &&
                                develop_ == job.params)
                            {
                                develop_ = job.previous;
                                saved_develop_ = job.previous;
                                if (job.push_history && !undo_stack_.empty())
                                {
                                    undo_stack_.pop_back();
                                }
                                preview_loading_ = false;
                                emit editChanged();
                                emit previewChanged();
                            }
                            setError(qstring_from_utf8(saved.error().message));
                            kick_develop_work();
                            return;
                        }
                        if (selected_matches)
                        {
                            saved_develop_ = job.params;
                            assets_.updateAsset(saved.value());
                            emit selectionChanged();
                            emit editChanged();
                        }
                    }
                    if (revision != preview_revision_ || !selected_matches)
                    {
                        kick_develop_work();
                        return;
                    }
                    if (job.save && !job.refresh_preview)
                    {
                        preview_loading_ = false;
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    preview_loading_ = false;
                    if (!preview)
                    {
                        if (preview.error().code == ErrorCode::kCancelled)
                        {
                            kick_develop_work();
                            return;
                        }
                        if (preview.error().code == ErrorCode::kNotFound)
                        {
                            assets_.markOriginalMissing(job.asset_id);
                            emit selectionChanged();
                        }
                        else
                        {
                            setError(qstring_from_utf8(preview.error().message));
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    if (preview.value().original_missing)
                    {
                        assets_.markOriginalMissing(job.asset_id);
                        emit selectionChanged();
                    }
                    if (job.ignore_crop && job.ignore_straighten && crop_tool_active_)
                    {
                        crop_guide_ready_ = true;
                    }
                    show_preview_result(preview.value(), revision);
                    emit previewChanged();
                    kick_develop_work();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = develop_;
    if (!apply_develop_field(next, utf8_from_qstring(name), value))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
    }
    clamp_develop(next);
    if (next == saved_develop_ && next == develop_)
    {
        return;
    }
    if (next == saved_develop_)
    {
        develop_ = next;
        emit editChanged();
        enqueue_preview();
        return;
    }
    const bool keep_crop_guide =
        crop_tool_active_ && crop_guide_ready_ && name == QLatin1String("straighten");
    commit_develop(next, true, !keep_crop_guide);
}

void StudioPresenter::setToneCurve(const QVariantList &points)
{
    DevelopParams next = develop_;
    next.tone_curve = tone_curve_from_variant(points);
    clamp_develop(next);
    if (next == saved_develop_ && next == develop_)
    {
        return;
    }
    if (next == saved_develop_)
    {
        develop_ = next;
        emit editChanged();
        enqueue_preview();
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::previewToneCurve(const QVariantList &points)
{
    DevelopParams next = develop_;
    next.tone_curve = tone_curve_from_variant(points);
    preview_develop(next);
}

void StudioPresenter::previewDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = develop_;
    if (!apply_develop_field(next, utf8_from_qstring(name), value))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
        if (crop_tool_active_)
        {
            clamp_develop(next);
            if (next == develop_)
            {
                return;
            }
            develop_ = next;
            emit editChanged();
            return;
        }
    }
    preview_develop(next);
}

void StudioPresenter::setCropRect(const double x, const double y, const double width,
                                  const double height)
{
    DevelopParams next = develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    if (next == develop_)
    {
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::previewCropRect(const double x, const double y, const double width,
                                      const double height)
{
    DevelopParams next = develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    if (next == develop_)
    {
        return;
    }
    develop_ = next;
    emit editChanged();
}

void StudioPresenter::setCropAspect(const QString &aspect)
{
    DevelopParams next = develop_;
    if (!apply_crop_aspect(next, utf8_from_qstring(aspect)))
    {
        return;
    }
    crop_aspect_ = aspect;
    fit_geometry_crop(next);
    if (next == develop_)
    {
        emit editChanged();
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::rotateLeft()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 3) % 4;
    transform_crop_for_quarter_turns(next, 3);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::rotateRight()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 1) % 4;
    transform_crop_for_quarter_turns(next, 1);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::flipHorizontal()
{
    DevelopParams next = develop_;
    next.flip_horizontal = next.flip_horizontal == 0 ? 1 : 0;
    transform_crop_for_flip(next, true, false);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::flipVertical()
{
    DevelopParams next = develop_;
    next.flip_vertical = next.flip_vertical == 0 ? 1 : 0;
    transform_crop_for_flip(next, false, true);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::setCropToolActive(const bool active)
{
    if (crop_tool_active_ == active)
    {
        return;
    }
    crop_tool_active_ = active;
    if (active)
    {
        setZoomMode(QStringLiteral("fit"));
        DevelopParams next = develop_;
        fit_geometry_crop(next);
        crop_guide_ready_ = std::abs(next.straighten_degrees) < 1e-4 && next.crop_width >= 0.999 &&
                            next.crop_height >= 0.999 && std::abs(next.crop_x) < 1e-6 &&
                            std::abs(next.crop_y) < 1e-6;
        if (next != develop_)
        {
            emit editChanged();
            emit previewChanged();
            commit_develop(next, true);
            return;
        }
    }
    else
    {
        crop_guide_ready_ = false;
    }
    emit editChanged();
    emit previewChanged();
    enqueue_preview();
}

void StudioPresenter::resetControl(const QString &name)
{
    DevelopParams next = develop_;
    if (!reset_develop_field(next, utf8_from_qstring(name)))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
    }
    commit_develop(next, true);
}

void StudioPresenter::resetSection(const QString &section)
{
    DevelopParams next = develop_;
    if (!reset_develop_section(next, utf8_from_qstring(section)))
    {
        return;
    }
    if (section == QLatin1String("geometry"))
    {
        crop_aspect_ = QStringLiteral("free");
    }
    commit_develop(next, true);
}

void StudioPresenter::resetAllEdits()
{
    crop_aspect_ = QStringLiteral("free");
    DevelopParams reset;
    reset.sigmoid_enabled = develop_.sigmoid_enabled;
    commit_develop(reset, true);
}

void StudioPresenter::undoEdit()
{
    if (undo_stack_.empty())
    {
        return;
    }
    redo_stack_.push_back(develop_);
    const auto previous = undo_stack_.back();
    undo_stack_.pop_back();
    commit_develop(previous, false);
}

void StudioPresenter::redoEdit()
{
    if (redo_stack_.empty())
    {
        return;
    }
    undo_stack_.push_back(develop_);
    const auto next = redo_stack_.back();
    redo_stack_.pop_back();
    commit_develop(next, false);
}

void StudioPresenter::toggleBeforeAfter()
{
    before_after_ = !before_after_;
    emit editChanged();
    enqueue_preview();
}

void StudioPresenter::requestPreviewForSelection()
{
    enqueue_preview();
}

double StudioPresenter::selected_source_aspect() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return static_cast<double>(*asset->width) / static_cast<double>(*asset->height);
    }
    return 1.5;
}

double StudioPresenter::selected_working_aspect() const
{
    if (crop_tool_active_)
    {
        const QMutexLocker lock(&preview_image_mutex_);
        if (!preview_image_.isNull() && preview_image_.height() > 0)
        {
            return static_cast<double>(preview_image_.width()) /
                   static_cast<double>(preview_image_.height());
        }
    }
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return working_image_aspect(develop_.rotate_quarters, selected_source_aspect());
    }
    const QMutexLocker lock(&preview_image_mutex_);
    if (!preview_image_.isNull() && preview_image_.height() > 0)
    {
        return static_cast<double>(preview_image_.width()) /
               static_cast<double>(preview_image_.height());
    }
    return working_image_aspect(develop_.rotate_quarters, selected_source_aspect());
}

void StudioPresenter::constrain_geometry_crop(DevelopParams &params) const
{
    constrain_crop_to_straighten(params, selected_working_aspect());
}

void StudioPresenter::fit_geometry_crop(DevelopParams &params) const
{
    fit_crop_to_straighten(params, selected_working_aspect());
}

} // namespace ravo
