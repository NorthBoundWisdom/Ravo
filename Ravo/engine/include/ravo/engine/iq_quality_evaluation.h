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

// ADR-0152: IQ-01 evaluation corpus + CPU denoise probe (first Ready).
inline constexpr std::string_view kIqEvaluationCorpusContractVersion =
    "ravo.iq.evaluation-corpus/v1";
inline constexpr std::int64_t kIqEvaluationCorpusSchemaVersion = 1;

inline constexpr std::string_view kIqDenoiseEvaluationContractVersion =
    "ravo.iq.denoise-evaluation/v1";
inline constexpr std::int64_t kIqDenoiseEvaluationSchemaVersion = 1;

inline constexpr std::string_view kIqCameraProfileProbeContractVersion =
    "ravo.iq.camera-profile-probe/v1";
inline constexpr std::int64_t kIqCameraProfileProbeSchemaVersion = 1;

inline constexpr std::string_view kIqCorpusEnvVar = "RAVO_IQ_CORPUS_ROOT";

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
    double strength = 0.0;
    double mean_abs_delta = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool finite = true;
};

struct IqCameraProfileProbeReport
{
    std::string schema{std::string(kIqCameraProfileProbeContractVersion)};
    std::int64_t schema_version = kIqCameraProfileProbeSchemaVersion;
    std::string corpus_id;
    std::string case_id;
    std::string probe{"camera_noise_calibration_document"};
    bool document_present = false;
    std::optional<std::string> document_sha256;
};

[[nodiscard]] Result<IqEvaluationCorpus>
resolve_iq_evaluation_corpus(std::optional<std::string> corpus_root = std::nullopt);

[[nodiscard]] Result<IqDenoiseEvaluationReport>
evaluate_denoise_cpu_reference(const IqEvaluationCorpus &corpus, double strength = 0.35,
                               const CancellationToken &cancellation = {});

[[nodiscard]] Result<IqCameraProfileProbeReport>
probe_camera_profile_quality(const IqEvaluationCorpus &corpus,
                             const CancellationToken &cancellation = {});

} // namespace ravo
