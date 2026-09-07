#include "ravo/desktop/studio_presenter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include "studio_qt.h"

namespace ravo
{
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
    request.expected_catalog_revision = import_scan_catalog_revision_;
    request.expected_content_hashes = import_candidates_.selectedContentHashes();
    request.cancellation = import_operation_.token();
    return request;
}

void StudioPresenter::refreshImportDestinationPreview()
{
    QByteArray key;
    if (import_page_open_ && !import_scan_active_ && !import_work_active_ &&
        !import_preflight_active_ && import_scan_catalog_revision_ &&
        import_draft_.mode != QLatin1String("add") && !import_draft_.destination.isEmpty() &&
        import_draft_.destination_error.isEmpty() && import_candidates_.selectedCount() > 0)
    {
        key = QJsonDocument(
                  QJsonObject{
                      {QStringLiteral("catalog"), catalog_path_},
                      {QStringLiteral("revision"), QString::number(*import_scan_catalog_revision_)},
                      {QStringLiteral("source"), import_draft_.source_root},
                      {QStringLiteral("destination"), import_draft_.destination},
                      {QStringLiteral("second"), import_draft_.second_copy_destination},
                      {QStringLiteral("mode"), import_draft_.mode},
                      {QStringLiteral("organization"), import_draft_.organization},
                      {QStringLiteral("name"), import_draft_.filename_pattern},
                      {QStringLiteral("paths"),
                       QJsonArray::fromStringList(import_candidates_.selectedPaths())}})
                  .toJson(QJsonDocument::Compact);
    }
    if (key == import_destination_preview_key_)
        return;
    import_destination_preview_key_ = std::move(key);
    static_cast<void>(import_destination_preview_operation_.cancel("destination_preview_changed"));
    import_destination_preview_operation_ = CancellationSource{};
    ++import_destination_preview_generation_;
    import_destination_preview_timer_->stop();
    import_destination_preview_.clear();
    import_destination_preview_error_.clear();
    import_destination_preview_active_ = !import_destination_preview_key_.isEmpty();
    if (import_destination_preview_active_)
        import_destination_preview_timer_->start();
    emit importDestinationPreviewChanged();
}

void StudioPresenter::startImportDestinationPreview()
{
    if (!import_destination_preview_active_)
        return;
    auto request = plannedImportRequest();
    request.cancellation = import_destination_preview_operation_.token();
    const auto generation = import_destination_preview_generation_;
    executor_.post(
        [this, request = std::move(request), generation]
        {
            auto preview = service_ ? service_->preview_import_destinations(request) :
                                      Result<ImportDestinationPreview>{
                                          make_error(ErrorCode::kIo, "Catalog session is closed")};
            QMetaObject::invokeMethod(
                this,
                [this, generation, preview = std::move(preview)]
                {
                    if (generation != import_destination_preview_generation_ || !import_page_open_)
                        return;
                    import_destination_preview_active_ = false;
                    if (!preview)
                        import_destination_preview_error_ =
                            qstring_from_utf8(preview.error().message);
                    else
                        for (const auto &folder : preview.value().folders)
                            import_destination_preview_.push_back(QVariantMap{
                                {QStringLiteral("path"), qstring_from_utf8(folder.path)},
                                {QStringLiteral("name"), qstring_from_utf8(folder.name)},
                                {QStringLiteral("depth"), static_cast<int>(folder.depth)},
                                {QStringLiteral("photoCount"),
                                 static_cast<qulonglong>(folder.photo_count)},
                                {QStringLiteral("willCreate"), folder.will_create},
                                {QStringLiteral("secondCopy"), folder.second_copy}});
                    emit importDestinationPreviewChanged();
                },
                Qt::QueuedConnection);
        });
}
} // namespace ravo
