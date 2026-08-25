#include "ravo/recipe/operation.h"

#include <cmath>
#include <utility>

namespace ravo
{

OperationRegistry::OperationRegistry(std::vector<OperationDescriptor> descriptors)
    : descriptors_(std::move(descriptors))
{
    for (std::size_t index = 0; index < descriptors_.size(); ++index)
    {
        indexes_.emplace(descriptors_[index].id, index);
    }
}

Result<OperationRegistry> OperationRegistry::create(std::vector<OperationDescriptor> descriptors)
{
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < descriptors.size(); ++index)
    {
        const auto &descriptor = descriptors[index];
        if (descriptor.id.empty())
        {
            return make_error(ErrorCode::kValidation, "Operation descriptor ID must not be empty",
                              {{"descriptor_index", std::to_string(index)}});
        }
        if (descriptor.parameter_schema_version < 1)
        {
            return make_error(ErrorCode::kValidation,
                              "Operation descriptor schema version must be positive",
                              {{"operation_id", descriptor.id}});
        }
        if (!indexes.emplace(descriptor.id, index).second)
        {
            return make_error(ErrorCode::kConflict, "Duplicate operation descriptor ID",
                              {{"operation_id", descriptor.id}});
        }

        std::map<std::string, bool, std::less<>> parameter_names;
        for (const auto &parameter : descriptor.parameters)
        {
            if (parameter.name.empty() || !parameter_names.emplace(parameter.name, true).second)
            {
                return make_error(ErrorCode::kValidation,
                                  "Operation descriptor has an invalid parameter name",
                                  {{"operation_id", descriptor.id}, {"parameter", parameter.name}});
            }
            if ((parameter.minimum.has_value() && !std::isfinite(*parameter.minimum)) ||
                (parameter.maximum.has_value() && !std::isfinite(*parameter.maximum)) ||
                (parameter.minimum.has_value() && parameter.maximum.has_value() &&
                 *parameter.minimum > *parameter.maximum))
            {
                return make_error(ErrorCode::kValidation,
                                  "Operation descriptor has an invalid numeric range",
                                  {{"operation_id", descriptor.id}, {"parameter", parameter.name}});
            }
        }
    }

    return OperationRegistry{std::move(descriptors)};
}

const OperationDescriptor *OperationRegistry::find(const std::string_view id) const noexcept
{
    const auto iterator = indexes_.find(std::string(id));
    if (iterator == indexes_.end())
    {
        return nullptr;
    }
    return &descriptors_[iterator->second];
}

const std::vector<OperationDescriptor> &OperationRegistry::descriptors() const noexcept
{
    return descriptors_;
}

