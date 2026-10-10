#include "ravo/desktop/studio_presenter.h"

#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/services/display_presentation.h"
#include "ravo/adapters/filesystem_preview_cache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLockFile>
#include <QFile>
#include <QSaveFile>
#include <QUuid>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ravo/domain/types.h"
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#endif
#include "studio_qt.h"
#include "studio_preview_handoff.h"
#include "studio_gallery_thumbnail_cache.h"

namespace ravo
{

StudioGalleryThumbnailCache::StudioGalleryThumbnailCache(const std::uint64_t max_bytes)
    : max_bytes_(max_bytes)
{
}

StudioGalleryThumbnailCache::~StudioGalleryThumbnailCache() = default;

Result<QString> StudioGalleryThumbnailCache::publish(const QString &root, const QString &key,
                                                     const QByteArray &png, const bool replace,
                                                     const CancellationToken &cancellation)
{
    QLockFile lock(QDir(root).filePath(QStringLiteral("publish.lock")));
    QElapsedTimer waiting;
    waiting.start();
    while (true)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        if (lock.tryLock(0))
            break;
        if (lock.error() != QLockFile::LockFailedError)
            return make_error(ErrorCode::kIo, "Cannot lock gallery display cache");
        if (waiting.elapsed() >= 1000)
            return make_error(ErrorCode::kConflict, "Gallery display cache is busy");
        // QLockFile bounds each wait; cancellation is observed between slices.
        if (lock.tryLock(20))
            break;
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    const auto output = QDir(root).filePath(key + QStringLiteral(".png"));
    if (!replace)
    {
        QImageReader reader(output, "PNG");
        const auto size = reader.size();
        if (size.isValid() && size.width() <= static_cast<int>(kThumbnailMaxEdge) &&
            size.height() <= static_cast<int>(kThumbnailMaxEdge) && reader.canRead())
            return output;
    } // Close the header reader before eviction/replacement, including on Windows.

    const auto epoch_path = QDir(root).filePath(QStringLiteral("index-epoch"));
    QFile epoch_file(epoch_path);
    QByteArray observed;
    if (epoch_file.exists())
    {
        if (!epoch_file.open(QIODevice::ReadOnly) || epoch_file.size() > 64)
            return make_error(ErrorCode::kIo, "Cannot read gallery cache epoch");
        observed = epoch_file.readAll();
        if (epoch_file.error() != QFileDevice::NoError)
            return make_error(ErrorCode::kIo, "Cannot read gallery cache epoch");
        epoch_file.close();
    }
    const bool reuse = cache_ && cache_->root() == utf8_from_qstring(root) && epoch_ == observed;
    // Publish invalidation BEFORE any eviction/write. If the process exits, the
    // next lock holder rescans instead of trusting a partially updated index.
    const auto next_epoch = QUuid::createUuid().toByteArray();
    QSaveFile epoch_output(epoch_path);
    if (!epoch_output.open(QIODevice::WriteOnly) ||
        epoch_output.write(next_epoch) != next_epoch.size() || !epoch_output.commit())
        return make_error(ErrorCode::kIo, "Cannot publish gallery cache epoch");
    epoch_.clear();
    if (!reuse)
    {
        cache_.reset();
        auto built = FilesystemPreviewCache::create(utf8_from_qstring(root), max_bytes_);
        if (!built)
            return built.error();
        cache_ = std::move(built).value();
        ++index_build_count_;
    }
    auto removed = cache_->remove_png(utf8_from_qstring(key));
    if (!removed)
        return removed.error();
    if (auto active = cancellation.check(); !active)
        return active.error();
    auto committed = cache_->commit_png_bytes(utf8_from_qstring(key),
                                              std::vector<std::uint8_t>(png.cbegin(), png.cend()));
    if (!committed)
        return committed.error();
    epoch_ = next_epoch;
    return qstring_from_utf8(committed.value());
}

namespace
{

inline constexpr std::size_t kMaximumPendingThumbnailRequests = kLibraryPageDefaultSize * 3U;
// A small serial-worker pipeline avoids one GUI wakeup per warm cache hit while
// leaving most of the page reorderable by the current viewport.
inline constexpr std::size_t kThumbnailPresentationPipeline = 4U;

Result<QUrl> prepare_gallery_thumbnail(const QString &base_path,
                                       const DisplayPresentationState &display, const QString &root,
                                       const CancellationToken &cancellation,
                                       StudioGalleryThumbnailCache &cache, const bool replace)
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    const auto missing_source = []
    {
        return make_error(ErrorCode::kNotFound, "Gallery thumbnail source is missing",
                          {{"reason", "gallery_thumbnail_cache_missing"}});
    };
    const QFileInfo file(base_path);
    if (!file.isFile())
        return missing_source();
    // Content, including embedded ICC, is the source identity. Path/size/mtime
    // alone can alias a replaced preview with preserved filesystem metadata.
    constexpr qsizetype kMaximumSourceBytes = 64 * 1024 * 1024;
    QFile source(base_path);
    if (!source.open(QIODevice::ReadOnly))
        return QFileInfo(base_path).isFile() ?
                   Result<QUrl>(
                       make_error(ErrorCode::kIo, "Cannot read gallery thumbnail source")) :
                   Result<QUrl>(missing_source());
    if (source.size() > kMaximumSourceBytes)
        return make_error(ErrorCode::kValidation, "Gallery thumbnail source exceeds byte limit");
    QByteArray source_bytes;
    QCryptographicHash source_digest(QCryptographicHash::Sha256);
    while (!source.atEnd())
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        const auto chunk = source.read(1024 * 1024);
        if (source.error() != QFileDevice::NoError)
            return make_error(ErrorCode::kIo, "Cannot read gallery thumbnail source");
        if (chunk.size() > kMaximumSourceBytes - source_bytes.size())
            return make_error(ErrorCode::kValidation,
                              "Gallery thumbnail source exceeds byte limit");
        source_bytes.append(chunk);
        source_digest.addData(chunk);
    }
    source.close();
    if (auto active = cancellation.check(); !active)
        return active.error();
    const auto source_hash = source_digest.result().toHex();
    // The immutable preview PNG owns its embedded source profile. Runtime profile
    // objects are absent on catalog reopen and must not change the cache identity.
    QByteArray identity = QByteArray::number(kThumbnailMaxEdge) + '\n' + source_hash + '\n' +
                          QByteArray::fromStdString(display.contract_version) + '\n' +
                          file.absoluteFilePath().toUtf8() + '\n' +
                          QByteArray::number(file.size()) + '\n' +
                          QByteArray::number(file.lastModified().toMSecsSinceEpoch()) + '\n' +
                          QByteArray::fromStdString(display.profile_fingerprint);
    const QString key =
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    const QString output = QDir(root).filePath(key + QStringLiteral(".png"));
    // Header-only warm lookup: no source decode, ICC transform, PNG encode or
    // full directory scan. Publication below is atomic across Studio windows.
    const auto valid_cached_header = [&]
    {
        QImageReader cached(output, "PNG");
        const auto dimensions = cached.size();
        return dimensions.isValid() && dimensions.width() <= static_cast<int>(kThumbnailMaxEdge) &&
               dimensions.height() <= static_cast<int>(kThumbnailMaxEdge) && cached.canRead();
    };
    if (!replace && valid_cached_header())
        return QUrl::fromLocalFile(output);
    QBuffer encoded_source(&source_bytes);
    if (!encoded_source.open(QIODevice::ReadOnly))
        return make_error(ErrorCode::kIo, "Cannot open gallery thumbnail bytes");
    QImageReader reader(&encoded_source, "PNG");
    const auto size = reader.size();
    if (!size.isValid())
    {
        if (!QFileInfo(base_path).isFile())
            return missing_source();
        return make_error(ErrorCode::kIo, "Gallery thumbnail has invalid dimensions");
    }
    if (size.width() > static_cast<int>(kThumbnailMaxEdge) ||
        size.height() > static_cast<int>(kThumbnailMaxEdge))
        reader.setScaledSize(
            size.scaled(kThumbnailMaxEdge, kThumbnailMaxEdge, Qt::KeepAspectRatio));
    QImage base = reader.read().convertToFormat(QImage::Format_RGB888);
    if (base.isNull())
    {
        if (!QFileInfo(base_path).isFile())
            return missing_source();
        return make_error(ErrorCode::kIo, "Unable to decode gallery thumbnail");
    }
    ColorProfileState source_profile;
    const QByteArray source_icc = base.colorSpace().iccProfile();
    if (!source_icc.isEmpty())
    {
        source_profile.kind = ColorProfileKind::kIcc;
        source_profile.icc_bytes.assign(source_icc.cbegin(), source_icc.cend());
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    const auto width = static_cast<std::uint32_t>(base.width());
    const auto height = static_cast<std::uint32_t>(base.height());
    const auto row_bytes = static_cast<std::size_t>(width) * 3U;
    std::vector<std::uint8_t> pixels(row_bytes * height);
    for (int y = 0; y < base.height(); ++y)
        std::copy_n(base.constScanLine(y), row_bytes,
                    pixels.data() + static_cast<std::size_t>(y) * row_bytes);
    auto converted = apply_display_presentation_rgb8(pixels, width, height, source_profile, display,
                                                     cancellation);
    if (!converted)
        return converted.error();
    QImage presented(converted.value().rgb8.data(), base.width(), base.height(), base.width() * 3,
                     QImage::Format_RGB888);
    const auto &icc = converted.value().color_profile.icc_bytes;
    if (!icc.empty())
        presented.setColorSpace(QColorSpace::fromIccProfile(QByteArray(
            reinterpret_cast<const char *>(icc.data()), static_cast<qsizetype>(icc.size()))));
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!QDir().mkpath(root))
        return make_error(ErrorCode::kIo, "Unable to create gallery display cache");
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !presented.save(&buffer, "PNG"))
        return make_error(ErrorCode::kIo, "Unable to encode gallery display thumbnail");
    auto committed = cache.publish(root, key, encoded, replace, cancellation);
    if (!committed)
        return committed.error();
    return QUrl::fromLocalFile(committed.value());
}

} // namespace

