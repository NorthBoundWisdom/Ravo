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

inline constexpr std::size_t kPhase1OperationCount = 33;

[[nodiscard]] Result<OperationRegistry> make_phase1_registry();
[[nodiscard]] std::string_view parameter_type_name(ParameterType type) noexcept;
[[nodiscard]] Result<JsonValue> operation_descriptor_to_json(const OperationDescriptor &descriptor);

} // namespace ravo
