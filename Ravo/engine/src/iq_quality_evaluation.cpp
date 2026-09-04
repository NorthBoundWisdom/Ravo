#include "ravo/engine/iq_quality_evaluation.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

namespace fs = std::filesystem;

[[nodiscard]] Result<std::string> read_text_file(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return make_error(ErrorCode::kNotFound, "Evaluation corpus file is missing",
                          {{"path", path.string()}, {"reason", "iq_corpus_file_missing"}});
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object,
                                                 const std::string_view key)
{
    const auto *value = [&]() -> const JsonValue *
    {
        const auto found = object.find(std::string(key));
        return found == object.end() ? nullptr : &found->second;
    }();
    if (value == nullptr || value->string_if() == nullptr || value->string_if()->empty())
    {
        return make_error(ErrorCode::kValidation, "Evaluation corpus manifest field is missing",
                          {{"field", std::string(key)}, {"reason", "iq_corpus_invalid"}});
    }
    return *value->string_if();
}

[[nodiscard]] LinearWorkingBuffer make_synthetic_working(const std::uint32_t width,
                                                         const std::uint32_t height)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(kInputProfileLinearRec709);
    profile.has_matrix = true;
    LinearWorkingBuffer input;
    input.width = width;
    input.height = height;
    input.color_profile = profile;
    input.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    input.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < input.rgb.size(); ++index)
    {
        const auto channel = index % 3U;
        const auto pixel = index / 3U;
        const float noise = static_cast<float>((pixel * 17U + channel * 13U) % 64U) / 255.0F;
        input.rgb[index] = 0.25F + noise;
    }
    return input;
}

