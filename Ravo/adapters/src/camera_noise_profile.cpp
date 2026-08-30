#include "ravo/adapters/camera_noise_profile.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <QByteArrayView>
#include <QCryptographicHash>

#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{

constexpr std::size_t kIdentityTextMaximumBytes = 128U;
constexpr double kMaximumSignal = 65535.0;
constexpr double kMaximumVariance = kMaximumSignal * kMaximumSignal;

[[nodiscard]] TaskError document_error(std::string message, std::string reason,
                                       std::string field = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!field.empty())
        context.emplace("field", std::move(field));
    return make_error(ErrorCode::kValidation, std::move(message), std::move(context));
}

[[nodiscard]] Result<void> expect_keys(const JsonValue::Object &object,
                                       const std::initializer_list<std::string_view> keys,
                                       const std::string_view owner)
{
    std::set<std::string, std::less<>> expected;
    for (const auto key : keys)
        expected.emplace(key);
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (!expected.contains(key))
            return document_error("Camera noise document has an unknown field", "unknown_field",
                                  std::string(owner) + "." + key);
    }
    for (const auto &key : expected)
        if (!object.contains(key))
            return document_error("Camera noise document is missing a required field",
                                  "missing_field", std::string(owner) + "." + key);
    return {};
}

[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue *value,
                                                               const std::string_view field)
{
    if (value == nullptr || value->object_if() == nullptr)
        return document_error("Camera noise field must be an object", "type_mismatch",
                              std::string(field));
    return value->object_if();
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object,
                                                 const std::string_view key,
                                                 const std::size_t maximum_bytes,
                                                 const bool allow_empty = false)
{
    const auto iterator = object.find(key);
    const auto *value = iterator == object.end() ? nullptr : iterator->second.string_if();
    if (value == nullptr)
        return document_error("Camera noise field must be a string", "type_mismatch",
                              std::string(key));
    if ((!allow_empty && value->empty()) || value->size() > maximum_bytes)
        return document_error("Camera noise string field is outside its supported bounds",
                              "string_out_of_range", std::string(key));
    return *value;
}

template <typename T>
[[nodiscard]] Result<T> require_integer(const JsonValue::Object &object, const std::string_view key,
                                        const T minimum, const T maximum)
{
    const auto iterator = object.find(key);
    const auto *number = iterator == object.end() ? nullptr : iterator->second.number_if();
    if (number == nullptr)
        return document_error("Camera noise field must be an integer", "type_mismatch",
                              std::string(key));
    T value{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        value < minimum || value > maximum)
        return document_error("Camera noise integer field is outside its supported bounds",
                              "integer_out_of_range", std::string(key));
    return value;
}

[[nodiscard]] Result<double> require_double(const JsonValue::Object &object,
                                            const std::string_view key, const double minimum,
                                            const double maximum, const bool minimum_inclusive)
{
    const auto iterator = object.find(key);
    const auto *number = iterator == object.end() ? nullptr : iterator->second.number_if();
    if (number == nullptr)
        return document_error("Camera noise field must be numeric", "type_mismatch",
                              std::string(key));
    double value = 0.0;
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        !std::isfinite(value) || (minimum_inclusive ? value < minimum : value <= minimum) ||
        value > maximum)
        return document_error("Camera noise numeric field is outside its supported bounds",
                              "number_out_of_range", std::string(key));
    return value;
}

[[nodiscard]] Result<CameraNoiseIdentity> parse_identity(const JsonValue *value)
{
    auto object = require_object(value, "identity");
    if (!object)
        return object.error();
    auto exact = expect_keys(*object.value(), {"iso", "make", "model"}, "identity");
    if (!exact)
        return exact.error();
    auto make = require_string(*object.value(), "make", kIdentityTextMaximumBytes);
    auto model = require_string(*object.value(), "model", kIdentityTextMaximumBytes);
    auto iso = require_integer<std::uint32_t>(*object.value(), "iso", 1U, 1'000'000U);
    if (!make)
        return make.error();
    if (!model)
        return model.error();
    if (!iso)
        return iso.error();
    return CameraNoiseIdentity{std::move(make).value(), std::move(model).value(), iso.value()};
}

[[nodiscard]] JsonValue identity_json(const CameraNoiseIdentity &identity)
{
    return JsonValue::Object{{"iso", JsonValue::number(std::to_string(identity.iso))},
                             {"make", identity.make},
                             {"model", identity.model}};
}

