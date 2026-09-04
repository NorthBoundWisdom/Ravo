#include "ravo/desktop/studio_presenter.h"

#include "studio_qt.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/services/ai_proposal.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap field_change_map(const AiProposalFieldChange &change)
{
    QVariantMap map{{QStringLiteral("field"), qstring_from_utf8(change.field)},
                    {QStringLiteral("value"), change.value}};
    if (change.confidence)
        map.insert(QStringLiteral("confidence"), *change.confidence);
    return map;
}

[[nodiscard]] QVariantMap field_diff_map(const DevelopChange &change)
{
    return QVariantMap{{QStringLiteral("field"), qstring_from_utf8(change.field)},
                       {QStringLiteral("value"), qstring_from_utf8(change.value)}};
}

[[nodiscard]] QVariantMap alternative_map(const AiProposalAlternative &alternative)
{
    QVariantList fields;
    fields.reserve(static_cast<qsizetype>(alternative.fields.size()));
    for (const auto &field : alternative.fields)
        fields.push_back(field_change_map(field));
    QVariantMap map{{QStringLiteral("label"), qstring_from_utf8(alternative.label)},
                    {QStringLiteral("fields"), fields}};
    if (alternative.confidence)
        map.insert(QStringLiteral("confidence"), *alternative.confidence);
    return map;
}

[[nodiscard]] QVariantMap proposal_map(const AiProposal &proposal)
{
    QVariantList fields;
    fields.reserve(static_cast<qsizetype>(proposal.fields.size()));
    for (const auto &field : proposal.fields)
        fields.push_back(field_change_map(field));
    QVariantList diffs;
    diffs.reserve(static_cast<qsizetype>(proposal.field_diff.size()));
    for (const auto &diff : proposal.field_diff)
        diffs.push_back(field_diff_map(diff));
    QVariantList alternatives;
    alternatives.reserve(static_cast<qsizetype>(proposal.alternatives.size()));
    for (const auto &alternative : proposal.alternatives)
        alternatives.push_back(alternative_map(alternative));
    QVariantMap provider{
        {QStringLiteral("providerId"), qstring_from_utf8(proposal.provider.provider_id)},
        {QStringLiteral("modelId"), qstring_from_utf8(proposal.provider.model_id)},
        {QStringLiteral("modelVersion"), qstring_from_utf8(proposal.provider.model_version)},
        {QStringLiteral("weightContentHash"),
         qstring_from_utf8(proposal.provider.weight_content_hash)}};
    QVariantMap map{
        {QStringLiteral("id"), qstring_from_utf8(proposal.id)},
        {QStringLiteral("contractVersion"), qstring_from_utf8(proposal.contract_version)},
        {QStringLiteral("kind"),
         qstring_from_utf8(std::string(ai_proposal_kind_name(proposal.kind)))},
        {QStringLiteral("status"),
         qstring_from_utf8(std::string(ai_proposal_status_name(proposal.status)))},
        {QStringLiteral("createdUnixMs"),
         QVariant::fromValue(static_cast<qlonglong>(proposal.created_unix_ms))},
        {QStringLiteral("assetId"), qstring_from_utf8(proposal.asset_id)},
        {QStringLiteral("observedCatalogRevision"),
         QVariant::fromValue(static_cast<qlonglong>(proposal.observed_catalog_revision))},
        {QStringLiteral("observedRecoveryGeneration"),
         QVariant::fromValue(static_cast<qlonglong>(proposal.observed_recovery_generation))},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("fields"), fields},
        {QStringLiteral("fieldDiff"), diffs},
        {QStringLiteral("alternatives"), alternatives},
        {QStringLiteral("pending"), proposal.status == AiProposalStatus::kPending},
    };
    if (proposal.semantic_label)
        map.insert(QStringLiteral("semanticLabel"), qstring_from_utf8(*proposal.semantic_label));
    if (proposal.reference_asset_id)
        map.insert(QStringLiteral("referenceAssetId"),
                   qstring_from_utf8(*proposal.reference_asset_id));
    if (proposal.applied_history_id)
        map.insert(QStringLiteral("appliedHistoryId"),
                   QVariant::fromValue(static_cast<qlonglong>(*proposal.applied_history_id)));
    return map;
}

