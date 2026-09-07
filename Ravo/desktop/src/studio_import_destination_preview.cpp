#include "ravo/desktop/studio_presenter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "studio_import_destination_preview_controller.h"
#include "studio_import_scan_controller.h"
#include "studio_qt.h"

namespace ravo
{

QVariantList StudioPresenter::importDestinationPreview() const
{
    return import_destination_preview_ ? import_destination_preview_->folders() : QVariantList{};
}

QString StudioPresenter::importDestinationPreviewError() const
{
    return import_destination_preview_ ? import_destination_preview_->error() : QString{};
}

bool StudioPresenter::importDestinationPreviewActive() const
{
    return import_destination_preview_ && import_destination_preview_->active();
}

ImportRequest StudioPresenter::plannedImportRequest() const
{
    ImportRequest request;
    for (const auto &path : import_candidates_.selectedPaths())
        request.inputs.push_back(utf8_from_qstring(path));
    request.source_root = utf8_from_qstring(import_draft_.source_root);
    request.mode = import_draft_.mode == QLatin1String("copy") ? ImportTransferMode::kCopy :
                   import_draft_.mode == QLatin1String("move") ? ImportTransferMode::kMove :
                                                                 ImportTransferMode::kAdd;
    request.organization =
        import_draft_.organization == QLatin1String("hierarchy") ?
            ImportOrganization::kPreserveHierarchy :
        import_draft_.organization == QLatin1String("date")  ? ImportOrganization::kCaptureDate :
        import_draft_.organization == QLatin1String("month") ? ImportOrganization::kCaptureMonth :
                                                               ImportOrganization::kSingleFolder;
    request.preview = import_draft_.preview_policy == QLatin1String("minimal") ?
                          ImportPreviewPolicy::kMinimal :
                      import_draft_.preview_policy == QLatin1String("one-to-one") ?
                          ImportPreviewPolicy::kOneToOne :
                          ImportPreviewPolicy::kStandard;
    if (request.mode != ImportTransferMode::kAdd)
    {
        request.destination_directory = utf8_from_qstring(import_draft_.destination);
        request.filename_template = utf8_from_qstring(import_draft_.filename_pattern);
        request.second_copy_directory = utf8_from_qstring(import_draft_.second_copy_destination);
    }
    request.recursive = false;
    request.defer_previews = true;
    request.skip_existing = true;
    request.expected_catalog_revision =
        import_scan_ ? import_scan_->catalogRevision() : std::nullopt;
    request.expected_content_hashes = import_candidates_.selectedContentHashes();
    request.cancellation = import_operation_.token();
    return request;
}

void StudioPresenter::refreshImportDestinationPreview()
{
    if (import_destination_preview_)
        import_destination_preview_->refresh();
}

void StudioPresenter::startImportDestinationPreview()
{
    // Debounced start is owned by StudioImportDestinationPreviewController.
}
} // namespace ravo
