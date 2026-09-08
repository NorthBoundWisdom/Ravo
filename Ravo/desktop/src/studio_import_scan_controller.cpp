#include "studio_import_scan_controller.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>

#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/foundation/error.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{
[[nodiscard]] ImportCandidate placeholder_candidate(const std::string &path,
                                                    const std::string &source_root)
{
    ImportCandidate candidate;
    candidate.source_path = path;
    const QString qpath = qstring_from_utf8(path);
    candidate.display_name = utf8_from_qstring(QFileInfo(qpath).fileName());
    if (!source_root.empty())
    {
        const QString relative = QDir(qstring_from_utf8(source_root)).relativeFilePath(qpath);
        if (!relative.isEmpty() && !relative.startsWith(QLatin1String("..")))
            candidate.relative_path = utf8_from_qstring(QDir::fromNativeSeparators(relative));
    }
    if (candidate.relative_path.empty())
        candidate.relative_path = candidate.display_name;
    return candidate;
}
} // namespace

StudioImportScanController::StudioImportScanController(Host host, QObject *parent)
    : QObject(parent)
    , host_(std::move(host))
{
}

StudioImportScanController::StudioImportScanController(QObject *parent)
    : QObject(parent)
{
}

CancellationToken StudioImportScanController::begin(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
    active_ = true;
    resetProgress();
    return operation_.token();
}

void StudioImportScanController::finish()
{
    active_ = false;
}

void StudioImportScanController::abandon(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
    active_ = false;
    resetProgress();
}

void StudioImportScanController::bumpGeneration(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
}

void StudioImportScanController::cancel(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
}

void StudioImportScanController::setProgress(int completed, int total, int duplicates)
{
    completed_ = completed;
    total_ = total;
    duplicate_count_ = duplicates;
}

void StudioImportScanController::setTotal(int total)
{
    total_ = total;
}

void StudioImportScanController::setCatalogRevision(std::optional<std::int64_t> revision)
{
    catalog_revision_ = std::move(revision);
}

void StudioImportScanController::resetProgress()
{
    completed_ = 0;
    total_ = 0;
    duplicate_count_ = 0;
    catalog_revision_.reset();
}

void StudioImportScanController::startRescan()
{
    if (host_.executor == nullptr || host_.callback_receiver == nullptr)
        return;
    if (host_.work_active && host_.work_active())
        return;
    const QString source = host_.source_root ? host_.source_root() : QString{};
    if (source.isEmpty())
        return;
    if (host_.prepare_thumbnails_for_rescan)
        host_.prepare_thumbnails_for_rescan();
    if (host_.clear_preflight_active)
        host_.clear_preflight_active();
    const auto token = begin("import_source_changed");
    const auto generation = generation_;
    const std::string root = utf8_from_qstring(source);
    const bool recursive = host_.recursive_for_root ? host_.recursive_for_root(source) : true;
    if (auto *model = host_.model ? host_.model() : nullptr)
        model->setCandidates({});
    if (host_.set_error)
        host_.set_error({});
    if (host_.emit_page_changed)
        host_.emit_page_changed();

    const bool queued = host_.executor->post(
        [this, root, recursive, generation, token]()
        {
            std::vector<ImportCandidate> pending;
            int duplicates = 0;
            const QPointer<StudioImportScanController> self(this);
            const auto publish =
                [self, generation, &pending, &duplicates](std::size_t completed, std::size_t total,
                                                          const ImportCandidate &candidate)
            {
                if (candidate.duplicate)
                    ++duplicates;
                pending.push_back(candidate);
                if (completed != 1 && completed % 32 != 0 && completed != total)
                    return;
                if (!self || self->host_.callback_receiver == nullptr)
                {
                    pending.clear();
                    return;
                }
                QMetaObject::invokeMethod(
                    self->host_.callback_receiver,
                    [self, generation, completed, total, duplicates,
                     batch = std::move(pending)]() mutable
                    {
                        if (!self || !self->matches(generation))
                            return;
                        if (self->host_.page_open && !self->host_.page_open())
                            return;
                        auto *model = self->host_.model ? self->host_.model() : nullptr;
                        if (!model)
                            return;
                        const int first = static_cast<int>(completed - batch.size());
                        model->applyScanBatch(first, std::move(batch));
                        self->setProgress(static_cast<int>(completed), static_cast<int>(total),
                                          duplicates);
                        if (self->host_.emit_page_changed)
                            self->host_.emit_page_changed();
                    },
                    Qt::QueuedConnection);
                pending.clear();
            };
            const auto scan_source = [&]() -> Result<ImportScanResult>
            {
                if (auto active = token.check(); !active)
                    return active.error();
                auto *service = self && self->host_.service ? self->host_.service() : nullptr;
                if (service == nullptr)
                    return make_error(ErrorCode::kIo, "Catalog session is closed");
                const QFileInfo source_info(qstring_from_utf8(root));
                if (!source_info.isDir() || !source_info.isReadable())
                    return make_error(ErrorCode::kIo,
                                      "Import source folder is unavailable: " + root,
                                      {{"reason", "import_source_unavailable"}});
                return service->import().scan_import_candidates(
                    {root}, root, recursive, token, publish,
                    [self, root, generation](const std::vector<std::string> &paths)
                    {
                        std::vector<ImportCandidate> placeholders;
                        placeholders.reserve(paths.size());
                        for (const auto &path : paths)
                            placeholders.push_back(placeholder_candidate(path, root));
                        if (!self || self->host_.callback_receiver == nullptr)
                            return;
                        QMetaObject::invokeMethod(
                            self->host_.callback_receiver,
                            [self, generation, placeholders = std::move(placeholders)]() mutable
                            {
                                if (!self || !self->matches(generation))
                                    return;
                                if (self->host_.page_open && !self->host_.page_open())
                                    return;
                                auto *model = self->host_.model ? self->host_.model() : nullptr;
                                if (!model)
                                    return;
                                self->setTotal(static_cast<int>(placeholders.size()));
                                model->setCandidates(std::move(placeholders), true);
                                if (self->host_.emit_page_changed)
                                    self->host_.emit_page_changed();
                            },
                            Qt::QueuedConnection);
                    });
            };
            auto scan = scan_source();
            if (!self || self->host_.callback_receiver == nullptr)
                return;
            QMetaObject::invokeMethod(
                self->host_.callback_receiver,
                [self, generation, scan = std::move(scan)]() mutable
                {
                    if (!self || !self->matches(generation))
                        return;
                    if (self->host_.page_open && !self->host_.page_open())
                        return;
                    self->finish();
                    if (!scan)
                    {
                        if (self->host_.set_error)
                            self->host_.set_error(qstring_from_utf8(scan.error().message));
                    }
                    else
                        self->setCatalogRevision(scan.value().catalog_revision);
                    if (self->host_.emit_page_changed)
                        self->host_.emit_page_changed();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        finish();
        if (host_.set_error)
            host_.set_error(QStringLiteral("Import scan worker is stopped."));
        if (host_.emit_page_changed)
            host_.emit_page_changed();
    }
}

} // namespace ravo
