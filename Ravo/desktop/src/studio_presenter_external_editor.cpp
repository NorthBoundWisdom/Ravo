#include "ravo/desktop/studio_presenter.h"

#include "studio_file_manager.h"
#include "studio_qt.h"

#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>
#include <QVariantMap>

#include "ravo/domain/types.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/external_editor.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap fingerprint_map(const ExternalEditorFileFingerprint &value)
{
    return QVariantMap{{QStringLiteral("sha256"), qstring_from_utf8(value.sha256)},
                       {QStringLiteral("sizeBytes"),
                        QVariant::fromValue(static_cast<qulonglong>(value.size_bytes))},
                       {QStringLiteral("mtimeUnixMs"),
                        QVariant::fromValue(static_cast<qlonglong>(value.mtime_unix_ms))}};
}

[[nodiscard]] QVariantMap session_map(const ExternalEditorWorkingCopySession &session)
{
    QVariantMap map{
        {QStringLiteral("schema"), qstring_from_utf8(session.schema)},
        {QStringLiteral("schemaVersion"),
         QVariant::fromValue(static_cast<qlonglong>(session.schema_version))},
        {QStringLiteral("workingCopyId"), qstring_from_utf8(session.working_copy_id)},
        {QStringLiteral("sourceAssetId"), qstring_from_utf8(session.source_asset_id)},
        {QStringLiteral("editorId"), qstring_from_utf8(session.editor_id)},
        {QStringLiteral("workingPath"), qstring_from_utf8(session.working_path)},
        {QStringLiteral("workingUri"), qstring_from_utf8(session.working_uri)},
        {QStringLiteral("tiffSampleType"),
         qstring_from_utf8(tiff_sample_type_name(session.tiff_sample_type))},
        {QStringLiteral("profile"), qstring_from_utf8(session.profile)},
        {QStringLiteral("autoStack"), session.auto_stack},
        {QStringLiteral("createdUnixMs"),
         QVariant::fromValue(static_cast<qlonglong>(session.created_unix_ms))},
        {QStringLiteral("observedCatalogRevision"),
         QVariant::fromValue(static_cast<qlonglong>(session.observed_catalog_revision))},
        {QStringLiteral("sourceOriginal"), fingerprint_map(session.source_original)},
        {QStringLiteral("workingCopy"), fingerprint_map(session.working_copy)}};
    if (session.editor_version)
        map.insert(QStringLiteral("editorVersion"), qstring_from_utf8(*session.editor_version));
    if (session.max_edge)
        map.insert(QStringLiteral("maxEdge"), QVariant::fromValue(*session.max_edge));
    if (session.open_intent_id)
        map.insert(QStringLiteral("openIntentId"), qstring_from_utf8(*session.open_intent_id));
    return map;
}

[[nodiscard]] Result<TiffSampleType> sample_type_from_options(const QVariantMap &options)
{
    const auto raw = options.value(QStringLiteral("tiffSampleType"), QStringLiteral("uint16"))
                         .toString()
                         .trimmed()
                         .toStdString();
    return parse_tiff_sample_type(raw);
}

} // namespace

QVariantMap StudioPresenter::externalEditorSession() const
{
    return external_editor_session_;
}

QVariantMap StudioPresenter::externalEditorDefaultOptions() const
{
    return QVariantMap{{QStringLiteral("editorId"), QStringLiteral("external")},
                       {QStringLiteral("editorVersion"), QString{}},
                       {QStringLiteral("applicationPath"), QString{}},
                       {QStringLiteral("tiffSampleType"), QStringLiteral("uint16")},
                       {QStringLiteral("profile"), QStringLiteral("srgb")},
                       {QStringLiteral("maxEdge"), 0},
                       {QStringLiteral("autoStack"), true},
                       {QStringLiteral("openAfterCreate"), true}};
}

QVariantList StudioPresenter::externalEditorTiffSampleTypeChoices() const
{
    return tiffSampleTypeChoices();
}

void StudioPresenter::clearExternalEditorSession()
{
    if (external_editor_session_.isEmpty())
        return;
    external_editor_session_.clear();
    emit externalEditorSessionChanged();
}

