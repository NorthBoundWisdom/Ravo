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
    request.source_root = utf8_from_qstring(import_source_root_);
    request.mode = import_mode_ == QLatin1String("copy") ? ImportTransferMode::kCopy :
                   import_mode_ == QLatin1String("move") ? ImportTransferMode::kMove :
                                                           ImportTransferMode::kAdd;
    request.organization =
        import_organization_ == QLatin1String("hierarchy") ?
            ImportOrganization::kPreserveHierarchy :
        import_organization_ == QLatin1String("date")  ? ImportOrganization::kCaptureDate :
        import_organization_ == QLatin1String("month") ? ImportOrganization::kCaptureMonth :
                                                         ImportOrganization::kSingleFolder;
    request.preview =
        import_preview_policy_ == QLatin1String("minimal")    ? ImportPreviewPolicy::kMinimal :
        import_preview_policy_ == QLatin1String("one-to-one") ? ImportPreviewPolicy::kOneToOne :
                                                                ImportPreviewPolicy::kStandard;
    if (request.mode != ImportTransferMode::kAdd)
    {
        request.destination_directory = utf8_from_qstring(import_destination_);
        request.filename_template = utf8_from_qstring(import_filename_template_);
        request.second_copy_directory = utf8_from_qstring(import_second_copy_destination_);
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
        import_mode_ != QLatin1String("add") && !import_destination_.isEmpty() &&
        import_destination_error_.isEmpty() && import_candidates_.selectedCount() > 0)
    {
        key = QJsonDocument(
                  QJsonObject{
                      {QStringLiteral("catalog"), catalog_path_},
                      {QStringLiteral("revision"), QString::number(*import_scan_catalog_revision_)},
                      {QStringLiteral("source"), import_source_root_},
                      {QStringLiteral("destination"), import_destination_},
                      {QStringLiteral("second"), import_second_copy_destination_},
                      {QStringLiteral("mode"), import_mode_},
                      {QStringLiteral("organization"), import_organization_},
                      {QStringLiteral("name"), import_filename_template_},
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
