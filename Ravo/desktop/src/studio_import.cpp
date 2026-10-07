#include "ravo/desktop/studio_import_workspace.h"
#include "studio_import_worker.h"
#include "studio_import_scan_controller.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_import_destination_preview_controller.h"
#include "ravo/desktop/studio_import_preferences.h"
#include "ravo/services/ingest_transport.h"
#include "studio_qt.h"
#include <algorithm>
#include <utility>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
[[nodiscard]] QVariantMap
StudioImportWorkspace::ingest_report_to_map(const IngestBatchResult &detailed)
{
    const auto &batch = detailed.import;
    QVariantList items;
    items.reserve(static_cast<int>(batch.items.size()));
    for (const auto &item : batch.items)
    {
        QVariantMap row{
            {QStringLiteral("status"),
             item.status == ImportItemStatus::kImported    ? QStringLiteral("imported") :
             item.status == ImportItemStatus::kDuplicate   ? QStringLiteral("duplicate") :
             item.status == ImportItemStatus::kUnsupported ? QStringLiteral("unsupported") :
             item.status == ImportItemStatus::kSkipped     ? QStringLiteral("skipped") :
                                                             QStringLiteral("failed")},
            {QStringLiteral("inputPath"), qstring_from_utf8(item.input_path)},
            {QStringLiteral("copiesVerified"), item.copies_verified},
        };
        if (item.destination_path)
            row.insert(QStringLiteral("destinationPath"),
                       qstring_from_utf8(*item.destination_path));
        if (item.error)
        {
            row.insert(QStringLiteral("error"), qstring_from_utf8(item.error->message));
            const auto reason = item.error->context.find("reason");
            if (reason != item.error->context.end())
                row.insert(QStringLiteral("reason"), qstring_from_utf8(reason->second));
        }
        items.push_back(row);
    }
    return QVariantMap{
        {QStringLiteral("transport"), qstring_from_utf8(detailed.transport)},
        {QStringLiteral("sourceUri"), qstring_from_utf8(detailed.source_uri)},
        {QStringLiteral("imported"), static_cast<int>(batch.imported)},
        {QStringLiteral("duplicates"), static_cast<int>(batch.duplicates)},
        {QStringLiteral("unsupported"), static_cast<int>(batch.unsupported)},
        {QStringLiteral("failed"), static_cast<int>(batch.failed)},
        {QStringLiteral("skipped"), static_cast<int>(batch.skipped)},
        {QStringLiteral("verifiedSecondCopies"), static_cast<int>(batch.verified_second_copies)},
        {QStringLiteral("resumeBatchId"),
         detailed.resume_batch_id ? qstring_from_utf8(*detailed.resume_batch_id) : QString()},
        {QStringLiteral("resumeCheckpointCleared"), detailed.resume_checkpoint_cleared},
        {QStringLiteral("items"), items},
        {QStringLiteral("nativeSupport"), native_support_to_map(detailed.support)},
    };
}

namespace
{
[[nodiscard]] bool uses_ingest_copy_path(const QString &transport, const QString &mode)
{
    if (mode != QLatin1String("copy"))
        return false;
    return transport == QLatin1String("filesystem-card") ||
           transport == QLatin1String("ptp-stub") || transport == QLatin1String("ptp-usb") ||
           transport == QLatin1String("mtp");
}

} // namespace
void StudioPresenter::beginImportGalleryPlaceholders(const std::vector<std::string> &paths)
{
    std::vector<AssetRecord> placeholders;
    placeholders.reserve(paths.size());
    for (const auto &path : paths)
    {
        AssetRecord placeholder;
        placeholder.normalized_uri = path;
        placeholders.push_back(std::move(placeholder));
    }
    last_import_count_ = paths.size();
    last_import_selected_ = true;
    last_import_after_unix_ms_.reset();
    last_import_before_unix_ms_.reset();
    selected_asset_id_.clear();
    selection_anchor_id_.clear();
    selected_ids_.clear();
    applyAssets(std::move(placeholders), false, {}, {}, paths.size(), false);
}

