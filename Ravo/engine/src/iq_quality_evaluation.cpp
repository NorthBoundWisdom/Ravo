#include "ravo/engine/iq_quality_evaluation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/engine/iq_consistency.h"
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

[[nodiscard]] Result<std::vector<std::uint8_t>> read_binary_file(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return make_error(ErrorCode::kNotFound, "Evaluation corpus file is missing",
                          {{"path", path.string()}, {"reason", "iq_corpus_file_missing"}});
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0)
    {
        return make_error(ErrorCode::kIo, "Unable to size evaluation corpus file",
                          {{"path", path.string()}, {"reason", "iq_corpus_file_missing"}});
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char *>(bytes.data()), size))
    {
        return make_error(ErrorCode::kIo, "Unable to read evaluation corpus file",
                          {{"path", path.string()}, {"reason", "iq_corpus_file_missing"}});
    }
    return bytes;
}

// Compact SHA-256 (public-domain style) for fixture document digests; Engine must
// not pull Qt adapter hashing.
class Sha256
{
public:
    Sha256()
    {
        reset();
    }

    void update(const std::uint8_t *data, std::size_t length)
    {
        for (std::size_t index = 0; index < length; ++index)
        {
            data_[datalen_] = data[index];
            ++datalen_;
            if (datalen_ == 64U)
            {
                transform();
                bitlen_ += 512ULL;
                datalen_ = 0U;
            }
        }
    }

    [[nodiscard]] std::string hex_digest()
    {
        std::array<std::uint8_t, 32> hash{};
        finalize(hash.data());
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (std::size_t index = 0; index < hash.size(); ++index)
        {
            out[index * 2U] = kHex[(hash[index] >> 4U) & 0x0FU];
            out[index * 2U + 1U] = kHex[hash[index] & 0x0FU];
        }
        return out;
    }

private:
    void reset()
    {
        datalen_ = 0U;
        bitlen_ = 0ULL;
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    }

