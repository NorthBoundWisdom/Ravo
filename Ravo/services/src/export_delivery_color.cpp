#include "export_delivery_color.h"

#include <utility>

namespace ravo
{

Result<OutputColorParams> output_color_params_from_export_options(const ExportColorOptions &options)
{
    auto valid = validate_export_color_options(options);
    if (!valid)
        return valid.error();
    OutputColorParams params;
    params.output_profile = options.output_profile;
    params.output_profile_filename = options.output_profile_filename;
    params.rendering_intent = options.rendering_intent;
    params.proof_mode = std::string(kProofModeOff);
    params.proof_profile = std::string(kInputProfileSrgb);
    params.proof_profile_filename.clear();
    params.proof_intent = std::string(kColorIntentRelative);
    params.black_point_compensation = options.black_point_compensation;
    auto canonical = output_color_to_parameters(params);
    auto roundtrip = output_color_from_parameters(canonical);
    if (!roundtrip)
        return roundtrip.error();
    return params;
}

Result<Recipe> apply_export_color_override(Recipe recipe, const ExportColorOptions &options)
{
    if (!options.enabled)
        return recipe;
    auto params = output_color_params_from_export_options(options);
    if (!params)
        return params.error();
    auto parameters = output_color_to_parameters(params.value());
    OperationInstance *selected = nullptr;
    for (auto &operation : recipe.operations)
    {
        if (operation.id != "ravo.color.output")
            continue;
        if (selected != nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Recipe contains duplicate Output Color operations",
                              {{"reason", "duplicate_output_color"}});
        }
        selected = &operation;
    }
    if (selected == nullptr)
    {
        recipe.operations.push_back({"ravo.color.output", 1, "export-color-output-1", true,
                                     std::move(parameters), std::nullopt});
        return recipe;
    }
    selected->enabled = true;
    selected->schema_version = 1;
    selected->parameters = std::move(parameters);
    selected->mask_id.reset();
    return recipe;
}

} // namespace ravo