void StudioPresenter::bindDisplayPresentation(StudioDisplayPresentation *owner)
{
    if (display_presentation_ == owner)
        return;
    if (display_presentation_ != nullptr)
        disconnect(display_presentation_, nullptr, this, nullptr);
    display_presentation_ = owner;
    if (display_presentation_ == nullptr)
        return;
    connect(display_presentation_, &StudioDisplayPresentation::stateChanged, this,
            &StudioPresenter::handle_display_presentation_changed);
    inspect_.bindDisplayPresentation(owner);
    reapply_display_presentation_to_cached_thumbnails();
}

void StudioPresenter::handle_display_presentation_changed()
{
    reapply_display_presentation_to_cached_thumbnails();
}

void StudioPresenter::clear_thumbnail_presentation_cache()
{
    static_cast<void>(thumbnail_presentation_cancel_.cancel("catalog_closed"));
    thumbnail_presentation_cancel_ = CancellationSource{};
    pending_thumbnail_presentations_.clear();
    thumbnail_presentation_order_.clear();
    thumbnail_presentation_revisions_.clear();
    thumbnail_repair_attempts_.clear();
    thumbnail_load_repair_attempts_.clear();
    thumbnail_bases_.clear();
    thumbnail_presented_root_.clear();
}

void StudioPresenter::startNextThumbnailPresentation()
{
    while (thumbnail_presentations_in_flight_ < kThumbnailPresentationPipeline &&
           !thumbnail_presentation_order_.empty())
    {
        auto id = std::move(thumbnail_presentation_order_.front());
        thumbnail_presentation_order_.pop_front();
        const auto next = pending_thumbnail_presentations_.find(id);
        if (next == pending_thumbnail_presentations_.end())
            continue;
        auto task = std::move(next->second);
        pending_thumbnail_presentations_.erase(next);
        if (!assets_.assetById(qstring_from_utf8(id)))
            continue;
        ++thumbnail_presentations_in_flight_;
        if (!thumbnail_presentation_executor_.post(std::move(task)))
        {
            --thumbnail_presentations_in_flight_;
            assets_.setThumbnail(id, {}, QStringLiteral("failed"));
            setError(QStringLiteral("Gallery display worker is unavailable."));
            return;
        }
    }
}