[[nodiscard]] Recipe make_denoise_recipe(const double strength)
{
    Recipe recipe;
    recipe.asset = {"iq-denoise-eval", "memory:iq-denoise-eval", std::nullopt};
    recipe.operations.push_back({"ravo.detail.denoiseprofile",
                                 1,
                                 "denoiseprofile-eval",
                                 true,
                                 {{"strength", ParameterValue{strength}},
                                  {"chroma", ParameterValue{1.0}},
                                  {"radius", ParameterValue{1.0}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    return recipe;
}

[[nodiscard]] Recipe make_identity_recipe()
{
    Recipe recipe;
    recipe.asset = {"iq-denoise-eval", "memory:iq-denoise-eval", std::nullopt};
    recipe.operations.push_back({"ravo.color.output", 1, "output", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    return recipe;
}

[[nodiscard]] Result<IqEvaluationCorpusCase> parse_case(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Evaluation corpus case must be an object",
                          {{"reason", "iq_corpus_invalid"}});
    }
    IqEvaluationCorpusCase parsed;
    auto case_id = require_string(*object, "case_id");
    if (!case_id)
        return case_id.error();
    parsed.case_id = std::move(case_id).value();
    auto kind = require_string(*object, "kind");
    if (!kind)
        return kind.error();
    parsed.kind = std::move(kind).value();
    if (const auto *path = value.find("relative_path"); path != nullptr && path->string_if())
        parsed.relative_path = *path->string_if();
    if (const auto *synthetic = value.find("synthetic");
        synthetic != nullptr && synthetic->boolean_if())
        parsed.synthetic = *synthetic->boolean_if();
    if (const auto *make = value.find("camera_make"); make != nullptr && make->string_if())
        parsed.camera_make = *make->string_if();
    if (const auto *model = value.find("camera_model"); model != nullptr && model->string_if())
        parsed.camera_model = *model->string_if();
    if (const auto *iso = value.find("iso"); iso != nullptr && iso->number_if())
        parsed.iso = static_cast<std::uint32_t>(std::stoul(iso->number_if()->text));
    if (const auto *illuminant = value.find("illuminant");
        illuminant != nullptr && illuminant->string_if())
        parsed.illuminant = *illuminant->string_if();
    if (const auto *notes = value.find("notes"); notes != nullptr && notes->string_if())
        parsed.notes = *notes->string_if();
    return parsed;
}

} // namespace

Result<IqEvaluationCorpus> resolve_iq_evaluation_corpus(std::optional<std::string> corpus_root)
{
    std::string root;
    if (corpus_root && !corpus_root->empty())
        root = *corpus_root;
    else if (const char *env = std::getenv(std::string(kIqCorpusEnvVar).c_str());
             env != nullptr && env[0] != '\0')
        root = env;

    if (root.empty())
    {
        return make_error(ErrorCode::kNotFound, "IQ evaluation corpus is unavailable",
                          {{"reason", "iq_corpus_unavailable"},
                           {"hint", "pass corpus root or set RAVO_IQ_CORPUS_ROOT"}});
    }

    const fs::path root_path = fs::path(root);
    const fs::path manifest_path = root_path / "manifest.json";
    auto text = read_text_file(manifest_path);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
    {
        return make_error(ErrorCode::kValidation, "IQ evaluation corpus manifest is invalid JSON",
                          {{"reason", "iq_corpus_invalid"}, {"path", manifest_path.string()}});
    }
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "IQ evaluation corpus manifest must be an object",
                          {{"reason", "iq_corpus_invalid"}});
    }

    auto schema = require_string(*object, "schema");
    if (!schema)
        return schema.error();
    if (schema.value() != kIqEvaluationCorpusContractVersion)
    {
        return make_error(ErrorCode::kValidation, "IQ evaluation corpus schema mismatch",
                          {{"reason", "iq_corpus_invalid"},
                           {"schema", schema.value()},
                           {"expected", std::string(kIqEvaluationCorpusContractVersion)}});
    }

    IqEvaluationCorpus corpus;
    auto corpus_id = require_string(*object, "corpus_id");
    if (!corpus_id)
        return corpus_id.error();
    corpus.corpus_id = std::move(corpus_id).value();
    auto license = require_string(*object, "license");
    if (!license)
        return license.error();
    corpus.license = std::move(license).value();
    auto notice = require_string(*object, "notice_path");
    if (!notice)
        return notice.error();
    corpus.notice_path = std::move(notice).value();
    corpus.root_path = root_path.string();

    const auto *cases = parsed.value().find("cases");
    if (cases == nullptr || cases->array_if() == nullptr || cases->array_if()->empty())
    {
        return make_error(ErrorCode::kValidation, "IQ evaluation corpus has no cases",
                          {{"reason", "iq_corpus_invalid"}, {"corpus_id", corpus.corpus_id}});
    }
    for (const auto &entry : *cases->array_if())
    {
        auto parsed_case = parse_case(entry);
        if (!parsed_case)
            return parsed_case.error();
        corpus.cases.push_back(std::move(parsed_case).value());
    }
    return corpus;
}

Result<IqDenoiseEvaluationReport>
evaluate_denoise_cpu_reference(const IqEvaluationCorpus &corpus, const double strength,
                               const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();

    const IqEvaluationCorpusCase *selected = nullptr;
    for (const auto &entry : corpus.cases)
    {
        if (entry.kind == "denoise_fixture")
        {
            selected = &entry;
            break;
        }
    }
    if (selected == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "IQ denoise evaluation case is missing",
                          {{"reason", "iq_corpus_unavailable"},
                           {"corpus_id", corpus.corpus_id},
                           {"kind", "denoise_fixture"}});
    }
    if (!selected->synthetic && !selected->relative_path)
    {
        return make_error(ErrorCode::kValidation,
                          "Denoise fixture must be synthetic or provide relative_path",
                          {{"reason", "iq_corpus_invalid"}, {"case_id", selected->case_id}});
    }

    const auto engine = EngineFacade::create_phase1();
    if (!engine)
        return engine.error();
    const auto working = make_synthetic_working(16, 12);
    const auto baseline_recipe = make_identity_recipe();
    const auto denoise_recipe = make_denoise_recipe(strength);

    const auto baseline =
        engine.value().render_linear_working(working, baseline_recipe, cancellation);
    if (!baseline)
        return baseline.error();
    if (!baseline.value().gpu_backend.empty())
    {
        return make_error(ErrorCode::kValidation, "Denoise evaluation left the CPU gold backend",
                          {{"reason", "iq_cpu_gold_backend_required"},
                           {"gpu_backend", baseline.value().gpu_backend}});
    }

    const auto denoised =
        engine.value().render_linear_working(working, denoise_recipe, cancellation);
    if (!denoised)
        return denoised.error();
    if (!denoised.value().gpu_backend.empty())
    {
        return make_error(ErrorCode::kValidation, "Denoise evaluation left the CPU gold backend",
                          {{"reason", "iq_cpu_gold_backend_required"},
                           {"gpu_backend", denoised.value().gpu_backend}});
    }
    if (baseline.value().rgb.size() != denoised.value().rgb.size() || baseline.value().rgb.empty())
    {
        return make_error(ErrorCode::kInternal, "Denoise evaluation buffer size mismatch",
                          {{"reason", "iq_denoise_buffer_mismatch"}});
    }

    double abs_sum = 0.0;
    bool finite = true;
    for (std::size_t index = 0; index < baseline.value().rgb.size(); ++index)
    {
        const float left = static_cast<float>(baseline.value().rgb[index]) / 255.0F;
        const float right = static_cast<float>(denoised.value().rgb[index]) / 255.0F;
        if (!std::isfinite(left) || !std::isfinite(right))
            finite = false;
        abs_sum += std::fabs(static_cast<double>(left - right));
    }

    IqDenoiseEvaluationReport report;
    report.corpus_id = corpus.corpus_id;
    report.case_id = selected->case_id;
    report.strength = strength;
    report.mean_abs_delta = abs_sum / static_cast<double>(baseline.value().rgb.size());
    report.width = baseline.value().width;
    report.height = baseline.value().height;
    report.finite = finite;
    if (!finite)
    {
        return make_error(ErrorCode::kValidation, "Denoise evaluation produced non-finite pixels",
                          {{"reason", "iq_denoise_non_finite"}, {"case_id", selected->case_id}});
    }
    return report;
}