Result<OperationRegistry> make_phase1_registry()
{
    return OperationRegistry::create({
        {"ravo.core.identity", "Identity", 1, {}, false, false},
        {"ravo.raw.prepare", "RAW prepare", 1, {}, false, false},
        {"ravo.raw.demosaic", "RAW demosaic", 1, {}, false, false},
        {"ravo.color.input", "Input colour", 1, {}, false, false},
        {"ravo.core.exposure",
         "Exposure",
         1,
         {{"exposure_ev", ParameterType::kNumber, false, ParameterValue{0.0}, -10.0, 10.0}},
         false,
         true},
        {"ravo.color.white_balance",
         "White balance",
         1,
         {{"temperature", ParameterType::kNumber, false, ParameterValue{6500.0}, 2000.0, 12000.0},
          {"tint", ParameterType::kNumber, false, ParameterValue{0.0}, -150.0, 150.0}},
         false,
         true},
        {"ravo.core.contrast",
         "Contrast",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.highlights",
         "Highlights",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.shadows",
         "Shadows",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.whites",
         "Whites",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.core.blacks",
         "Blacks",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.vibrance",
         "Vibrance",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.saturation",
         "Saturation",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.geometry.rotate",
         "Rotate",
         1,
         {{"quarters", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 3.0}},
         false,
         true},
        {"ravo.geometry.crop",
         "Crop",
         1,
         {{"x", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"y", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"width", ParameterType::kNumber, false, ParameterValue{1.0}, 0.01, 1.0},
          {"height", ParameterType::kNumber, false, ParameterValue{1.0}, 0.01, 1.0}},
         false,
         true},
        {"ravo.geometry.flip",
         "Flip",
         1,
         {{"horizontal", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 1.0},
          {"vertical", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0, 1.0}},
         false,
         true},
        {"ravo.core.gamma",
         "Gamma",
         1,
         {{"gamma", ParameterType::kNumber, false, ParameterValue{1.0}, 0.2, 3.0}},
         false,
         true},
        {"ravo.color.colorbalance",
         "Color balance",
         1,
         {{"lift", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"gamma", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0},
          {"gain", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.colorcontrast",
         "Color contrast",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.velvia",
         "Velvia",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"bias", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.color.monochrome",
         "Monochrome",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.color.splittoning",
         "Split toning",
         1,
         {{"shadows_hue", ParameterType::kNumber, false, ParameterValue{0.55}, 0.0, 1.0},
          {"highlights_hue", ParameterType::kNumber, false, ParameterValue{0.08}, 0.0, 1.0},
          {"balance", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.detail.sharpen",
         "Sharpen",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 2.0},
          {"radius", ParameterType::kNumber, false, ParameterValue{1.0}, 0.0, 12.0},
          {"threshold", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.detail.clarity",
         "Clarity",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.effect.vignette",
         "Vignette",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0},
          {"midpoint", ParameterType::kNumber, false, ParameterValue{0.5}, 0.0, 1.0},
          {"falloff", ParameterType::kNumber, false, ParameterValue{0.5}, 0.05, 1.0}},
         false,
         true},
        {"ravo.effect.grain",
         "Grain",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.bloom",
         "Bloom",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.soften",
         "Soften",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, 0.0, 1.0}},
         false,
         true},
        {"ravo.effect.dehaze",
         "Dehaze",
         1,
         {{"amount", ParameterType::kNumber, false, ParameterValue{0.0}, -1.0, 1.0}},
         false,
         true},
        {"ravo.color.output", "Output colour", 1, {}, false, false},
        {"ravo.output.scale",
         "Output scale",
         1,
         {{"max_dimension", ParameterType::kInteger, false, ParameterValue{std::int64_t{0}}, 0.0,
           100000.0}},
         false,
         false},
    });
}

std::string_view parameter_type_name(const ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::kBoolean:
        return "boolean";
    case ParameterType::kInteger:
        return "integer";
    case ParameterType::kNumber:
        return "number";
    case ParameterType::kString:
        return "string";
    case ParameterType::kArray:
        return "array";
    case ParameterType::kObject:
        return "object";
    }
    return "string";
}

Result<JsonValue> operation_descriptor_to_json(const OperationDescriptor &descriptor)
{
    JsonValue::Array parameters;
    for (const auto &rule : descriptor.parameters)
    {
        JsonValue::Object parameter{
            {"name", rule.name},
            {"required", rule.required},
            {"type", std::string(parameter_type_name(rule.type))},
        };
        if (rule.default_value.has_value())
        {
            auto default_value = parameter_value_to_json(*rule.default_value);
            if (!default_value)
            {
                return default_value.error();
            }
            parameter.emplace("default", std::move(default_value).value());
        }
        if (rule.minimum.has_value())
        {
            parameter.emplace("minimum", JsonValue::number(std::to_string(*rule.minimum)));
        }
        if (rule.maximum.has_value())
        {
            parameter.emplace("maximum", JsonValue::number(std::to_string(*rule.maximum)));
        }
        parameters.emplace_back(std::move(parameter));
    }

    return JsonValue{JsonValue::Object{
        {"capabilities",
         JsonValue::Object{{"cpu_reference_available", descriptor.cpu_reference_available},
                           {"supports_mask", descriptor.supports_mask}}},
        {"display_name", descriptor.display_name},
        {"id", descriptor.id},
        {"parameter_schema_version",
         JsonValue::number(std::to_string(descriptor.parameter_schema_version))},
        {"parameters", std::move(parameters)},
    }};
}

} // namespace ravo