void StudioPresenter::invalidate_thumbnail(const std::string &asset_id)
{
    // Both render and monitor-presentation results from the previous recipe
    // must lose publication rights before a replacement browse request starts.
    thumbnail_requests_.erase(asset_id);
    pending_thumbnail_presentations_.erase(asset_id);
    std::erase(thumbnail_presentation_order_, asset_id);
    thumbnail_presentation_revisions_.erase(asset_id);
    thumbnail_bases_.erase(asset_id);
    thumbnail_repair_attempts_.erase(asset_id);
    thumbnail_load_repair_attempts_.erase(asset_id);
    const auto id = qstring_from_utf8(asset_id);
    const int row = assets_.indexOf(id);
    if (row < 0)
        return;
    const auto old_url =
        assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
    assets_.setThumbnail(asset_id, old_url, QStringLiteral("pending"));
    emit thumbnailsChanged();
    // Let the save callback finish queuing its settled foreground preview.
    // The existing demand owner then prioritizes that preview and coalesces
    // repeated edits instead of starting another rendering path.
    const auto catalog = catalog_path_;
    const auto generation = library_query_generation_;
    QMetaObject::invokeMethod(
        this,
        [this, id, catalog, generation]
        {
            if (catalog_path_ == catalog && library_query_generation_ == generation)
                ensureThumbnail(id);
        },
        Qt::QueuedConnection);
}

