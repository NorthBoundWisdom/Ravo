#pragma once

#include <QVariantMap>

#include "ravo/recipe/develop_mask.h"

namespace ravo::studio_develop_internal
{

[[nodiscard]] QVariantMap develop_mask_editor_map(const DevelopMaskEditorState &state,
                                                  DevelopMaskTarget target);

} // namespace ravo::studio_develop_internal
