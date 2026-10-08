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
#include <QLockFile>
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

namespace ravo
{
namespace
{

inline constexpr std::size_t kMaximumPendingThumbnailRequests = kLibraryPageDefaultSize * 3U;

Result<QUrl> prepare_gallery_thumbnail(const QString &base_path,
                                       const DisplayPresentationState &display, const QString &root,
                                       const CancellationToken &cancellation)
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
    // The immutable preview PNG owns its embedded source profile. Runtime profile
    // objects are absent on catalog reopen and must not change the cache identity.
    QByteArray identity = QByteArray::number(kThumbnailMaxEdge) + '\n' +
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
    if (valid_cached_header())
        return QUrl::fromLocalFile(output);
    QImageReader reader(base_path);
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
    // Serialize miss publication/eviction across processes. Re-index only on a
    // miss so the shared 512 MiB budget includes other windows' publications.
    QLockFile lock(QDir(root).filePath(QStringLiteral("publish.lock")));
    if (!lock.tryLock(1000))
        return make_error(ErrorCode::kConflict, "Gallery display cache is busy");
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (valid_cached_header())
        return QUrl::fromLocalFile(output);
    auto cache = FilesystemPreviewCache::create(utf8_from_qstring(root));
    if (!cache)
        return cache.error();
    auto removed = cache.value()->remove_png(utf8_from_qstring(key));
    if (!removed)
        return removed.error();
    if (auto active = cancellation.check(); !active)
        return active.error();
    auto committed = cache.value()->commit_png_bytes(
        utf8_from_qstring(key), std::vector<std::uint8_t>(encoded.cbegin(), encoded.cend()));
    if (!committed)
        return committed.error();
    return QUrl::fromLocalFile(qstring_from_utf8(committed.value()));
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
    thumbnail_presentation_revisions_.clear();
    thumbnail_repair_attempts_.clear();
    thumbnail_base_paths_.clear();
    thumbnail_base_profiles_.clear();
    thumbnail_presented_root_.clear();
}

void StudioPresenter::startNextThumbnailPresentation()
{
    while (!pending_thumbnail_presentations_.empty() &&
           !assets_.assetById(qstring_from_utf8(pending_thumbnail_presentations_.begin()->first)))
        pending_thumbnail_presentations_.erase(pending_thumbnail_presentations_.begin());
    if (thumbnail_presentation_in_flight_ || pending_thumbnail_presentations_.empty())
        return;
    auto next = pending_thumbnail_presentations_.begin();
    auto task = std::move(next->second);
    pending_thumbnail_presentations_.erase(next);
    thumbnail_presentation_in_flight_ = true;
    if (!thumbnail_presentation_executor_.post(std::move(task)))
    {
        thumbnail_presentation_in_flight_ = false;
        setError(QStringLiteral("Gallery display worker is unavailable."));
    }
}

void StudioPresenter::invalidate_thumbnail(const std::string &asset_id)
{
    // Both render and monitor-presentation results from the previous recipe
    // must lose publication rights before a replacement browse request starts.
    thumbnail_requests_.erase(asset_id);
    pending_thumbnail_presentations_.erase(asset_id);
    thumbnail_presentation_revisions_.erase(asset_id);
    thumbnail_base_paths_.erase(asset_id);
    thumbnail_base_profiles_.erase(asset_id);
    thumbnail_repair_attempts_.erase(asset_id);
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
    thumbnail_base_paths_[asset_id] = base_path;
    thumbnail_base_profiles_[asset_id] = source_profile;
    const auto revision = ++thumbnail_presentation_revision_;
    thumbnail_presentation_revisions_[asset_id] = revision;
    if (display_presentation_ == nullptr || !display_presentation_->valid())
    {
        thumbnail_repair_attempts_.erase(asset_id);
        pending_thumbnail_presentations_.erase(asset_id);
        assets_.setThumbnail(asset_id, QUrl::fromLocalFile(base_path), thumb_state);
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
            QDir(cache_root).filePath(QStringLiteral("ravo-gallery-display-v2"));
    }
    if (!thumbnail_display_state_ ||
        thumbnail_display_state_->profile_fingerprint !=
            display_presentation_->presentationState().profile_fingerprint)
        thumbnail_display_state_ = std::make_shared<const DisplayPresentationState>(
            display_presentation_->presentationState());
    const auto display = thumbnail_display_state_;
    const auto root = thumbnail_presented_root_;
    const auto generation = library_query_generation_;
    const auto cancellation = thumbnail_presentation_cancel_.token();
    if (pending_thumbnail_presentations_.size() >= kMaximumPendingThumbnailRequests)
    {
        std::erase_if(pending_thumbnail_presentations_, [this](const auto &entry)
                      { return !assets_.assetById(qstring_from_utf8(entry.first)); });
        if (pending_thumbnail_presentations_.size() >= kMaximumPendingThumbnailRequests &&
            !pending_thumbnail_presentations_.contains(asset_id))
        {
            assets_.setThumbnail(asset_id, {}, QStringLiteral("failed"));
            setError(QStringLiteral("Gallery display request limit exceeded."));
            return;
        }
    }
    assets_.setThumbnail(asset_id, {}, QStringLiteral("presenting"));
    pending_thumbnail_presentations_[asset_id] =
        [this, asset_id, base_path, display, root, generation, revision, cancellation, thumb_state]
    {
        auto result = prepare_gallery_thumbnail(base_path, *display, root, cancellation);
        QMetaObject::invokeMethod(
            this,
            [this, asset_id, generation, revision, result = std::move(result),
             thumb_state]() mutable
            {
                thumbnail_presentation_in_flight_ = false;
                const auto latest = thumbnail_presentation_revisions_.find(asset_id);
                if (generation == library_query_generation_ &&
                    latest != thumbnail_presentation_revisions_.end() &&
                    latest->second == revision && assets_.assetById(qstring_from_utf8(asset_id)))
                {
                    if (result)
                    {
                        thumbnail_repair_attempts_.erase(asset_id);
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
                            thumbnail_base_paths_.erase(asset_id);
                            thumbnail_base_profiles_.erase(asset_id);
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
    startNextThumbnailPresentation();
}

void StudioPresenter::reapply_display_presentation_to_cached_thumbnails()
{
    if (thumbnail_base_paths_.empty())
        return;
    static_cast<void>(thumbnail_presentation_cancel_.cancel("display_changed"));
    thumbnail_presentation_cancel_ = CancellationSource{};
    pending_thumbnail_presentations_.clear();
    for (const auto &[asset_id, base_path] : thumbnail_base_paths_)
    {
        if (!assets_.assetById(qstring_from_utf8(asset_id)))
            continue;
        const auto profile_it = thumbnail_base_profiles_.find(asset_id);
        const ColorProfileState profile =
            profile_it == thumbnail_base_profiles_.end() ? ColorProfileState{} : profile_it->second;
        const QString state = assets_.thumbnailState(asset_id);
        const QString publish_state = state.isEmpty() ? QStringLiteral("ready") : state;
        remember_thumbnail_base(asset_id, base_path, profile,
                                publish_state == QStringLiteral("presenting") ?
                                    QStringLiteral("ready") :
                                    publish_state);
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
    const auto base = thumbnail_base_paths_.find(id);
    if (base != thumbnail_base_paths_.end() && QFileInfo::exists(base->second))
    {
        inspect_.refresh_scopes(QImage(base->second));
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
    inspect_.refresh_scopes(QImage(url.toLocalFile()));
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    const QString state = assets_.thumbnailState(id);
    if (state == QLatin1String("ready") || state == QLatin1String("presenting") ||
        state == QLatin1String("missing") || state == QLatin1String("failed"))
    {
        return;
    }
    if (thumbnail_requests_.contains(id) ||
        std::find(pending_thumbnail_ids_.begin(), pending_thumbnail_ids_.end(), id) !=
            pending_thumbnail_ids_.end())
    {
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