void StudioPresenter::prepareExternalEditorWorkingCopy(const QVariantMap &options)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;

    const auto editor_id = options.value(QStringLiteral("editorId")).toString().trimmed();
    if (editor_id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "External editor id must not be empty."));
        return;
    }
    auto sample_type = sample_type_from_options(options);
    if (!sample_type)
    {
        setError(qstring_from_utf8(sample_type.error().message));
        return;
    }
    const auto profile = options.value(QStringLiteral("profile"), QStringLiteral("srgb"))
                             .toString()
                             .trimmed()
                             .toLower();
    if (profile != QStringLiteral("srgb"))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Edit in… v1 only supports the sRGB working-copy profile."));
        return;
    }

    ExternalEditorWorkingCopyRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.editor_id = utf8_from_qstring(editor_id);
    const auto editor_version = options.value(QStringLiteral("editorVersion")).toString().trimmed();
    if (!editor_version.isEmpty())
        request.editor_version = utf8_from_qstring(editor_version);
    const auto application_path =
        options.value(QStringLiteral("applicationPath")).toString().trimmed();
    if (!application_path.isEmpty())
        request.application_path = utf8_from_qstring(application_path);
    request.tiff_sample_type = sample_type.value();
    request.profile = "srgb";
    const auto max_edge = options.value(QStringLiteral("maxEdge"), 0).toUInt();
    if (max_edge > 0U)
        request.max_edge = max_edge;
    request.auto_stack = options.value(QStringLiteral("autoStack"), true).toBool();
    request.user_initiated = true;
    if (observed_catalog_revision_ >= 0)
        request.expected_catalog_revision = observed_catalog_revision_;
    request.cancellation = shutdown_.token();
    const bool open_after = options.value(QStringLiteral("openAfterCreate"), true).toBool();

    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Preparing Edit in… working copy…"));
    executor_.post(
        [this, request = std::move(request), open_after, application_path]() mutable
        {
            Result<ExternalEditorWorkingCopyResult> prepared =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                prepared = service_->create_external_editor_working_copy(request);
            QMetaObject::invokeMethod(
                this,
                [this, prepared = std::move(prepared), open_after, application_path]() mutable
                {
                    setBusy(false);
                    if (!prepared)
                    {
                        setError(qstring_from_utf8(prepared.error().message));
                        setStatus(QCoreApplication::translate(
                            "StudioPresenter", "Edit in… working-copy prepare failed."));
                        return;
                    }
                    external_editor_session_ = session_map(prepared.value().session);
                    emit externalEditorSessionChanged();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter",
                                                    "Prepared Edit in… working copy %1")
                            .arg(QFileInfo(qstring_from_utf8(prepared.value().session.working_path))
                                     .fileName()));
                    if (open_after)
                    {
                        openExternalEditorWorkingCopy(
                            qstring_from_utf8(prepared.value().session.working_path),
                            application_path);
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::openExternalEditorWorkingCopy(const QString &working_path,
                                                    const QString &application_path)
{
    const auto launch = file_open_with_launch(working_path, application_path);
    if (!launch)
    {
        setError(qstring_from_utf8(launch.error().message));
        return;
    }
    if (!start_file_open(launch.value()))
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "The external editor could not be opened."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Opened working copy in editor."));
}

void StudioPresenter::checkExternalEditorReturned(const QString &working_copy_id,
                                                  const QString &returned_path)
{
    if (busy_ || catalog_path_.isEmpty())
        return;
    QString id = working_copy_id.trimmed();
    if (id.isEmpty())
        id = external_editor_session_.value(QStringLiteral("workingCopyId")).toString().trimmed();
    if (id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "No Edit in… working-copy session is active."));
        return;
    }

    ExternalEditorCheckReturnedRequest request;
    request.working_copy_id = utf8_from_qstring(id);
    const auto path = returned_path.trimmed();
    if (!path.isEmpty())
        request.returned_path = utf8_from_qstring(path);
    if (observed_catalog_revision_ >= 0)
        request.expected_catalog_revision = observed_catalog_revision_;
    request.cancellation = shutdown_.token();

    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Checking returned editor output…"));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<ExternalEditorCheckReturnedResult> checked =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                checked = service_->check_external_editor_returned(request);
            QMetaObject::invokeMethod(
                this,
                [this, checked = std::move(checked)]() mutable
                {
                    setBusy(false);
                    if (!checked)
                    {
                        setError(qstring_from_utf8(checked.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Edit in… check-returned failed."));
                        return;
                    }
                    external_editor_session_ = session_map(checked.value().session);
                    external_editor_session_.insert(
                        QStringLiteral("derivedAssetId"),
                        qstring_from_utf8(checked.value().registration.derived_asset.id));
                    external_editor_session_.insert(QStringLiteral("autoStacked"),
                                                    checked.value().registration.auto_stacked);
                    external_editor_session_.insert(QStringLiteral("registered"), true);
                    emit externalEditorSessionChanged();
                    observed_catalog_revision_ =
                        checked.value().registration.provenance.observed_catalog_revision;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter",
                                                    "Registered returned editor output as %1")
                            .arg(qstring_from_utf8(checked.value().registration.derived_asset.id)));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
