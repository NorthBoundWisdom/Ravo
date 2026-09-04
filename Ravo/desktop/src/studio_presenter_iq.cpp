#include "ravo/desktop/studio_presenter.h"

#include <optional>
#include <string>

#include <QString>
#include <QVariantMap>

#include "ravo/engine/iq_quality_evaluation.h"
#include "ravo/foundation/cancellation.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap task_error_map(const TaskError &error)
{
    QVariantMap context;
    for (const auto &[key, value] : error.context)
        context.insert(qstring_from_utf8(key), qstring_from_utf8(value));
    return QVariantMap{
        {QStringLiteral("ok"), false},
        {QStringLiteral("code"), qstring_from_utf8(std::string(error_code_name(error.code)))},
        {QStringLiteral("message"), qstring_from_utf8(error.message)},
        {QStringLiteral("context"), context},
    };
}

[[nodiscard]] QVariantMap denoise_map(const IqDenoiseEvaluationReport &report)
{
    return QVariantMap{
        {QStringLiteral("schema"), qstring_from_utf8(report.schema)},
        {QStringLiteral("corpusId"), qstring_from_utf8(report.corpus_id)},
        {QStringLiteral("caseId"), qstring_from_utf8(report.case_id)},
        {QStringLiteral("operationId"), qstring_from_utf8(report.operation_id)},
        {QStringLiteral("backend"), qstring_from_utf8(report.backend)},
        {QStringLiteral("supportClaimStatus"), qstring_from_utf8(report.support_claim_status)},
        {QStringLiteral("cpuGoldAligned"), report.cpu_gold_aligned},
        {QStringLiteral("learnedDenoiseAdmitted"), report.learned_denoise_admitted},
        {QStringLiteral("decodeOnly"), report.decode_only},
        {QStringLiteral("strength"), report.strength},
        {QStringLiteral("meanAbsDelta"), report.mean_abs_delta},
        {QStringLiteral("maxAbsDelta"), report.max_abs_delta},
        {QStringLiteral("width"), static_cast<int>(report.width)},
        {QStringLiteral("height"), static_cast<int>(report.height)},
        {QStringLiteral("finite"), report.finite},
    };
}

[[nodiscard]] QVariantMap camera_map(const IqCameraProfileProbeReport &report)
{
    QVariantMap map{
        {QStringLiteral("schema"), qstring_from_utf8(report.schema)},
        {QStringLiteral("corpusId"), qstring_from_utf8(report.corpus_id)},
        {QStringLiteral("caseId"), qstring_from_utf8(report.case_id)},
        {QStringLiteral("probe"), qstring_from_utf8(report.probe)},
        {QStringLiteral("supportClaimStatus"), qstring_from_utf8(report.support_claim_status)},
        {QStringLiteral("documentPresent"), report.document_present},
        {QStringLiteral("colourAccuracyClosed"), report.colour_accuracy_closed},
        {QStringLiteral("decodeOnly"), report.decode_only},
    };
    if (report.document_sha256)
        map.insert(QStringLiteral("documentSha256"), qstring_from_utf8(*report.document_sha256));
    if (report.document_bytes)
        map.insert(QStringLiteral("documentBytes"),
                   QVariant::fromValue(static_cast<qulonglong>(*report.document_bytes)));
    if (report.camera_make)
        map.insert(QStringLiteral("cameraMake"), qstring_from_utf8(*report.camera_make));
    if (report.camera_model)
        map.insert(QStringLiteral("cameraModel"), qstring_from_utf8(*report.camera_model));
    if (report.iso)
        map.insert(QStringLiteral("iso"), static_cast<int>(*report.iso));
    if (report.illuminant)
        map.insert(QStringLiteral("illuminant"), qstring_from_utf8(*report.illuminant));
    return map;
}

} // namespace

QVariantMap StudioPresenter::iqQualityPolicy() const
{
    return QVariantMap{
        {QStringLiteral("schema"),
         qstring_from_utf8(std::string(kIqFixtureSupportContractVersion))},
        {QStringLiteral("corpusSchema"),
         qstring_from_utf8(std::string(kIqEvaluationCorpusContractVersion))},
        {QStringLiteral("maturity"), QStringLiteral("C2")},
        {QStringLiteral("supportClaimStatus"),
         qstring_from_utf8(std::string(kIqSupportClaimFixtureEvidenceReady))},
        {QStringLiteral("cameraProductSupportClaimed"), false},
        {QStringLiteral("learnedDenoiseAdmitted"), false},
        {QStringLiteral("cpuGoldAligned"), true},
        {QStringLiteral("decodeOnly"), false},
        {QStringLiteral("residualC3"), QStringLiteral("licensed_real_corpus_and_human_review")},
        {QStringLiteral("corpusEnv"), qstring_from_utf8(std::string(kIqCorpusEnvVar))},
        {QStringLiteral("failClosedWithoutCorpus"), true},
    };
}

QVariantMap StudioPresenter::evaluateIqQuality(const QString &corpusRoot,
                                               const double strength) const
{
    std::optional<std::string> root;
    if (!corpusRoot.isEmpty())
        root = utf8_from_qstring(corpusRoot);
    auto report = evaluate_iq_fixture_support(std::move(root), strength, CancellationToken{});
    if (!report)
    {
        auto map = task_error_map(report.error());
        map.insert(QStringLiteral("policy"), iqQualityPolicy());
        return map;
    }
    return QVariantMap{
        {QStringLiteral("ok"), true},
        {QStringLiteral("schema"), qstring_from_utf8(report.value().schema)},
        {QStringLiteral("maturity"), qstring_from_utf8(report.value().maturity)},
        {QStringLiteral("supportClaimStatus"),
         qstring_from_utf8(report.value().support_claim_status)},
        {QStringLiteral("cameraProductSupportClaimed"),
         report.value().camera_product_support_claimed},
        {QStringLiteral("learnedDenoiseAdmitted"), report.value().learned_denoise_admitted},
        {QStringLiteral("cpuGoldAligned"), report.value().cpu_gold_aligned},
        {QStringLiteral("decodeOnly"), report.value().decode_only},
        {QStringLiteral("residualC3"), qstring_from_utf8(report.value().residual_c3)},
        {QStringLiteral("corpusId"), qstring_from_utf8(report.value().corpus_id)},
        {QStringLiteral("corpusLicense"), qstring_from_utf8(report.value().corpus_license)},
        {QStringLiteral("denoise"), denoise_map(report.value().denoise)},
        {QStringLiteral("cameraProfile"), camera_map(report.value().camera_profile)},
        {QStringLiteral("policy"), iqQualityPolicy()},
    };
}

} // namespace ravo