void StudioPresenter::publishImportItem(const ImportItemResult &item, const int row)
{
    if (!import_workspace_->galleryPlaceholders() || row < 0 || row >= assets_.rowCount())
        return;
    const bool last_import_was_available = lastImportAvailable();
    if (item.status == ImportItemStatus::kImported && item.asset)
    {
        const auto created = item.asset->created_unix_ms;
        last_import_after_unix_ms_ =
            last_import_after_unix_ms_ ? std::min(*last_import_after_unix_ms_, created) : created;
        last_import_before_unix_ms_ =
            last_import_before_unix_ms_ ? std::max(*last_import_before_unix_ms_, created) : created;
        const std::string asset_id = item.asset->id;
        assets_.replaceAssetAt(row, *item.asset);
        if (item.preview_cache_path)
            remember_thumbnail_base(asset_id, qstring_from_utf8(*item.preview_cache_path),
                                    ColorProfileState{}, QStringLiteral("ready"));
        if (selected_asset_id_.isEmpty())
            selectAsset(qstring_from_utf8(asset_id));
        else
            emit thumbnailsChanged();
        if (!last_import_was_available && lastImportAvailable())
            emit folderChanged();
        return;
    }
    AssetRecord placeholder;
    placeholder.normalized_uri =
        item.input_path.empty() ? assets_.assetIdAt(row).toStdString() : item.input_path;
    placeholder.import_state = std::string(kImportStateFailed);
    if (item.error)
        placeholder.error_message = item.error->message;
    assets_.replaceAssetAt(row, std::move(placeholder));
}

