#pragma once

#include <memory>

#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_draft.h"
#include "studio_import_destination_preview_controller.h"
#include "studio_import_scan_controller.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_import_worker.h"

namespace ravo
{

// Assembles Import desktop owners. Presenter Import getters/commands forward here.
//
// Ownership (UI thread unless noted):
//   draft                 — configuration only; no live selection
//   scan                  — generation/busy/progress/revision + scan cancel + orchestration
//   thumbnails            — decode executor/engine/pending (own SerialExecutor)
//   destination_preview   — debounce timer/generation/key/published folders
//   candidates/source_folders/destination_folders — workspace-owned GUI models
//   worker                — catalog/Engine owner thread; controllers borrow via Host
//
// Shutdown order: destination_preview.shutdown → thumbnails.shutdown → scan.abandon
// before import executor stop. Async receivers are Presenter (UI) unless noted.
struct StudioImportWorkspace
{
    explicit StudioImportWorkspace(QObject *parent = nullptr)
        : candidates(parent)
        , source_folders(parent)
        , destination_folders(parent)
    {
    }

    std::unique_ptr<StudioImportWorker> worker = std::make_unique<StudioImportWorker>();
    ImportCandidateListModel candidates;
    FilesystemBrowserModel source_folders;
    FilesystemBrowserModel destination_folders;
    ImportDraft draft;
    std::unique_ptr<StudioImportScanController> scan;
    std::unique_ptr<StudioImportThumbnailController> thumbnails;
    std::unique_ptr<StudioImportDestinationPreviewController> destination_preview;

    void shutdown()
    {
        if (destination_preview)
            destination_preview->shutdown();
        if (thumbnails)
            thumbnails->shutdown();
        if (scan)
            scan->abandon("workspace_shutdown");
    }
};

} // namespace ravo