[[nodiscard]] Result<std::string> number_text(const double value)
{
    if (!std::isfinite(value))
        return document_error("Camera noise output contains a non-finite number",
                              "nonfinite_output");
    std::array<char, 64U> buffer{};
    const auto written =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (written.ec != std::errc{})
        return make_error(ErrorCode::kInternal, "Unable to format camera noise number",
                          {{"reason", "number_format_failed"}});
    return std::string(buffer.data(), written.ptr);
}

[[nodiscard]] std::string sha256(const std::string_view text)
{
    const auto bytes = QByteArrayView(text.data(), static_cast<qsizetype>(text.size()));
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

[[nodiscard]] bool is_sha256(const std::string_view text)
{
    if (text.size() != 64U)
        return false;
    for (const char value : text)
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    return true;
}

[[nodiscard]] Result<JsonValue> profile_payload(const CameraNoiseIdentity &identity,
                                                const CameraNoiseFit &fit,
                                                const std::string_view source_sha256)
{
    if (identity.make.empty() || identity.model.empty() || identity.iso == 0U ||
        !is_sha256(source_sha256) || fit.input_sample_count == 0U ||
        fit.retained_sample_count == 0U || fit.retained_sample_count > fit.input_sample_count ||
        !std::isfinite(fit.gaussian_variance) || fit.gaussian_variance < 0.0 ||
        fit.gaussian_variance > kMaximumVariance || !std::isfinite(fit.poisson_slope) ||
        fit.poisson_slope < 0.0 || fit.poisson_slope > kMaximumSignal ||
        !std::isfinite(fit.weighted_rmse) || fit.weighted_rmse < 0.0 ||
        !std::isfinite(fit.weighted_r_squared))
        return document_error("Camera noise profile state is invalid", "invalid_profile_state");
    auto gaussian = number_text(fit.gaussian_variance);
    auto poisson = number_text(fit.poisson_slope);
    auto rmse = number_text(fit.weighted_rmse);
    auto r_squared = number_text(fit.weighted_r_squared);
    if (!gaussian)
        return gaussian.error();
    if (!poisson)
        return poisson.error();
    if (!rmse)
        return rmse.error();
    if (!r_squared)
        return r_squared.error();
    JsonValue::Object fit_object{
        {"gaussian_variance", JsonValue::number(std::move(gaussian).value())},
        {"input_sample_count", JsonValue::number(std::to_string(fit.input_sample_count))},
        {"poisson_slope", JsonValue::number(std::move(poisson).value())},
        {"retained_sample_count", JsonValue::number(std::to_string(fit.retained_sample_count))},
        {"weighted_r_squared", JsonValue::number(std::move(r_squared).value())},
        {"weighted_rmse", JsonValue::number(std::move(rmse).value())},
    };
    return JsonValue{JsonValue::Object{
        {"fit", std::move(fit_object)},
        {"fit_policy", std::string(kCameraNoiseFitPolicy)},
        {"identity", identity_json(identity)},
        {"model", std::string(kCameraNoiseModel)},
        {"schema", std::string(kCameraNoiseProfileSchema)},
        {"source_samples_sha256", std::string(source_sha256)},
        {"units", std::string(kCameraNoiseSignalUnits)},
        {"version", JsonValue::number(std::to_string(kCameraNoiseProfileSchemaVersion))},
    }};
}

} // namespace

