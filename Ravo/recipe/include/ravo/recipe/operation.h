#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kDemosaicOperationId = "ravo.raw.demosaic";
inline constexpr std::string_view kDemosaicModeRcd = "rcd";
inline constexpr std::string_view kDemosaicModePpg = "ppg";
inline constexpr std::string_view kDemosaicModeMarkesteijn1 = "markesteijn1";
inline constexpr std::string_view kDemosaicModeMarkesteijn3 = "markesteijn3";

[[nodiscard]] Result<std::string> demosaic_mode_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> validate_demosaic_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

inline constexpr std::string_view kExposureOperationId = "ravo.core.exposure";
inline constexpr std::int64_t kExposureOperationSchemaVersion = 2;
inline constexpr std::string_view kExposureModeManual = "manual";
inline constexpr std::string_view kExposureModeDeflicker = "deflicker";

inline constexpr double kExposureBlackMin = -1.0;
inline constexpr double kExposureBlackMax = 1.0;
inline constexpr double kExposureEvMin = -18.0;
inline constexpr double kExposureEvMax = 18.0;
inline constexpr double kExposureDeflickerPercentileMin = 0.0;
inline constexpr double kExposureDeflickerPercentileMax = 100.0;
inline constexpr double kExposureDeflickerPercentileDefault = 50.0;
inline constexpr double kExposureDeflickerTargetEvMin = -18.0;
inline constexpr double kExposureDeflickerTargetEvMax = 18.0;
inline constexpr double kExposureDeflickerTargetEvDefault = -4.0;

struct ExposureParams
{
    std::string mode{std::string(kExposureModeManual)};
    double black = 0.0;
    double exposure_ev = 0.0;
    double deflicker_percentile = kExposureDeflickerPercentileDefault;
    double deflicker_target_ev = kExposureDeflickerTargetEvDefault;
    bool compensate_exposure_bias = false;
    bool compensate_highlight_preservation = false;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const ExposureParams &) const noexcept = default;
};

[[nodiscard]] Result<ExposureParams>
exposure_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
exposure_to_parameters(const ExposureParams &params);
[[nodiscard]] Result<void>
validate_exposure_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> upgrade_exposure_operation(OperationInstance &operation);

enum class ParameterType
{
    kBoolean,
    kInteger,
    kNumber,
    kString,
    kArray,
    kObject,
};

struct ParameterRule
{
    std::string name;
    ParameterType type = ParameterType::kString;
    bool required = false;
    std::optional<ParameterValue> default_value;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct OperationDescriptor
{
    std::string id;
    std::string display_name;
    std::int64_t parameter_schema_version = 1;
    std::vector<ParameterRule> parameters;
    bool supports_mask = false;
    bool cpu_reference_available = false;
};

class OperationRegistry
{
public:
    [[nodiscard]] static Result<OperationRegistry>
    create(std::vector<OperationDescriptor> descriptors);

    [[nodiscard]] const OperationDescriptor *find(std::string_view id) const noexcept;
    [[nodiscard]] const std::vector<OperationDescriptor> &descriptors() const noexcept;

private:
    explicit OperationRegistry(std::vector<OperationDescriptor> descriptors);

    std::vector<OperationDescriptor> descriptors_;
    std::map<std::string, std::size_t, std::less<>> indexes_;
};

inline constexpr std::size_t kPhase1OperationCount = 62;

[[nodiscard]] Result<OperationRegistry> make_phase1_registry();
[[nodiscard]] std::string_view parameter_type_name(ParameterType type) noexcept;
[[nodiscard]] Result<JsonValue> operation_descriptor_to_json(const OperationDescriptor &descriptor);

} // namespace ravo
