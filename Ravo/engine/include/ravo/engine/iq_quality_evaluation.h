#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0152: IQ-01 evaluation corpus + CPU denoise / camera-profile probes.
inline constexpr std::string_view kIqEvaluationCorpusContractVersion =
    "ravo.iq.evaluation-corpus/v1";
inline constexpr std::int64_t kIqEvaluationCorpusSchemaVersion = 1;

inline constexpr std::string_view kIqDenoiseEvaluationContractVersion =
    "ravo.iq.denoise-evaluation/v1";
inline constexpr std::int64_t kIqDenoiseEvaluationSchemaVersion = 1;

inline constexpr std::string_view kIqCameraProfileProbeContractVersion =
    "ravo.iq.camera-profile-probe/v1";
inline constexpr std::int64_t kIqCameraProfileProbeSchemaVersion = 1;

inline constexpr std::string_view kIqFixtureSupportContractVersion = "ravo.iq.fixture-support/v1";
inline constexpr std::int64_t kIqFixtureSupportSchemaVersion = 1;

inline constexpr std::string_view kIqCorpusEnvVar = "RAVO_IQ_CORPUS_ROOT";

// Photographer-facing support-claim status for fixture evaluation (not product
// camera certification). Real licensed corpus + human review remain C3.
inline constexpr std::string_view kIqSupportClaimFixtureEvidenceReady = "fixture_evidence_ready";
inline constexpr std::string_view kIqSupportClaimUnavailable = "unavailable";

struct IqEvaluationCorpusCase
{
    std::string case_id;
    std::string kind;
    std::optional<std::string> relative_path;
    bool synthetic = false;
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
    std::optional<std::uint32_t> iso;
    std::optional<std::string> illuminant;
    std::optional<std::string> notes;
};

struct IqEvaluationCorpus
{
    std::string schema{std::string(kIqEvaluationCorpusContractVersion)};
    std::int64_t schema_version = kIqEvaluationCorpusSchemaVersion;
    std::string corpus_id;
    std::string license;
    std::string notice_path;
    std::string root_path;
    std::vector<IqEvaluationCorpusCase> cases;
};

struct IqDenoiseEvaluationReport
{
    std::string schema{std::string(kIqDenoiseEvaluationContractVersion)};
    std::int64_t schema_version = kIqDenoiseEvaluationSchemaVersion;
    std::string corpus_id;
    std::string case_id;
    std::string operation_id{"ravo.detail.denoiseprofile"};
    std::string backend{"cpu"};
    std::string support_claim_status{std::string(kIqSupportClaimFixtureEvidenceReady)};
    bool cpu_gold_aligned = true;
    bool learned_denoise_admitted = false;
    double strength = 0.0;
    double mean_abs_delta = 0.0;
    double max_abs_delta = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool finite = true;
    bool decode_only = false;
};

struct IqCameraProfileProbeReport
{
    std::string schema{std::string(kIqCameraProfileProbeContractVersion)};
    std::int64_t schema_version = kIqCameraProfileProbeSchemaVersion;
    std::string corpus_id;
    std::string case_id;
    std::string probe{"camera_noise_calibration_document"};
    std::string support_claim_status{std::string(kIqSupportClaimFixtureEvidenceReady)};
    bool document_present = false;
    std::optional<std::string> document_sha256;
    std::optional<std::uint64_t> document_bytes;
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
    std::optional<std::uint32_t> iso;
    std::optional<std::string> illuminant;
    bool colour_accuracy_closed = false;
    bool decode_only = false;
};

struct IqFixtureSupportReport
{
    std::string schema{std::string(kIqFixtureSupportContractVersion)};
    std::int64_t schema_version = kIqFixtureSupportSchemaVersion;
    std::string maturity{"C2"};
    std::string support_claim_status{std::string(kIqSupportClaimFixtureEvidenceReady)};
    bool camera_product_support_claimed = false;
    bool learned_denoise_admitted = false;
    bool cpu_gold_aligned = true;
    bool decode_only = false;
    std::string residual_c3{"licensed_real_corpus_and_human_review"};
    std::string corpus_id;
    std::string corpus_license;
    IqDenoiseEvaluationReport denoise;
    IqCameraProfileProbeReport camera_profile;
};

[[nodiscard]] Result<IqEvaluationCorpus>
resolve_iq_evaluation_corpus(std::optional<std::string> corpus_root = std::nullopt);

[[nodiscard]] Result<IqDenoiseEvaluationReport>
evaluate_denoise_cpu_reference(const IqEvaluationCorpus &corpus, double strength = 0.35,
                               const CancellationToken &cancellation = {});

[[nodiscard]] Result<IqCameraProfileProbeReport>
probe_camera_profile_quality(const IqEvaluationCorpus &corpus,
                             const CancellationToken &cancellation = {});

// Resolve corpus (fail-closed), run CPU denoise + camera-profile probes, and
// emit the photographer-facing fixture support-claim bundle (ADR-0152 C2).
[[nodiscard]] Result<IqFixtureSupportReport>
evaluate_iq_fixture_support(std::optional<std::string> corpus_root = std::nullopt,
                            double strength = 0.35, const CancellationToken &cancellation = {});

} // namespace ravo
