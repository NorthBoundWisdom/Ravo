#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include <QString>
#include <QVariantMap>

#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

namespace ravo::studio_develop_internal
{

[[nodiscard]] QVariantMap develop_mask_editor_map(const DevelopMaskEditorState &state,
                                                  DevelopMaskTarget target);
[[nodiscard]] QString develop_mask_field_prefix(DevelopMaskTarget target);
[[nodiscard]] std::optional<DevelopMaskTarget>
develop_mask_target_from_name(std::string_view name) noexcept;
[[nodiscard]] bool mask_place_geometry_allowed(const DevelopParams &params) noexcept;
[[nodiscard]] Result<std::pair<double, double>>
map_mask_place_preview(const DevelopParams &params, double preview_x, double preview_y);

} // namespace ravo::studio_develop_internal