    void transform()
    {
        static constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::array<std::uint32_t, 64> m{};
        for (std::uint32_t index = 0, j = 0; index < 16U; ++index, j += 4U)
        {
            m[index] = (static_cast<std::uint32_t>(data_[j]) << 24U) |
                       (static_cast<std::uint32_t>(data_[j + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(data_[j + 2U]) << 8U) |
                       (static_cast<std::uint32_t>(data_[j + 3U]));
        }
        for (std::uint32_t index = 16U; index < 64U; ++index)
        {
            const std::uint32_t sig0 =
                rotr(m[index - 15U], 7U) ^ rotr(m[index - 15U], 18U) ^ (m[index - 15U] >> 3U);
            const std::uint32_t sig1 =
                rotr(m[index - 2U], 17U) ^ rotr(m[index - 2U], 19U) ^ (m[index - 2U] >> 10U);
            m[index] = m[index - 16U] + sig0 + m[index - 7U] + sig1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::uint32_t index = 0; index < 64U; ++index)
        {
            const std::uint32_t sum1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = h + sum1 + ch + k[index] + m[index];
            const std::uint32_t sum0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = sum0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    void finalize(std::uint8_t *hash)
    {
        std::uint32_t index = datalen_;
        if (datalen_ < 56U)
        {
            data_[index++] = 0x80U;
            while (index < 56U)
                data_[index++] = 0x00U;
        }
        else
        {
            data_[index++] = 0x80U;
            while (index < 64U)
                data_[index++] = 0x00U;
            transform();
            std::fill(data_.begin(), data_.begin() + 56, 0U);
        }
        bitlen_ += static_cast<std::uint64_t>(datalen_) * 8ULL;
        data_[63] = static_cast<std::uint8_t>(bitlen_);
        data_[62] = static_cast<std::uint8_t>(bitlen_ >> 8U);
        data_[61] = static_cast<std::uint8_t>(bitlen_ >> 16U);
        data_[60] = static_cast<std::uint8_t>(bitlen_ >> 24U);
        data_[59] = static_cast<std::uint8_t>(bitlen_ >> 32U);
        data_[58] = static_cast<std::uint8_t>(bitlen_ >> 40U);
        data_[57] = static_cast<std::uint8_t>(bitlen_ >> 48U);
        data_[56] = static_cast<std::uint8_t>(bitlen_ >> 56U);
        transform();
        for (std::uint32_t i = 0; i < 4U; ++i)
        {
            hash[i] = (state_[0] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 4U] = (state_[1] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 8U] = (state_[2] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 12U] = (state_[3] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 16U] = (state_[4] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 20U] = (state_[5] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 24U] = (state_[6] >> (24U - i * 8U)) & 0x000000ffU;
            hash[i + 28U] = (state_[7] >> (24U - i * 8U)) & 0x000000ffU;
        }
    }

    [[nodiscard]] static std::uint32_t rotr(const std::uint32_t value, const std::uint32_t bits)
    {
        return (value >> bits) | (value << (32U - bits));
    }

    std::array<std::uint8_t, 64> data_{};
    std::uint32_t datalen_ = 0U;
    std::uint64_t bitlen_ = 0ULL;
    std::array<std::uint32_t, 8> state_{};
};

[[nodiscard]] std::string sha256_hex(const std::vector<std::uint8_t> &bytes)
{
    Sha256 hash;
    if (!bytes.empty())
        hash.update(bytes.data(), bytes.size());
    return hash.hex_digest();
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
    if (auto gold = require_cpu_gold_backend(baseline.value().gpu_backend, "iq_denoise_baseline");
        !gold)
        return gold.error();

    const auto denoised =
        engine.value().render_linear_working(working, denoise_recipe, cancellation);
    if (!denoised)
        return denoised.error();
    if (auto gold = require_cpu_gold_backend(denoised.value().gpu_backend, "iq_denoise_evaluated");
        !gold)
        return gold.error();
    if (baseline.value().rgb.size() != denoised.value().rgb.size() || baseline.value().rgb.empty())
    {
        return make_error(ErrorCode::kInternal, "Denoise evaluation buffer size mismatch",
                          {{"reason", "iq_denoise_buffer_mismatch"}});
    }

    double abs_sum = 0.0;
    double abs_max = 0.0;
    bool finite = true;
    for (std::size_t index = 0; index < baseline.value().rgb.size(); ++index)
    {
        const float left = static_cast<float>(baseline.value().rgb[index]) / 255.0F;
        const float right = static_cast<float>(denoised.value().rgb[index]) / 255.0F;
        if (!std::isfinite(left) || !std::isfinite(right))
            finite = false;
        const double delta = std::fabs(static_cast<double>(left - right));
        abs_sum += delta;
        abs_max = std::max(abs_max, delta);
    }

    IqDenoiseEvaluationReport report;
    report.corpus_id = corpus.corpus_id;
    report.case_id = selected->case_id;
    report.strength = strength;
    report.mean_abs_delta = abs_sum / static_cast<double>(baseline.value().rgb.size());
    report.max_abs_delta = abs_max;
    report.width = baseline.value().width;
    report.height = baseline.value().height;
    report.finite = finite;
    report.cpu_gold_aligned = true;
    report.learned_denoise_admitted = false;
    report.decode_only = false;
    report.support_claim_status = std::string(kIqSupportClaimFixtureEvidenceReady);
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
    report.camera_make = selected->camera_make;
    report.camera_model = selected->camera_model;
    report.iso = selected->iso;
    report.illuminant = selected->illuminant;
    report.colour_accuracy_closed = false;
    report.decode_only = false;
    report.support_claim_status = std::string(kIqSupportClaimFixtureEvidenceReady);
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
    auto bytes = read_binary_file(document_path);
    if (!bytes)
        return bytes.error();
    report.document_present = true;
    report.document_bytes = bytes.value().size();
    report.document_sha256 = sha256_hex(bytes.value());
    return report;
}

Result<IqFixtureSupportReport> evaluate_iq_fixture_support(std::optional<std::string> corpus_root,
                                                           const double strength,
                                                           const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto corpus = resolve_iq_evaluation_corpus(std::move(corpus_root));
    if (!corpus)
        return corpus.error();

    auto denoise = evaluate_denoise_cpu_reference(corpus.value(), strength, cancellation);
    if (!denoise)
        return denoise.error();
    auto camera = probe_camera_profile_quality(corpus.value(), cancellation);
    if (!camera)
        return camera.error();

    IqFixtureSupportReport report;
    report.maturity = "C2";
    report.support_claim_status = std::string(kIqSupportClaimFixtureEvidenceReady);
    report.camera_product_support_claimed = false;
    report.learned_denoise_admitted = false;
    report.cpu_gold_aligned = denoise.value().cpu_gold_aligned;
    report.decode_only = false;
    report.residual_c3 = "licensed_real_corpus_and_human_review";
    report.corpus_id = corpus.value().corpus_id;
    report.corpus_license = corpus.value().license;
    report.denoise = std::move(denoise).value();
    report.camera_profile = std::move(camera).value();
    return report;
}

} // namespace ravo