void StudioPresenter::remember_thumbnail_base(const std::string &asset_id, const QString &base_path,
                                              const ColorProfileState &source_profile,
                                              const QString &thumb_state)
{
    if (asset_id.empty() || base_path.isEmpty())
        return;
    thumbnail_bases_[asset_id] = {base_path, source_profile, thumb_state};
    const auto revision = ++thumbnail_presentation_revision_;
    thumbnail_presentation_revisions_[asset_id] = revision;
    if (display_presentation_ == nullptr || !display_presentation_->valid())
    {
        thumbnail_repair_attempts_.erase(asset_id);
        pending_thumbnail_presentations_.erase(asset_id);
        std::erase(thumbnail_presentation_order_, asset_id);
        auto url = QUrl::fromLocalFile(base_path);
        if (thumbnail_load_repair_attempts_.contains(asset_id))
            url.setFragment(QString::number(revision));
        assets_.setThumbnail(asset_id, url, thumb_state);
        return;
    }
    if (thumbnail_presented_root_.isEmpty())
    {
        const auto cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cache_root.isEmpty())
        {
            assets_.setThumbnail(asset_id, {}, QStringLiteral("failed"));
            setError(QStringLiteral("Gallery display cache location is unavailable."));
            return;
        }
        thumbnail_presented_root_ =
            QDir(cache_root).filePath(QStringLiteral("ravo-gallery-display-v3"));
    }
    if (!thumbnail_display_state_ ||
        thumbnail_display_state_->contract_version !=
            display_presentation_->presentationState().contract_version ||
        thumbnail_display_state_->profile_fingerprint !=
            display_presentation_->presentationState().profile_fingerprint)
        thumbnail_display_state_ = std::make_shared<const DisplayPresentationState>(
            display_presentation_->presentationState());
    const auto display = thumbnail_display_state_;
    const auto root = thumbnail_presented_root_;
    const auto generation = library_query_generation_;
    const auto cancellation = thumbnail_presentation_cancel_.token();
    const bool replace = thumbnail_load_repair_attempts_.contains(asset_id);
    if (!gallery_thumbnail_cache_)
        gallery_thumbnail_cache_ = std::make_unique<StudioGalleryThumbnailCache>();
    if (pending_thumbnail_presentations_.size() >= kMaximumPendingThumbnailRequests)
    {
        std::erase_if(pending_thumbnail_presentations_, [this](const auto &entry)
                      { return !assets_.assetById(qstring_from_utf8(entry.first)); });
        std::erase_if(thumbnail_presentation_order_, [this](const auto &id)
                      { return !pending_thumbnail_presentations_.contains(id); });
        if (pending_thumbnail_presentations_.size() >= kMaximumPendingThumbnailRequests &&
            !pending_thumbnail_presentations_.contains(asset_id))
        {
            assets_.setThumbnail(asset_id, {}, QStringLiteral("failed"));
            setError(QStringLiteral("Gallery display request limit exceeded."));
            return;
        }
    }
    const int row = assets_.indexOf(qstring_from_utf8(asset_id));
    auto previous_url =
        row < 0 ? QUrl{} :
                  assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
    // Incoming listing URLs are unconverted source pixels, not a prior display frame.
    if (previous_url == QUrl::fromLocalFile(base_path))
        previous_url = QUrl{};
    pending_thumbnail_presentations_[asset_id] = [this, asset_id, base_path, display, root,
                                                  generation, revision, cancellation, thumb_state,
                                                  replace]
    {
        auto result = prepare_gallery_thumbnail(base_path, *display, root, cancellation,
                                                *gallery_thumbnail_cache_, replace);
        QMetaObject::invokeMethod(
            this,
            [this, asset_id, generation, revision, result = std::move(result),
             thumb_state]() mutable
            {
                --thumbnail_presentations_in_flight_;
                const auto latest = thumbnail_presentation_revisions_.find(asset_id);
                if (generation == library_query_generation_ &&
                    latest != thumbnail_presentation_revisions_.end() &&
                    latest->second == revision && assets_.assetById(qstring_from_utf8(asset_id)))
                {
                    if (result)
                    {
                        thumbnail_repair_attempts_.erase(asset_id);
                        // Qt's image cache keys include the URL: a repaired file
                        // must not retain the previously failed decoded resource.
                        if (thumbnail_load_repair_attempts_.contains(asset_id))
                            result.value().setFragment(QString::number(revision));
                        assets_.setThumbnail(asset_id, result.value(), thumb_state);
                        if (selected_asset_id_ == qstring_from_utf8(asset_id) &&
                            browse_mode_ == QLatin1String("grid"))
                            refresh_scopes_from_thumbnail(selected_asset_id_);
                    }
                    else if (result.error().code != ErrorCode::kCancelled)
                    {
                        const auto reason = result.error().context.find("reason");
                        if (result.error().code == ErrorCode::kNotFound &&
                            reason != result.error().context.end() &&
                            reason->second == "gallery_thumbnail_cache_missing" &&
                            thumbnail_repair_attempts_.insert(asset_id).second)
                        {
                            // The service cache can evict a PNG after listing or
                            // generation. Re-enter its bounded browse-demand owner;
                            // only the service decides whether the original is missing.
                            thumbnail_bases_.erase(asset_id);
                            assets_.setThumbnail(asset_id, {}, QStringLiteral("pending"));
                            ensureThumbnail(qstring_from_utf8(asset_id));
                        }
                        else
                        {
                            assets_.setThumbnail(asset_id, {}, QStringLiteral("failed"));
                            setError(qstring_from_utf8(result.error().message));
                        }
                    }
                    emit thumbnailsChanged();
                }
                startNextThumbnailPresentation();
            },
            Qt::QueuedConnection);
    };
    std::erase(thumbnail_presentation_order_, asset_id);
    thumbnail_presentation_order_.push_back(asset_id);
    assets_.setThumbnail(asset_id, previous_url, QStringLiteral("presenting"));
    emit thumbnailsChanged();
    startNextThumbnailPresentation();
}

