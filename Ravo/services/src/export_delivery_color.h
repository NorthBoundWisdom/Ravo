#pragma once

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] Result<OutputColorParams>
output_color_params_from_export_options(const ExportColorOptions &options);

// Clone recipe and replace/insert ravo.color.output for delivery colour (proof off).
[[nodiscard]] Result<Recipe> apply_export_color_override(Recipe recipe,
                                                         const ExportColorOptions &options);

} // namespace ravo
