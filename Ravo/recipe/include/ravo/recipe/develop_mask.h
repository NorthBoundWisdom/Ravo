#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

// Mask authoring is intentionally a recipe-level Develop helper. It owns the
// stable field names, canonical typed-node lifecycle, and Studio-reserved IDs;
// services and QML only pass the numeric intents through their existing paths.
enum class DevelopMaskTarget
{
    kColorHarmonizer,
    kGraduatedNd,
    kColorBalanceRgb,
    kExposure,
};

inline constexpr std::string_view kColorHarmonizerMaskFieldPrefix = "colorHarmonizerMask";
inline constexpr std::string_view kGraduatedMaskFieldPrefix = "graduatedMask";
inline constexpr std::string_view kColorBalanceRgbMaskFieldPrefix = "colorBalanceRgbMask";
inline constexpr std::string_view kExposureMaskFieldPrefix = "exposureMask";

enum class DevelopMaskAttachmentStatus
{
    kNoMask,
    kEditable,
    kExternalReadOnly,
    kSharedReadOnly,
    kGroupReadOnly,
    kInvalid,
};

// This value-only snapshot intentionally mirrors only canonical typed mask
// values. It has no QVariant/QObject ownership and carries no evaluator math.
struct DevelopMaskEditorState
{
    bool attached = false;
    bool editable = true;
    bool can_detach = false;
    std::int64_t kind_index = 0;
    std::string kind_name{"none"};
    DevelopMaskAttachmentStatus status = DevelopMaskAttachmentStatus::kNoMask;
    double opacity = 1.0;
    bool inverted = false;
    double anchor_x = 0.5;
    double anchor_y = 0.5;
    double rotation_degrees = 0.0;
    double transition = 0.1;
    double center_x = 0.5;
    double center_y = 0.5;
    double radius = 0.25;
    double feather = 0.0;
    double radius_x = 0.25;
    double radius_y = 0.25;
    std::int64_t source_index = 0;
    std::int64_t channel_index = 0;
    double threshold0 = 0.0;
    double threshold1 = 0.0;
    double threshold2 = 1.0;
    double threshold3 = 1.0;
    std::int64_t child_count = 0;
    std::int64_t child_index = 0;
    std::int64_t child_kind_index = 0;
    std::string child_kind_name{"all"};
    std::int64_t child_operator_index = 0;
    double child_opacity = 1.0;
    bool child_inverted = false;
    std::int64_t point_count = 0;
    std::int64_t point_index = 0;
    double point_x = 0.5;
    double point_y = 0.5;
    double point_radius = 0.05;
    double point_hardness = 0.5;
    double point_density = 1.0;
    double path_feather = 0.0;
};

[[nodiscard]] std::string_view develop_mask_target_name(DevelopMaskTarget target) noexcept;
[[nodiscard]] std::string_view
develop_mask_attachment_status_name(DevelopMaskAttachmentStatus status) noexcept;
[[nodiscard]] bool is_develop_mask_field(std::string_view field) noexcept;
[[nodiscard]] DevelopMaskEditorState develop_mask_editor_state(const DevelopParams &params,
                                                               DevelopMaskTarget target);

// Both entry points work transactionally: they leave `params` untouched on any
// invalid field/value/graph/ownership result. `kind=0` and the whole-target
// reset are the explicit detach intents; no ordinary field can modify an
// external, shared, or group attachment.
[[nodiscard]] Result<void> apply_develop_mask_field_strict(DevelopParams &params,
                                                           std::string_view field, double value);
[[nodiscard]] Result<void> reset_develop_mask_field(DevelopParams &params, std::string_view field);

} // namespace ravo