void StudioPresenter::reapply_display_presentation_to_cached_thumbnails()
{
    if (thumbnail_bases_.empty())
        return;
    static_cast<void>(thumbnail_presentation_cancel_.cancel("display_changed"));
    thumbnail_presentation_cancel_ = CancellationSource{};
    pending_thumbnail_presentations_.clear();
    thumbnail_presentation_order_.clear();
    const auto bases = thumbnail_bases_;
    for (const auto &[asset_id, base] : bases)
    {
        if (!assets_.assetById(qstring_from_utf8(asset_id)))
            continue;
        remember_thumbnail_base(asset_id, base.path, base.profile, base.terminal_state);
    }
    emit thumbnailsChanged();
}

void StudioPresenter::refresh_scopes_from_thumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        inspect_.clear_scopes();
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    const auto base = thumbnail_bases_.find(id);
    if (base != thumbnail_bases_.end() && QFileInfo::exists(base->second.path))
    {
        const QImage image(base->second.path);
        inspect_.observeNavigatorThumbnail(image);
        inspect_.refresh_scopes(image);
        return;
    }
    const int row = assets_.indexOf(asset_id);
    if (row < 0)
    {
        inspect_.clear_scopes();
        return;
    }
    const QUrl url = assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
    if (!url.isLocalFile())
    {
        inspect_.clear_scopes();
        return;
    }
    const QImage image(url.toLocalFile());
    inspect_.observeNavigatorThumbnail(image);
    inspect_.refresh_scopes(image);
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    const QString state = assets_.thumbnailState(id);
    if (state == QLatin1String("presenting"))
    {
        // Listing hydration may enqueue a whole page. An actual visible-cell
        // demand gets publication priority over that background FIFO.
        if (pending_thumbnail_presentations_.contains(id))
        {
            std::erase(thumbnail_presentation_order_, id);
            thumbnail_presentation_order_.push_front(id);
        }
        return;
    }
    if (state == QLatin1String("ready") || state == QLatin1String("proxy") ||
        state == QLatin1String("missing") || state == QLatin1String("failed"))
    {
        return;
    }
    if (thumbnail_requests_.contains(id))
    {
        return;
    }
    if (std::find(pending_thumbnail_ids_.begin(), pending_thumbnail_ids_.end(), id) !=
        pending_thumbnail_ids_.end())
    {
        std::erase(pending_thumbnail_ids_, id);
        pending_thumbnail_ids_.push_front(id);
        return;
    }
    if (!assets_.assetById(asset_id))
    {
        return;
    }
    if (!preview_work_active_ && !thumbnail_request_in_flight_ && pending_thumbnail_ids_.empty())
    {
        preview_work_completed_ = 0;
        preview_work_total_ = 0;
    }
    if (pending_thumbnail_ids_.size() >= kMaximumPendingThumbnailRequests)
    {
        pending_thumbnail_ids_.pop_back();
        preview_work_total_ = std::max(preview_work_completed_, preview_work_total_ - 1);
    }
    pending_thumbnail_ids_.push_front(id);
    ++preview_work_total_;
    preview_work_active_ = true;
    emit libraryWorkChanged();
    kickThumbnailDemand();
}