[[nodiscard]] Result<AiProposalKind> kind_from_text(const QString &raw)
{
    return parse_ai_proposal_kind(utf8_from_qstring(raw.trimmed().toLower()));
}

} // namespace

QVariantMap StudioPresenter::selectedAiProposal() const
{
    return selected_ai_proposal_;
}

QVariantList StudioPresenter::aiProposals() const
{
    return ai_proposals_;
}

void StudioPresenter::clearSelectedAiProposal()
{
    if (selected_ai_proposal_.isEmpty())
        return;
    selected_ai_proposal_.clear();
    emit selectedAiProposalChanged();
}

void StudioPresenter::refreshAiProposals()
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Refreshing AI proposals…"));
    executor_.post(
        [this, asset_id]() mutable
        {
            Result<std::vector<AiProposal>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                listed = service_->list_ai_proposals(asset_id);
            QMetaObject::invokeMethod(
                this,
                [this, listed = std::move(listed)]() mutable
                {
                    setBusy(false);
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "AI proposal refresh failed."));
                        return;
                    }
                    QVariantList rows;
                    rows.reserve(static_cast<qsizetype>(listed.value().size()));
                    std::optional<QVariantMap> keep_selected;
                    const auto selected_id =
                        selected_ai_proposal_.value(QStringLiteral("id")).toString();
                    for (const auto &proposal : listed.value())
                    {
                        auto map = proposal_map(proposal);
                        if (!selected_id.isEmpty() &&
                            map.value(QStringLiteral("id")).toString() == selected_id)
                            keep_selected = map;
                        rows.push_back(map);
                    }
                    ai_proposals_ = std::move(rows);
                    emit aiProposalsChanged();
                    if (keep_selected)
                    {
                        selected_ai_proposal_ = *keep_selected;
                        emit selectedAiProposalChanged();
                    }
                    else if (!selected_id.isEmpty())
                    {
                        clearSelectedAiProposal();
                    }
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Loaded %1 AI proposal(s).")
                            .arg(ai_proposals_.size()));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createAiStubProposal(const QString &kind_text, const QString &semantic_label)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    auto kind = kind_from_text(kind_text.isEmpty() ? QStringLiteral("global") : kind_text);
    if (!kind)
    {
        setError(qstring_from_utf8(kind.error().message));
        return;
    }
    AiProposalCreateRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.user_initiated = true;
    request.kind = kind.value();
    if (request.kind == AiProposalKind::kSemanticMask)
    {
        const auto label = semantic_label.trimmed().isEmpty() ? QStringLiteral("subject") :
                                                                semantic_label.trimmed();
        request.semantic_label = utf8_from_qstring(label);
        request.model_id = std::string(kAiStubSemanticMaskModelId);
    }
    if (observed_catalog_revision_ >= 0)
        request.expected_catalog_revision = observed_catalog_revision_;
    request.cancellation = shutdown_.token();

    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Creating stub AI proposal…"));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<AiProposal> created = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                created = service_->create_ai_proposal(request);
            QMetaObject::invokeMethod(
                this,
                [this, created = std::move(created)]() mutable
                {
                    setBusy(false);
                    if (!created)
                    {
                        setError(qstring_from_utf8(created.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "AI proposal create failed."));
                        return;
                    }
                    selected_ai_proposal_ = proposal_map(created.value());
                    emit selectedAiProposalChanged();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter",
                                                    "Created stub AI proposal %1")
                            .arg(selected_ai_proposal_.value(QStringLiteral("id")).toString()));
                    refreshAiProposals();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::selectAiProposal(const QString &proposal_id)
{
    const auto id = proposal_id.trimmed();
    if (id.isEmpty())
    {
        clearSelectedAiProposal();
        return;
    }
    for (const auto &row : ai_proposals_)
    {
        const auto map = row.toMap();
        if (map.value(QStringLiteral("id")).toString() == id)
        {
            selected_ai_proposal_ = map;
            emit selectedAiProposalChanged();
            return;
        }
    }
    if (busy_ || catalog_path_.isEmpty())
        return;
    setBusy(true);
    setError({});
    executor_.post(
        [this, proposal_id = utf8_from_qstring(id)]() mutable
        {
            Result<AiProposal> loaded = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                loaded = service_->get_ai_proposal(proposal_id);
            QMetaObject::invokeMethod(
                this,
                [this, loaded = std::move(loaded)]() mutable
                {
                    setBusy(false);
                    if (!loaded)
                    {
                        setError(qstring_from_utf8(loaded.error().message));
                        return;
                    }
                    selected_ai_proposal_ = proposal_map(loaded.value());
                    emit selectedAiProposalChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::applySelectedAiProposal()
{
    if (busy_ || catalog_path_.isEmpty())
        return;
    const auto id = selected_ai_proposal_.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select an AI proposal first."));
        return;
    }
    std::optional<std::int64_t> expected;
    if (observed_catalog_revision_ >= 0)
        expected = observed_catalog_revision_;
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Applying AI proposal…"));
    executor_.post(
        [this, proposal_id = utf8_from_qstring(id), expected]() mutable
        {
            Result<AiProposalApplyResult> applied =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                applied = service_->apply_ai_proposal(proposal_id, expected);
            QMetaObject::invokeMethod(
                this,
                [this, applied = std::move(applied)]() mutable
                {
                    setBusy(false);
                    if (!applied)
                    {
                        setError(qstring_from_utf8(applied.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "AI proposal apply failed."));
                        return;
                    }
                    selected_ai_proposal_ = proposal_map(applied.value().proposal);
                    emit selectedAiProposalChanged();
                    if (applied.value().revision >= 0)
                        observed_catalog_revision_ = applied.value().revision;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Applied AI proposal."));
                    load_develop_for_selection();
                    refreshAiProposals();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::rejectSelectedAiProposal()
{
    if (busy_ || catalog_path_.isEmpty())
        return;
    const auto id = selected_ai_proposal_.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select an AI proposal first."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Rejecting AI proposal…"));
    executor_.post(
        [this, proposal_id = utf8_from_qstring(id)]() mutable
        {
            Result<AiProposal> rejected = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                rejected = service_->reject_ai_proposal(proposal_id);
            QMetaObject::invokeMethod(
                this,
                [this, rejected = std::move(rejected)]() mutable
                {
                    setBusy(false);
                    if (!rejected)
                    {
                        setError(qstring_from_utf8(rejected.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "AI proposal reject failed."));
                        return;
                    }
                    selected_ai_proposal_ = proposal_map(rejected.value());
                    emit selectedAiProposalChanged();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Rejected AI proposal."));
                    refreshAiProposals();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::cancelSelectedAiProposal()
{
    if (busy_ || catalog_path_.isEmpty())
        return;
    const auto id = selected_ai_proposal_.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select an AI proposal first."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Cancelling AI proposal…"));
    executor_.post(
        [this, proposal_id = utf8_from_qstring(id)]() mutable
        {
            Result<AiProposal> cancelled = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                cancelled = service_->cancel_ai_proposal(proposal_id);
            QMetaObject::invokeMethod(
                this,
                [this, cancelled = std::move(cancelled)]() mutable
                {
                    setBusy(false);
                    if (!cancelled)
                    {
                        setError(qstring_from_utf8(cancelled.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "AI proposal cancel failed."));
                        return;
                    }
                    selected_ai_proposal_ = proposal_map(cancelled.value());
                    emit selectedAiProposalChanged();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Cancelled AI proposal."));
                    refreshAiProposals();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