void StudioImportWorkspace::startPlannedImport()
{
    const QStringList selected = candidates.selectedPaths();
    if (!import_page_open_ || import_work_active_ || import_preflight_active_ || selected.isEmpty())
        return;
    if (!(scan && scan->catalogRevision()))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Scan the source folder again."));
        return;
    }
    if (draft.mode != QLatin1String("add") && draft.destination.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Choose an import destination."));
        return;
    }
    if ((import_ingest_transport_ == QLatin1String("filesystem-card") ||
         import_ingest_transport_ == QLatin1String("ptp-stub") ||
         import_ingest_transport_ == QLatin1String("ptp-usb") ||
         import_ingest_transport_ == QLatin1String("mtp")) &&
        draft.mode == QLatin1String("move"))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter",
            "Ingest transports are Copy-only; Move and camera delete stay rejected."));
        return;
    }
    if (import_ingest_transport_ == QLatin1String("ptp-usb") ||
        import_ingest_transport_ == QLatin1String("mtp"))
    {
        refreshImportNativeSupport();
        const auto reason = import_native_support_.value(QStringLiteral("reason")).toString();
        setError(QCoreApplication::translate(
                     "StudioPresenter",
                     "Native PTP/MTP adapter is not packaged (%1). Use filesystem-card or the "
                     "ptp-stub fixture.")
                     .arg(reason.isEmpty() ? QStringLiteral("native_ingest_adapter_not_packaged") :
                                             reason));
        return;
    }

    ImportRequest request = plannedImportRequest();
    cancelImportPreviews();
    ++import_generation_;
    // Enumeration supplies stable candidate identities. Stop optional classification
    // before queueing preflight; preflight/import still recheck duplicates and hashes.
    scan->bumpGeneration("import_preflight_started");
    scan->finish();
    const auto generation = scan ? scan->generation() : 0U;
    import_preflight_active_ = true;
    import_page_open_ = false;
    import_context_row_ = -1;
    import_context_path_.clear();
    if (thumbnails)
    {
        thumbnails->cancel("import_preflight_started");
        thumbnails->clearPending();
    }
    setImportWork(0, static_cast<int>(request.inputs.size()), true);
    host_.enter_gallery();
    setStatus(QCoreApplication::translate("ImportPage", "Checking destination…"));
    setError({});
    emit importPageChanged();
    worker->executor().post(
        [this, generation, request = std::move(request)]() mutable
        {
            auto *service = worker->service();
            auto ready = service == nullptr ?
                             Result<void>{make_error(ErrorCode::kIo, "Catalog session is closed")} :
                             service->import().preflight_import(request);
            QMetaObject::invokeMethod(
                this,
                [this, generation, ready = std::move(ready), request = std::move(request)]() mutable
                {
                    if (!scan->matches(generation) || !import_preflight_active_)
                        return;
                    import_preflight_active_ = false;
                    emit importPageChanged();
                    if (auto active = request.cancellation.check(); !active)
                        ready = active.error();
                    if (!ready)
                    {
                        setImportWork(0, 0, false);
                        setError(ready.error().code == ErrorCode::kCancelled ?
                                     QString{} :
                                     qstring_from_utf8(ready.error().message));
                        setStatus(
                            ready.error().code == ErrorCode::kCancelled ?
                                QCoreApplication::translate(
                                    "StudioPresenter", "Import cancelled after %1 of %2 photos.")
                                    .arg(0)
                                    .arg(request.inputs.size()) :
                                QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    beginPlannedImport(std::move(request));
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioImportWorkspace::beginPlannedImport(ImportRequest request)
{
    const auto generation = import_generation_;
    if (thumbnails)
        thumbnails->cancel("planned_import_started");
    const bool ingest_copy = uses_ingest_copy_path(import_ingest_transport_, draft.mode);
    pending_import_destination_ = request.mode == ImportTransferMode::kAdd ?
                                      QString{} :
                                      qstring_from_utf8(request.destination_directory);
    import_destination_remembered_ = false;
    import_preference_error_.clear();
    static_cast<void>(import_operation_.cancel("planned_import_started"));
    if (scan)
        scan->bumpGeneration("planned_import_started");
    if (thumbnails)
        thumbnails->clearPending();
    import_operation_ = CancellationSource{};
    request.cancellation = import_operation_.token();
    pending_import_paths_ = request.inputs;
    import_query_snapshot_ = host_.current_query();
    pending_import_preview_policy_ = request.preview;
    import_defer_previews_ = true;
    import_skip_existing_ = true;
    pending_import_content_hashes_.clear();
    for (const auto &[path, digest] : request.expected_content_hashes)
        pending_import_content_hashes_.emplace(path, digest);
    import_results_.clear();
    import_ingest_report_ = {};
    import_next_index_ = 0U;
    pending_import_preview_ids_.clear();
    import_preview_operation_ = CancellationSource{};
    setImportWork(0, static_cast<int>(request.inputs.size()), true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Importing 0 / %1…")
                  .arg(request.inputs.size()));
    beginImportGalleryPlaceholders(request.inputs);
    if (request.mode == ImportTransferMode::kAdd)
    {
        startNextImportItem();
        return;
    }
    if (ingest_copy)
    {
        IngestRequest ingest;
        ingest.source_root = utf8_from_qstring(draft.source_root);
        ingest.transport = utf8_from_qstring(import_ingest_transport_);
        ingest.mode = ImportTransferMode::kCopy;
        ingest.organization = request.organization;
        ingest.preview = request.preview;
        ingest.destination_directory = request.destination_directory;
        ingest.filename_template = request.filename_template;
        ingest.second_copy_directory = request.second_copy_directory;
        ingest.recursive =
            import_source_recursion(draft.source_root, QDir::homePath(), import_recursive_);
        ingest.include_xmp_sidecars = true;
        ingest.defer_previews = true;
        ingest.skip_existing = true;
        ingest.expected_catalog_revision = request.expected_catalog_revision;
        ingest.expected_content_hashes = request.expected_content_hashes;
        ingest.selected_paths = request.inputs;
        ingest.cancellation = request.cancellation;
        if (!import_resume_batch_id_.isEmpty())
            ingest.resume_batch_id = utf8_from_qstring(import_resume_batch_id_);
        worker->executor().post(
            [this, generation, ingest = std::move(ingest)]() mutable
            {
                auto *service = worker->service();
                auto detailed =
                    service == nullptr ?
                        Result<IngestBatchResult>{
                            make_error(ErrorCode::kIo, "Catalog session is closed")} :
                        service->ingest().execute_ingest_detailed(
                            ingest,
                            [this, generation](const std::size_t completed, const std::size_t total,
                                               const ImportItemResult *item)
                            {
                                ImportItemResult copy;
                                if (item != nullptr)
                                    copy = *item;
                                QMetaObject::invokeMethod(
                                    this,
                                    [this, generation, completed, total, copy = std::move(copy),
                                     has_item = item != nullptr]
                                    {
                                        if (generation != import_generation_)
                                            return;
                                        setImportWork(static_cast<int>(completed),
                                                      static_cast<int>(total), true);
                                        if (has_item)
                                            publishImportItem(copy,
                                                              static_cast<int>(completed) - 1);
                                    },
                                    Qt::QueuedConnection);
                            });
                QMetaObject::invokeMethod(
                    this,
                    [this, generation, detailed = std::move(detailed)]() mutable
                    {
                        if (generation != import_generation_)
                            return;
                        if (!detailed)
                        {
                            setImportWork(0, 0, false);
                            import_gallery_placeholders_ = false;
                            import_defer_previews_ = false;
                            host_.restore_listing(import_query_snapshot_);
                            import_ingest_report_ = {};
                            setError(qstring_from_utf8(detailed.error().message));
                            setStatus(
                                QCoreApplication::translate("StudioPresenter", "Ingest failed."));
                            emit importPageChanged();
                            host_.reload_library();

                            return;
                        }
                        auto value = std::move(detailed).value();
                        import_ingest_report_ = ingest_report_to_map(value);
                        if (value.resume_batch_id && !value.resume_checkpoint_cleared)
                            import_resume_batch_id_ = qstring_from_utf8(*value.resume_batch_id);
                        else if (value.resume_checkpoint_cleared)
                            import_resume_batch_id_.clear();
                        emit importPageChanged();
                        import_results_ = std::move(value.import.items);
                        import_next_index_ = import_results_.size();
                        finishImportBatch();
                    },
                    Qt::QueuedConnection);
            },
            TaskPriority::kForeground);
        return;
    }
    worker->executor().post(
        [this, generation, request = std::move(request)]() mutable
        {
            auto *service = worker->service();
            auto batch =
                service == nullptr ?
                    Result<ImportBatchResult>{
                        make_error(ErrorCode::kIo, "Catalog session is closed")} :
                    service->import().execute_import(
                        request,
                        [this, generation](const std::size_t completed, const std::size_t total,
                                           const ImportItemResult *item)
                        {
                            ImportItemResult copy;
                            if (item != nullptr)
                                copy = *item;
                            QMetaObject::invokeMethod(
                                this,
                                [this, generation, completed, total, copy = std::move(copy),
                                 has_item = item != nullptr]
                                {
                                    if (generation != import_generation_)
                                        return;
                                    setImportWork(static_cast<int>(completed),
                                                  static_cast<int>(total), true);
                                    if (has_item)
                                        publishImportItem(copy, static_cast<int>(completed) - 1);
                                },
                                Qt::QueuedConnection);
                        });
            QMetaObject::invokeMethod(
                this,
                [this, generation, batch = std::move(batch)]() mutable
                {
                    if (generation != import_generation_)
                        return;
                    if (!batch)
                    {
                        setImportWork(0, 0, false);
                        import_gallery_placeholders_ = false;
                        import_defer_previews_ = false;
                        host_.restore_listing(import_query_snapshot_);
                        setError(qstring_from_utf8(batch.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        host_.reload_library();
                        return;
                    }
                    import_results_ = std::move(batch).value().items;
                    import_next_index_ = import_results_.size();
                    finishImportBatch();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioImportWorkspace::startNextImportPreview()
{
    if (pending_import_preview_ids_.empty())
    {
        import_preview_work_active_ = false;
        import_preview_work_completed_ = import_preview_work_total_;
        emit libraryWorkChanged();
        return;
    }
    const std::string asset_id = std::move(pending_import_preview_ids_.front());
    pending_import_preview_ids_.pop_front();
    const auto policy = pending_import_preview_policy_;
    const auto token = import_preview_operation_.token();
    const auto generation = import_preview_generation_;
    worker->executor().post(
        [this, asset_id, policy, token, generation]
        {
            auto *service = worker->service();
            auto preview =
                service == nullptr ?
                    Result<PreviewResult>{make_error(ErrorCode::kIo, "Catalog session is closed")} :
                    service->preview().build_import_preview(asset_id, policy, token);
            QMetaObject::invokeMethod(
                this,
                [this, generation, preview = std::move(preview)]
                {
                    if (generation != import_preview_generation_)
                        return;
                    if (!preview && preview.error().code != ErrorCode::kCancelled &&
                        !(preview.error().code == ErrorCode::kConflict &&
                          preview.error().context.contains("reason") &&
                          preview.error().context.at("reason") == "stale_preview_state"))
                        setError(qstring_from_utf8(preview.error().message));
                    ++import_preview_work_completed_;
                    emit libraryWorkChanged();
                    startNextImportPreview();
                },
                Qt::QueuedConnection);
        });
}

void StudioImportWorkspace::cancelImportPreviews()
{
    if (!import_preview_work_active_)
        return;
    static_cast<void>(import_preview_operation_.cancel("user_cancelled"));
    ++import_preview_generation_;
    pending_import_preview_ids_.clear();
    import_preview_work_active_ = false;
    import_preview_work_total_ = import_preview_work_completed_;
    emit libraryWorkChanged();
}

void StudioImportWorkspace::setImportWork(const int completed, const int total, const bool active)
{
    const int clamped_total = std::max(0, total);
    const int clamped_completed = std::clamp(completed, 0, std::max(clamped_total, completed));
    if (import_work_active_ == active && import_work_completed_ == clamped_completed &&
        import_work_total_ == clamped_total)
    {
        return;
    }
    import_work_active_ = active;
    import_work_completed_ = clamped_completed;
    import_work_total_ = clamped_total;
    emit libraryWorkChanged();
}

void StudioImportWorkspace::importFolder(const QUrl &folder_url)
{
    importFiles(QList<QUrl>{folder_url});
}

void StudioImportWorkspace::importFilePaths(const QStringList &paths)
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

void StudioImportWorkspace::importFolderFromPath(const QString &path)
{
    importFolder(url_from_dialog_path(path));
}

void StudioImportWorkspace::importFiles(const QList<QUrl> &files)
{
    if (context_.busy || import_work_active_ || context_.catalog_path.isEmpty())
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
        setError(QCoreApplication::translate("StudioPresenter", "No local files selected."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Scanning folder…"));
    closeImportPage();
    import_skip_existing_ = false;
    cancelImportPreviews();
    const auto generation = ++import_generation_;
    pending_import_content_hashes_.clear();
    pending_import_destination_.clear();
    import_preference_error_.clear();
    setImportWork(0, 0, true);
    import_operation_ = CancellationSource{};
    const auto cancellation = import_operation_.token();
    import_query_snapshot_ = host_.current_query();
    worker->executor().post(
        [this, paths = std::move(paths), cancellation, generation]
        {
            auto *service = worker->service();
            Result<std::vector<std::string>> enumerated =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service != nullptr)
                enumerated = service->import().enumerate_import_inputs(paths, cancellation);
            QMetaObject::invokeMethod(
                this,
                [this, generation, enumerated = std::move(enumerated)]() mutable
                {
                    if (generation != import_generation_)
                        return;
                    if (!enumerated)
                    {
                        setImportWork(0, 0, false);
                        setError(qstring_from_utf8(enumerated.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    pending_import_paths_ = std::move(enumerated).value();
                    import_results_.clear();
                    import_results_.reserve(pending_import_paths_.size());
                    import_next_index_ = 0U;
                    setImportWork(0, static_cast<int>(pending_import_paths_.size()), true);
                    if (pending_import_paths_.empty())
                    {
                        finishImportBatch();
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter", "Importing 0 / %1…")
                                  .arg(pending_import_paths_.size()));
                    startNextImportItem();
                },
                Qt::QueuedConnection);
        });
}

void StudioImportWorkspace::startNextImportItem()
{
    if (!import_work_active_)
        return;
    if (import_operation_.token().is_cancellation_requested() ||
        import_next_index_ >= pending_import_paths_.size())
    {
        finishImportBatch();
        return;
    }
    const auto path = pending_import_paths_[import_next_index_];
    const auto generation = import_generation_;
    const auto cancellation = import_operation_.token();
    const auto policy =
        import_defer_previews_ ? pending_import_preview_policy_ : ImportPreviewPolicy::kMinimal;
    const bool defer = import_defer_previews_;
    const bool skip_existing = import_skip_existing_;
    const auto hash = pending_import_content_hashes_.find(path);
    const std::string expected_hash =
        hash == pending_import_content_hashes_.end() ? std::string{} : hash->second;
    worker->executor().post(
        [this, path, cancellation, policy, defer, skip_existing, expected_hash, generation]
        {
            auto *service = worker->service();
            Result<ImportItemResult> imported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service != nullptr)
                imported = service->import().import_one(path, cancellation, policy, defer,
                                                        skip_existing, expected_hash);
            QMetaObject::invokeMethod(
                this,
                [this, path, generation, imported = std::move(imported)]() mutable
                {
                    if (generation != import_generation_)
                        return;
                    ImportItemResult item;
                    if (imported)
                    {
                        item = std::move(imported).value();
                    }
                    else
                    {
                        item.status = ImportItemStatus::kFailed;
                        item.input_path = path;
                        item.error = imported.error();
                    }
                    const auto row = static_cast<int>(import_next_index_);
                    import_results_.push_back(item);
                    ++import_next_index_;
                    setImportWork(static_cast<int>(import_next_index_),
                                  static_cast<int>(pending_import_paths_.size()), true);
                    setStatus(QCoreApplication::translate("StudioPresenter", "Importing %1 / %2…")
                                  .arg(import_next_index_)
                                  .arg(pending_import_paths_.size()));
                    publishImportItem(item, row);
                    startNextImportItem();
                },
                Qt::QueuedConnection);
        });
}

void StudioImportWorkspace::finishImportBatch()
{
    if (!import_work_active_)
        return;
    import_gallery_placeholders_ = false;
    import_skip_existing_ = false;
    pending_import_content_hashes_.clear();
    import_defer_previews_ = false;
    const bool cancelled = import_operation_.token().is_cancellation_requested();
    const auto completed = import_results_.size();
    const auto total = pending_import_paths_.size();
    const auto generation = import_generation_;
    auto results = std::move(import_results_);
    pending_import_paths_.clear();
    import_next_index_ = 0U;
    LibraryQuery query = import_query_snapshot_;
    std::optional<std::int64_t> imported_after;
    std::optional<std::int64_t> imported_before;
    std::size_t imported_count = 0U;
    for (const auto &item : results)
    {
        if (item.status != ImportItemStatus::kImported || !item.asset)
            continue;
        const auto created = item.asset->created_unix_ms;
        imported_after = imported_after ? std::min(*imported_after, created) : created;
        imported_before = imported_before ? std::max(*imported_before, created) : created;
        ++imported_count;
    }
    if (imported_count > 0U)
    {
        query.folder_uri.clear();
        query.imported_after_unix_ms = imported_after;
        query.imported_before_unix_ms = imported_before;
    }
    host_.finish_batch(BatchCompletion{generation, std::move(results), query, imported_after,
                                       imported_before, imported_count, cancelled, completed, total,
                                       import_preference_error_});
}

void StudioImportWorkspace::publishImportItem(const ImportItemResult &item, const int row)
{
    if (import_gallery_placeholders_ && item.status == ImportItemStatus::kImported && item.asset)
    {
        if (!pending_import_destination_.isEmpty() && !import_destination_remembered_)
        {
            import_destination_remembered_ = true;
            const auto remembered =
                StudioImportPreferences{}.rememberDestination(pending_import_destination_);
            if (!remembered)
            {
                import_preference_error_ = QCoreApplication::translate(
                    "StudioPresenter",
                    "Photos imported, but the destination could not be remembered.");
                setError(import_preference_error_);
            }
        }
        if (item.preview_pending)
            pending_import_preview_ids_.push_back(item.asset->id);
    }
    host_.publish_item(item, row);
}

void StudioImportWorkspace::beginImportGalleryPlaceholders(const std::vector<std::string> &paths)
{
    import_gallery_placeholders_ = true;
    import_page_open_ = false;
    emit importPageChanged();
    host_.begin_placeholders(paths);
}
} // namespace ravo