void StudioPresenter::thumbnailLoadFailed(const QString &asset_id, const QUrl &url)
{
    const auto id = utf8_from_qstring(asset_id);
    const int row = assets_.indexOf(asset_id);
    const auto state = assets_.thumbnailState(id);
    if (row < 0 || url.isEmpty() || catalog_path_.isEmpty() ||
        assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl() != url ||
        (state != QLatin1String("ready") && state != QLatin1String("proxy") &&
         state != QLatin1String("missing")))
        return;
    if (!thumbnail_load_repair_attempts_.insert(id).second)
    {
        assets_.setThumbnail(id, {}, QStringLiteral("failed"));
        setError(QStringLiteral("Gallery thumbnail remains unreadable after repair."));
        return;
    }
    const auto base = thumbnail_bases_.find(id);
    if (base != thumbnail_bases_.end())
    {
        const auto saved = base->second;
        remember_thumbnail_base(id, saved.path, saved.profile, saved.terminal_state);
        ensureThumbnail(asset_id);
    }
    else
    {
        assets_.setThumbnail(id, {}, QStringLiteral("pending"));
        ensureThumbnail(asset_id);
    }
}

void StudioPresenter::startThumbnailRequest(std::string id)
{
    const auto revision = ++thumbnail_revision_;
    const auto cancellation = thumbnail_work_.token();
    thumbnail_requests_[id] = revision;
    const bool queued = executor_.post(
        [this, id, revision, cancellation]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kThumbnailMaxEdge;
                request.request_revision = revision;
                request.purpose = PreviewPurpose::kBrowse;
                request.prefer_embedded_preview = true;
                request.cancellation = cancellation;
                preview = service_->preview().request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = thumbnail_requests_.find(id);
                    if (latest == thumbnail_requests_.end() || latest->second != revision)
                    {
                        finishThumbnailRequest(false);
                        return;
                    }
                    thumbnail_requests_.erase(latest);
                    if (catalog_path_.isEmpty() || !assets_.assetById(qstring_from_utf8(id)))
                    {
                        finishThumbnailRequest(false);
                        return;
                    }
                    if (preview)
                    {
                        const QString thumb_state =
                            preview.value().media_state == "proxy" ?
                                QStringLiteral("proxy") :
                                (preview.value().original_missing ? QStringLiteral("missing") :
                                                                    QStringLiteral("ready"));
                        remember_thumbnail_base(id, qstring_from_utf8(preview.value().cache_path),
                                                preview.value().color_profile, thumb_state);
                        if (utf8_from_qstring(selected_asset_id_) == id)
                        {
                            emit thumbnailsChanged();
                            if (browse_mode_ == QLatin1String("grid"))
                            {
                                refresh_scopes_from_thumbnail(qstring_from_utf8(id));
                            }
                        }
                        if (preview.value().original_missing)
                        {
                            assets_.markOriginalMissing(id);
                        }
                        finishThumbnailRequest(true);
                        return;
                    }
                    if (preview.error().code == ErrorCode::kNotFound)
                    {
                        assets_.markOriginalMissing(id);
                        assets_.setThumbnail(id, {}, QStringLiteral("missing"));
                        if (selected_ids_.contains(id))
                        {
                            emit thumbnailsChanged();
                        }
                        finishThumbnailRequest(false);
                        return;
                    }
                    if (preview.error().code == ErrorCode::kCancelled)
                    {
                        if (std::find(pending_thumbnail_ids_.begin(), pending_thumbnail_ids_.end(),
                                      id) == pending_thumbnail_ids_.end())
                        {
                            if (pending_thumbnail_ids_.size() >= kMaximumPendingThumbnailRequests)
                            {
                                pending_thumbnail_ids_.pop_back();
                                preview_work_total_ =
                                    std::max(preview_work_completed_, preview_work_total_ - 1);
                            }
                            pending_thumbnail_ids_.push_front(id);
                        }
                        thumbnail_request_in_flight_ = false;
                        kickThumbnailDemand();
                        return;
                    }
                    assets_.setThumbnail(id, {}, QStringLiteral("failed"));
                    finishThumbnailRequest(false);
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        thumbnail_requests_.erase(id);
        assets_.setThumbnail(id, {}, QStringLiteral("failed"));
        finishThumbnailRequest(false);
    }
}

