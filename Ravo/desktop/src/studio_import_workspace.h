#pragma once

#include <memory>

#include "ravo/desktop/studio_import_draft.h"
#include "studio_import_destination_preview_controller.h"
#include "studio_import_scan_controller.h"
#include "studio_import_thumbnail_controller.h"

namespace ravo
{

// Assembles Import desktop owners. Presenter Import getters/commands forward here.
//
// Ownership (UI thread unless noted):
//   draft                 — configuration only; no live selection
//   scan                  — generation/busy/progress/revision + scan cancel token
//   thumbnails            — decode executor/engine/pending (own SerialExecutor)
//   destination_preview   — debounce timer/generation/key/published folders
//   ImportCandidateListModel — remains on Presenter (Q_PROPERTY model owner)
//   CatalogService/engine — Presenter catalog executor; controllers borrow via Host
//
// Shutdown order: destination_preview.shutdown → thumbnails.shutdown → scan.abandon
// before catalog executor stop. Async receivers are Presenter (UI) unless noted.
struct StudioImportWorkspace
{
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