Result<IqCameraProfileProbeReport>
probe_camera_profile_quality(const IqEvaluationCorpus &corpus,
                             const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();

    const IqEvaluationCorpusCase *selected = nullptr;
    for (const auto &entry : corpus.cases)
    {
        if (entry.kind == "camera_profile_fixture")
        {
            selected = &entry;
            break;
        }
    }
    if (selected == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "IQ camera profile probe case is missing",
                          {{"reason", "iq_corpus_unavailable"},
                           {"corpus_id", corpus.corpus_id},
                           {"kind", "camera_profile_fixture"}});
    }

    IqCameraProfileProbeReport report;
    report.corpus_id = corpus.corpus_id;
    report.case_id = selected->case_id;
    if (selected->synthetic && !selected->relative_path)
    {
        report.document_present = false;
        return report;
    }
    if (!selected->relative_path)
    {
        return make_error(ErrorCode::kValidation,
                          "Camera profile fixture requires relative_path when not synthetic",
                          {{"reason", "iq_corpus_invalid"}, {"case_id", selected->case_id}});
    }
    const fs::path document_path = fs::path(corpus.root_path) / *selected->relative_path;
    std::error_code error;
    const bool exists = fs::is_regular_file(document_path, error) && !error;
    if (!exists)
    {
        return make_error(ErrorCode::kNotFound, "Camera profile fixture document is missing",
                          {{"reason", "iq_corpus_unavailable"},
                           {"path", document_path.string()},
                           {"case_id", selected->case_id}});
    }
    report.document_present = true;
    return report;
}

} // namespace ravo