Result<CameraNoiseCalibrationDocument>
parse_camera_noise_calibration_json(const std::string_view text)
{
    auto root = parse_json(text);
    if (!root)
        return root.error();
    const auto *object = root.value().object_if();
    if (object == nullptr)
        return document_error("Camera noise calibration input must be an object", "type_mismatch");
    auto exact =
        expect_keys(*object, {"identity", "samples", "schema", "units", "version"}, "document");
    if (!exact)
        return exact.error();
    auto schema = require_string(*object, "schema", 64U);
    auto units = require_string(*object, "units", 64U);
    auto version = require_integer<std::uint32_t>(*object, "version", 1U, 1U);
    if (!schema)
        return schema.error();
    if (schema.value() != kCameraNoiseSampleSchema)
        return document_error("Unsupported camera noise calibration schema", "unsupported_schema",
                              "schema");
    if (!units)
        return units.error();
    if (units.value() != kCameraNoiseSignalUnits)
        return document_error("Unsupported camera noise signal units", "unsupported_units",
                              "units");
    if (!version)
        return version.error();
    auto identity = parse_identity(root.value().find("identity"));
    if (!identity)
        return identity.error();
    const auto *samples_value = root.value().find("samples");
    const auto *samples_array = samples_value == nullptr ? nullptr : samples_value->array_if();
    if (samples_array == nullptr)
        return document_error("Camera noise samples must be an array", "type_mismatch", "samples");
    if (samples_array->size() < kCameraNoiseMinimumSamples ||
        samples_array->size() > kCameraNoiseMaximumSamples)
        return document_error("Camera noise sample count is outside the supported range",
                              "sample_count_out_of_range", "samples");

    CameraNoiseCalibrationDocument result;
    result.identity = std::move(identity).value();
    result.samples.reserve(samples_array->size());
    for (std::size_t index = 0U; index < samples_array->size(); ++index)
    {
        const auto *sample = (*samples_array)[index].object_if();
        if (sample == nullptr)
            return document_error("Camera noise sample must be an object", "type_mismatch",
                                  "samples[" + std::to_string(index) + "]");
        exact = expect_keys(*sample, {"count", "signal_mean", "variance"},
                            "samples[" + std::to_string(index) + "]");
        if (!exact)
            return exact.error();
        auto mean = require_double(*sample, "signal_mean", 0.0, kMaximumSignal, true);
        auto variance = require_double(*sample, "variance", 0.0, kMaximumVariance, false);
        auto count = require_integer<std::uint64_t>(*sample, "count", 1ULL, 1'000'000'000ULL);
        if (!mean)
            return mean.error();
        if (!variance)
            return variance.error();
        if (!count)
            return count.error();
        result.samples.push_back({mean.value(), variance.value(), count.value()});
    }
    return result;
}

Result<std::string>
serialize_camera_noise_calibration_json(const CameraNoiseCalibrationDocument &document)
{
    if (document.identity.make.empty() ||
        document.identity.make.size() > kIdentityTextMaximumBytes ||
        document.identity.model.empty() ||
        document.identity.model.size() > kIdentityTextMaximumBytes || document.identity.iso == 0U ||
        document.identity.iso > 1'000'000U)
        return document_error("Camera noise identity is invalid", "invalid_identity");
    if (document.samples.size() < kCameraNoiseMinimumSamples ||
        document.samples.size() > kCameraNoiseMaximumSamples)
        return document_error("Camera noise sample count is outside the supported range",
                              "sample_count_out_of_range", "samples");
    JsonValue::Array samples;
    samples.reserve(document.samples.size());
    for (const auto &sample : document.samples)
    {
        auto mean = number_text(sample.signal_mean);
        auto variance = number_text(sample.variance);
        if (!mean)
            return mean.error();
        if (!variance)
            return variance.error();
        if (sample.signal_mean < 0.0 || sample.signal_mean > kMaximumSignal ||
            sample.variance <= 0.0 || sample.variance > kMaximumVariance || sample.count == 0U ||
            sample.count > 1'000'000'000ULL)
            return document_error("Camera noise sample is invalid", "invalid_sample");
        samples.push_back(JsonValue::Object{
            {"count", JsonValue::number(std::to_string(sample.count))},
            {"signal_mean", JsonValue::number(std::move(mean).value())},
            {"variance", JsonValue::number(std::move(variance).value())},
        });
    }
    return serialize_json(JsonValue::Object{
               {"identity", identity_json(document.identity)},
               {"samples", std::move(samples)},
               {"schema", std::string(kCameraNoiseSampleSchema)},
               {"units", std::string(kCameraNoiseSignalUnits)},
               {"version", JsonValue::number(std::to_string(kCameraNoiseSampleSchemaVersion))},
           }) +
           "\n";
}

Result<std::string> camera_noise_calibration_sha256(const CameraNoiseCalibrationDocument &document)
{
    auto canonical = serialize_camera_noise_calibration_json(document);
    if (!canonical)
        return canonical.error();
    return sha256(canonical.value());
}

Result<std::string>
serialize_camera_noise_profile_json(const CameraNoiseIdentity &identity, const CameraNoiseFit &fit,
                                    const std::string_view source_samples_sha256)
{
    auto payload = profile_payload(identity, fit, source_samples_sha256);
    if (!payload)
        return payload.error();
    const auto canonical_payload = serialize_json(payload.value());
    const auto checksum = sha256(canonical_payload);
    return serialize_json(JsonValue::Object{
               {"checksum", JsonValue::Object{{"algorithm", "sha256"}, {"value", checksum}}},
               {"payload", std::move(payload).value()},
           }) +
           "\n";
}

Result<CameraNoiseProfile> parse_camera_noise_profile_json(const std::string_view text)
{
    auto root = parse_json(text);
    if (!root)
        return root.error();
    const auto *object = root.value().object_if();
    if (object == nullptr)
        return document_error("Camera noise profile must be an object", "type_mismatch");
    auto exact = expect_keys(*object, {"checksum", "payload"}, "profile");
    if (!exact)
        return exact.error();
    auto checksum = require_object(root.value().find("checksum"), "checksum");
    if (!checksum)
        return checksum.error();
    exact = expect_keys(*checksum.value(), {"algorithm", "value"}, "checksum");
    if (!exact)
        return exact.error();
    auto algorithm = require_string(*checksum.value(), "algorithm", 16U);
    auto checksum_value = require_string(*checksum.value(), "value", 64U);
    if (!algorithm)
        return algorithm.error();
    if (!checksum_value)
        return checksum_value.error();
    if (algorithm.value() != "sha256" || !is_sha256(checksum_value.value()))
        return document_error("Camera noise profile checksum declaration is invalid",
                              "invalid_checksum", "checksum");
    const auto *payload_value = root.value().find("payload");
    auto payload = require_object(payload_value, "payload");
    if (!payload)
        return payload.error();
    exact = expect_keys(*payload.value(),
                        {"fit", "fit_policy", "identity", "model", "schema",
                         "source_samples_sha256", "units", "version"},
                        "payload");
    if (!exact)
        return exact.error();
    if (sha256(serialize_json(*payload_value)) != checksum_value.value())
        return document_error("Camera noise profile checksum does not match its payload",
                              "checksum_mismatch", "checksum.value");

    auto schema = require_string(*payload.value(), "schema", 64U);
    auto units = require_string(*payload.value(), "units", 64U);
    auto model = require_string(*payload.value(), "model", 128U);
    auto policy = require_string(*payload.value(), "fit_policy", 128U);
    auto source_sha = require_string(*payload.value(), "source_samples_sha256", 64U);
    auto version = require_integer<std::uint32_t>(*payload.value(), "version", 1U, 1U);
    if (!schema || schema.value() != kCameraNoiseProfileSchema)
        return document_error("Unsupported camera noise profile schema", "unsupported_schema");
    if (!units || units.value() != kCameraNoiseSignalUnits)
        return document_error("Unsupported camera noise profile units", "unsupported_units");
    if (!model || model.value() != kCameraNoiseModel)
        return document_error("Unsupported camera noise model", "unsupported_model");
    if (!policy || policy.value() != kCameraNoiseFitPolicy)
        return document_error("Unsupported camera noise fit policy", "unsupported_fit_policy");
    if (!source_sha || !is_sha256(source_sha.value()))
        return document_error("Camera noise source checksum is invalid", "invalid_checksum");
    if (!version)
        return version.error();
    auto identity = parse_identity(payload_value->find("identity"));
    if (!identity)
        return identity.error();
    auto fit = require_object(payload_value->find("fit"), "fit");
    if (!fit)
        return fit.error();
    exact = expect_keys(*fit.value(),
                        {"gaussian_variance", "input_sample_count", "poisson_slope",
                         "retained_sample_count", "weighted_r_squared", "weighted_rmse"},
                        "fit");
    if (!exact)
        return exact.error();
    auto gaussian = require_double(*fit.value(), "gaussian_variance", 0.0, kMaximumVariance, true);
    auto poisson = require_double(*fit.value(), "poisson_slope", 0.0, kMaximumSignal, true);
    auto rmse = require_double(*fit.value(), "weighted_rmse", 0.0, kMaximumVariance, true);
    auto r_squared = require_double(*fit.value(), "weighted_r_squared",
                                    -std::numeric_limits<double>::max(), 1.0, true);
    auto input_count =
        require_integer<std::uint64_t>(*fit.value(), "input_sample_count", 1ULL, 1'000'000ULL);
    auto retained_count =
        require_integer<std::uint64_t>(*fit.value(), "retained_sample_count", 1ULL, 1'000'000ULL);
    if (!gaussian)
        return gaussian.error();
    if (!poisson)
        return poisson.error();
    if (!rmse)
        return rmse.error();
    if (!r_squared)
        return r_squared.error();
    if (!input_count)
        return input_count.error();
    if (!retained_count)
        return retained_count.error();
    if (retained_count.value() > input_count.value())
        return document_error("Camera noise retained sample count exceeds input count",
                              "invalid_fit_counts");

    return CameraNoiseProfile{std::move(identity).value(),
                              CameraNoiseFit{gaussian.value(), poisson.value(), rmse.value(),
                                             r_squared.value(),
                                             static_cast<std::size_t>(input_count.value()),
                                             static_cast<std::size_t>(retained_count.value())},
                              std::move(source_sha).value(), std::move(checksum_value).value()};
}

} // namespace ravo