void StudioPresenter::requestPreviewForSelection()
{
    if (selectedMediaType().startsWith(QLatin1String("video/")))
    {
        inspect_.setPreviewLoading(false);
        inspect_.notifyPreviewChanged();
        if (browse_mode_ == QLatin1String("loupe"))
        {
            video_presenter_->observeAsset(assets_.assetById(selected_asset_id_));
            video_presenter_->requestPoster();
        }
        if (browse_mode_ != QLatin1String("survey"))
            return;
    }
    if (browse_mode_ == QLatin1String("grid"))
    {
        inspect_.setPreviewLoading(false);
        inspect_.notifyPreviewChanged();
        refresh_scopes_from_thumbnail(selected_asset_id_);
        return;
    }
    if (browse_mode_ == QLatin1String("survey"))
    {
        inspect_.setPreviewLoading(false);
        inspect_.notifyPreviewChanged();
        requestSurveyPreviews();
        return;
    }
    develop_presenter_->enqueue_preview();
}

void StudioPresenter::rebuild_survey_slots()
{
    survey_slot_ids_.clear();
    if (burst_compare_slot_ids_.size() >= static_cast<std::size_t>(kSurveySlotMinimum))
    {
        const std::size_t count =
            std::min(burst_compare_slot_ids_.size(), static_cast<std::size_t>(kSurveySlotMaximum));
        survey_slot_ids_.assign(burst_compare_slot_ids_.begin(),
                                burst_compare_slot_ids_.begin() +
                                    static_cast<std::ptrdiff_t>(count));
    }
    else
    {
        const auto ids = selected_asset_ids();
        std::size_t count = 0U;
        if (ids.size() >= static_cast<std::size_t>(kSurveySlotMaximum))
            count = static_cast<std::size_t>(kSurveySlotMaximum);
        else if (ids.size() >= static_cast<std::size_t>(kSurveySlotMinimum))
            count = static_cast<std::size_t>(kSurveySlotMinimum);
        survey_slot_ids_.assign(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count));
    }
    for (auto it = survey_preview_urls_.begin(); it != survey_preview_urls_.end();)
    {
        if (std::find(survey_slot_ids_.begin(), survey_slot_ids_.end(), it->first) ==
            survey_slot_ids_.end())
            it = survey_preview_urls_.erase(it);
        else
            ++it;
    }
}

void StudioPresenter::requestSurveyPreviews()
{
    rebuild_survey_slots();
    emit surveyChanged();
    for (const auto &id : survey_slot_ids_)
    {
        if (survey_preview_urls_.contains(id) || survey_preview_requests_.contains(id))
            continue;
        if (std::find(pending_survey_ids_.begin(), pending_survey_ids_.end(), id) !=
            pending_survey_ids_.end())
            continue;
        pending_survey_ids_.push_back(id);
    }
    if (!survey_preview_in_flight_ && !pending_survey_ids_.empty())
    {
        auto next = pending_survey_ids_.front();
        pending_survey_ids_.pop_front();
        startSurveyPreviewRequest(std::move(next));
    }
}

void StudioPresenter::startSurveyPreviewRequest(std::string id)
{
    const auto revision = ++survey_preview_revision_;
    survey_preview_in_flight_ = true;
    survey_preview_requests_[id] = revision;
    const auto cancellation = thumbnail_work_.token();
    static_cast<void>(executor_.post(
        [this, id, revision, cancellation]()
        {
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                PreviewRequest request;
                request.asset_id = id;
                request.max_edge = kDefaultPreviewMaxEdge;
                request.request_revision = revision;
                request.purpose = PreviewPurpose::kBrowse;
                request.prefer_embedded_preview = false;
                request.cancellation = cancellation;
                preview = service_->preview().request_preview(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, id, revision, preview = std::move(preview)]() mutable
                {
                    const auto latest = survey_preview_requests_.find(id);
                    if (latest == survey_preview_requests_.end() || latest->second != revision)
                    {
                        finishSurveyPreviewRequest(false);
                        return;
                    }
                    survey_preview_requests_.erase(latest);
                    if (preview)
                    {
                        survey_preview_urls_[id] =
                            QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path));
                        emit surveyChanged();
                        finishSurveyPreviewRequest(true);
                        return;
                    }
                    finishSurveyPreviewRequest(false);
                },
                Qt::QueuedConnection);
        }));
}

void StudioPresenter::finishSurveyPreviewRequest(const bool success)
{
    static_cast<void>(success);
    survey_preview_in_flight_ = false;
    if (browse_mode_ != QLatin1String("survey") || pending_survey_ids_.empty())
        return;
    auto next = pending_survey_ids_.front();
    pending_survey_ids_.pop_front();
    startSurveyPreviewRequest(std::move(next));
}

} // namespace ravo
