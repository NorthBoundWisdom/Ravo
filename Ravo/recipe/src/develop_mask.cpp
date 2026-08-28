#include "ravo/recipe/develop_mask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

enum class MaskField
{
    kWhole,
    kKind,
    kOpacity,
    kInverted,
    kAnchorX,
    kAnchorY,
    kRotationDegrees,
    kTransition,
    kCenterX,
    kCenterY,
    kRadius,
    kFeather,
    kRadiusX,
    kRadiusY,
    kSource,
    kChannel,
    kThreshold0,
    kThreshold1,
    kThreshold2,
    kThreshold3,
};

struct ParsedMaskField
{
    DevelopMaskTarget target;
    MaskField field;
};

[[nodiscard]] std::optional<ParsedMaskField> parse_mask_field(const std::string_view field) noexcept
{
    const auto parse_suffix = [](const DevelopMaskTarget target,
                                 const std::string_view suffix) -> std::optional<ParsedMaskField>
    {
        const auto parsed = [target](const MaskField value)
        { return std::optional<ParsedMaskField>{{target, value}}; };
        if (suffix.empty())
            return parsed(MaskField::kWhole);
        if (suffix == "Kind")
            return parsed(MaskField::kKind);
        if (suffix == "Opacity")
            return parsed(MaskField::kOpacity);
        if (suffix == "Inverted")
            return parsed(MaskField::kInverted);
        if (suffix == "AnchorX")
            return parsed(MaskField::kAnchorX);
        if (suffix == "AnchorY")
            return parsed(MaskField::kAnchorY);
        if (suffix == "RotationDegrees")
            return parsed(MaskField::kRotationDegrees);
        if (suffix == "Transition")
            return parsed(MaskField::kTransition);
        if (suffix == "CenterX")
            return parsed(MaskField::kCenterX);
        if (suffix == "CenterY")
            return parsed(MaskField::kCenterY);
        if (suffix == "Radius")
            return parsed(MaskField::kRadius);
        if (suffix == "Feather")
            return parsed(MaskField::kFeather);
        if (suffix == "RadiusX")
            return parsed(MaskField::kRadiusX);
        if (suffix == "RadiusY")
            return parsed(MaskField::kRadiusY);
        if (suffix == "Source")
            return parsed(MaskField::kSource);
        if (suffix == "Channel")
            return parsed(MaskField::kChannel);
        if (suffix == "Threshold0")
            return parsed(MaskField::kThreshold0);
        if (suffix == "Threshold1")
            return parsed(MaskField::kThreshold1);
        if (suffix == "Threshold2")
            return parsed(MaskField::kThreshold2);
        if (suffix == "Threshold3")
            return parsed(MaskField::kThreshold3);
        return std::nullopt;
    };

    if (field.starts_with(kColorHarmonizerMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kColorHarmonizer,
                            field.substr(kColorHarmonizerMaskFieldPrefix.size()));
    }
    if (field.starts_with(kGraduatedMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kGraduatedNd,
                            field.substr(kGraduatedMaskFieldPrefix.size()));
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view studio_mask_id_prefix(const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return "ravo.studio.mask.color_harmonizer.";
    case DevelopMaskTarget::kGraduatedNd:
        return "ravo.studio.mask.graduatednd.";
    }
    return {};
}

[[nodiscard]] bool studio_owns_mask_id(const DevelopMaskTarget target,
                                       const std::string_view id) noexcept
{
    const auto prefix = studio_mask_id_prefix(target);
    if (!id.starts_with(prefix) || id.size() == prefix.size())
    {
        return false;
    }
    const auto suffix = id.substr(prefix.size());
    if (suffix.front() == '0')
    {
        return false;
    }
    return std::all_of(suffix.begin(), suffix.end(),
                       [](const char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] std::optional<std::string> &mask_attachment(DevelopParams &params,
                                                          const DevelopMaskTarget target) noexcept
{
    return target == DevelopMaskTarget::kColorHarmonizer ? params.color_harmonizer_mask_id :
                                                           params.graduated_mask_id;
}

[[nodiscard]] const std::optional<std::string> &
mask_attachment(const DevelopParams &params, const DevelopMaskTarget target) noexcept
{
    return target == DevelopMaskTarget::kColorHarmonizer ? params.color_harmonizer_mask_id :
                                                           params.graduated_mask_id;
}

[[nodiscard]] Mask *find_mask(std::vector<Mask> &masks, const std::string_view id) noexcept
{
    const auto found =
        std::find_if(masks.begin(), masks.end(), [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

[[nodiscard]] const Mask *find_mask(const std::vector<Mask> &masks,
                                    const std::string_view id) noexcept
{
    const auto found =
        std::find_if(masks.begin(), masks.end(), [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

[[nodiscard]] bool develop_mask_attachments_resolve(const DevelopParams &params) noexcept
{
    for (const auto target : {DevelopMaskTarget::kColorHarmonizer, DevelopMaskTarget::kGraduatedNd})
    {
        const auto &attachment = mask_attachment(params, target);
        if (attachment && find_mask(params.masks, *attachment) == nullptr)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_group_edge_to(const DevelopParams &params,
                                     const std::string_view id) noexcept
{
    for (const auto &mask : params.masks)
    {
        const auto *group = std::get_if<MaskGroup>(&mask.payload);
        if (group == nullptr)
        {
            continue;
        }
        if (std::any_of(group->children.begin(), group->children.end(),
                        [id](const MaskGroupChild &child) { return child.mask_id == id; }))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_attached_by_other_operation(const DevelopParams &params,
                                                  const DevelopMaskTarget target,
                                                  const std::string_view id) noexcept
{
    const auto other = target == DevelopMaskTarget::kColorHarmonizer ?
                           DevelopMaskTarget::kGraduatedNd :
                           DevelopMaskTarget::kColorHarmonizer;
    const auto &other_attachment = mask_attachment(params, other);
    return other_attachment.has_value() && *other_attachment == id;
}

[[nodiscard]] TaskError mask_edit_error(const ErrorCode code, std::string message,
                                        const std::string_view reason,
                                        const DevelopMaskTarget target,
                                        const std::string_view field)
{
    return make_error(code, std::move(message),
                      {{"field", std::string(field)},
                       {"reason", std::string(reason)},
                       {"target", std::string(develop_mask_target_name(target))}});
}

[[nodiscard]] TaskError decorate_mask_error(TaskError error, const DevelopMaskTarget target,
                                            const std::string_view field)
{
    error.context.insert_or_assign("field", std::string(field));
    error.context.insert_or_assign("target", std::string(develop_mask_target_name(target)));
    if (!error.context.contains("reason"))
    {
        error.context.emplace("reason", "invalid_develop_mask_graph");
    }
    return error;
}

[[nodiscard]] Result<void> validate_develop_mask_state(const DevelopParams &params,
                                                       const DevelopMaskTarget target,
                                                       const std::string_view field)
{
    auto graph = validate_mask_graph(params.masks);
    if (!graph)
    {
        return decorate_mask_error(graph.error(), target, field);
    }
    if (!develop_mask_attachments_resolve(params))
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Develop mask attachment does not resolve to a canonical node",
                               "missing_develop_mask_attachment", target, field);
    }
    return {};
}

[[nodiscard]] std::int64_t mask_kind_index(const MaskKind kind) noexcept
{
    switch (kind)
    {
    case MaskKind::kAll:
        return 1;
    case MaskKind::kLinearGradient:
        return 2;
    case MaskKind::kCircle:
        return 3;
    case MaskKind::kEllipse:
        return 4;
    case MaskKind::kParametric:
        return 5;
    case MaskKind::kGroup:
        return -1;
    }
    return -1;
}

[[nodiscard]] std::optional<MaskKind> mask_kind_from_index(const std::int64_t index) noexcept
{
    switch (index)
    {
    case 1:
        return MaskKind::kAll;
    case 2:
        return MaskKind::kLinearGradient;
    case 3:
        return MaskKind::kCircle;
    case 4:
        return MaskKind::kEllipse;
    case 5:
        return MaskKind::kParametric;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] Result<std::int64_t> exact_index(const double value, const std::int64_t minimum,
                                               const std::int64_t maximum,
                                               const DevelopMaskTarget target,
                                               const std::string_view field,
                                               const std::string_view reason)
{
    if (!std::isfinite(value) || value != std::trunc(value) ||
        value < static_cast<double>(minimum) || value > static_cast<double>(maximum))
    {
        return mask_edit_error(ErrorCode::kInvalidArgument,
                               "Develop mask selector must be a known exact integer", reason,
                               target, field);
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] Result<void> finite_range(const double value, const double minimum,
                                        const double maximum, const DevelopMaskTarget target,
                                        const std::string_view field)
{
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        return mask_edit_error(ErrorCode::kInvalidArgument,
                               "Develop mask value is outside the canonical bounds",
                               "invalid_develop_mask_value", target, field);
    }
    return {};
}

[[nodiscard]] Result<void> require_editable(const DevelopParams &params,
                                            const DevelopMaskTarget target,
                                            const std::string_view field)
{
    const auto state = develop_mask_editor_state(params, target);
    if (!state.attached)
    {
        return mask_edit_error(ErrorCode::kNotFound,
                               "Develop mask field requires an attached canonical mask",
                               "missing_develop_mask_attachment", target, field);
    }
    if (state.editable)
    {
        return {};
    }
    const auto reason = develop_mask_attachment_status_name(state.status);
    return mask_edit_error(ErrorCode::kUnsupported,
                           "Attached Develop mask is read-only for Studio authoring", reason,
                           target, field);
}

void enable_target_operation(DevelopParams &params, const DevelopMaskTarget target) noexcept
{
    if (target == DevelopMaskTarget::kColorHarmonizer)
    {
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return;
    }
    params.graduated_present = true;
    params.graduated_enabled = true;
}

[[nodiscard]] Result<std::string> next_studio_mask_id(const DevelopParams &params,
                                                      const DevelopMaskTarget target,
                                                      const std::string_view field)
{
    if (params.masks.size() >= kCanonicalMaskMaxNodes)
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Canonical mask graph has reached its node limit",
                               "develop_mask_node_limit", target, field);
    }
    const auto prefix = studio_mask_id_prefix(target);
    for (std::uint64_t ordinal = 1U; ordinal < std::numeric_limits<std::uint64_t>::max(); ++ordinal)
    {
        std::string candidate{prefix};
        candidate += std::to_string(ordinal);
        if (find_mask(params.masks, candidate) == nullptr)
        {
            return candidate;
        }
    }
    return mask_edit_error(ErrorCode::kConflict,
                           "Unable to allocate a collision-safe Studio mask ID",
                           "develop_mask_id_exhausted", target, field);
}

[[nodiscard]] Result<void> detach_mask(DevelopParams &params, const DevelopMaskTarget target,
                                       const std::string_view field)
{
    auto valid = validate_develop_mask_state(params, target, field);
    if (!valid)
    {
        return valid.error();
    }
    auto &attachment = mask_attachment(params, target);
    if (!attachment)
    {
        return {};
    }
    const auto state = develop_mask_editor_state(params, target);
    const std::string id = *attachment;
    attachment.reset();
    if (state.status == DevelopMaskAttachmentStatus::kEditable)
    {
        params.masks.erase(std::remove_if(params.masks.begin(), params.masks.end(),
                                          [&id](const Mask &mask) { return mask.id == id; }),
                           params.masks.end());
    }
    return validate_develop_mask_state(params, target, field);
}

[[nodiscard]] Result<void> type_mismatch(const Mask &, const DevelopMaskTarget target,
                                         const std::string_view field)
{
    return mask_edit_error(ErrorCode::kUnsupported,
                           "Develop mask field is unavailable for the attached mask kind",
                           "develop_mask_field_kind_mismatch", target, field);
}

[[nodiscard]] Result<void> apply_mask_value(Mask &mask, const MaskField field, const double value,
                                            const DevelopMaskTarget target,
                                            const std::string_view field_name)
{
    switch (field)
    {
    case MaskField::kOpacity:
    {
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        mask.common.opacity = value;
        return {};
    }
    case MaskField::kInverted:
    {
        auto parsed = exact_index(value, 0, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!parsed)
            return parsed.error();
        mask.common.inverted = parsed.value() == 1;
        return {};
    }
    case MaskField::kAnchorX:
    case MaskField::kAnchorY:
    case MaskField::kTransition:
    {
        auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
        if (gradient == nullptr)
            return type_mismatch(mask, target, field_name);
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kAnchorX)
            gradient->anchor_x = value;
        else if (field == MaskField::kAnchorY)
            gradient->anchor_y = value;
        else
            gradient->transition = value;
        return {};
    }
    case MaskField::kRotationDegrees:
    {
        auto valid =
            finite_range(value, kCanonicalMaskAngleMin, kCanonicalMaskAngleMax, target, field_name);
        if (!valid)
            return valid.error();
        if (auto *gradient = std::get_if<LinearGradientMask>(&mask.payload); gradient != nullptr)
        {
            gradient->rotation_degrees = value;
            return {};
        }
        if (auto *ellipse = std::get_if<EllipseMask>(&mask.payload); ellipse != nullptr)
        {
            ellipse->rotation_degrees = value;
            return {};
        }
        return type_mismatch(mask, target, field_name);
    }
    case MaskField::kCenterX:
    case MaskField::kCenterY:
    case MaskField::kRadius:
    case MaskField::kFeather:
    {
        auto *circle = std::get_if<CircleMask>(&mask.payload);
        if (circle == nullptr)
            return type_mismatch(mask, target, field_name);
        const double minimum =
            field == MaskField::kRadius ? kCanonicalMaskPositiveMin : kCanonicalMaskUnitMin;
        auto valid = finite_range(value, minimum, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kCenterX)
            circle->center_x = value;
        else if (field == MaskField::kCenterY)
            circle->center_y = value;
        else if (field == MaskField::kRadius)
            circle->radius = value;
        else
            circle->feather = value;
        return {};
    }
    case MaskField::kRadiusX:
    case MaskField::kRadiusY:
    {
        auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
        if (ellipse == nullptr)
            return type_mismatch(mask, target, field_name);
        auto valid = finite_range(value, kCanonicalMaskPositiveMin, kCanonicalMaskUnitMax, target,
                                  field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kRadiusX)
            ellipse->radius_x = value;
        else
            ellipse->radius_y = value;
        return {};
    }
    case MaskField::kSource:
    case MaskField::kChannel:
    case MaskField::kThreshold0:
    case MaskField::kThreshold1:
    case MaskField::kThreshold2:
    case MaskField::kThreshold3:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        if (parametric == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kSource)
        {
            auto parsed =
                exact_index(value, 0, 1, target, field_name, "invalid_develop_mask_source");
            if (!parsed)
                return parsed.error();
            parametric->source = parsed.value() == 0 ? ParametricMaskSource::kInput :
                                                       ParametricMaskSource::kOperationOutput;
            return {};
        }
        if (field == MaskField::kChannel)
        {
            auto parsed =
                exact_index(value, 0, 3, target, field_name, "invalid_develop_mask_channel");
            if (!parsed)
                return parsed.error();
            parametric->channel = static_cast<ParametricMaskChannel>(parsed.value());
            return {};
        }
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        const auto index = field == MaskField::kThreshold0 ? 0U :
                           field == MaskField::kThreshold1 ? 1U :
                           field == MaskField::kThreshold2 ? 2U :
                                                             3U;
        parametric->thresholds[index] = value;
        return {};
    }
    case MaskField::kKind:
    case MaskField::kWhole:
        break;
    }
    return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                           "unknown_develop_mask_field", target, field_name);
}

[[nodiscard]] Result<void> reset_mask_value(Mask &mask, const MaskField field,
                                            const DevelopMaskTarget target,
                                            const std::string_view field_name)
{
    if (field == MaskField::kKind)
    {
        Mask replacement{mask.id, kCanonicalMaskSchemaVersion, MaskKind::kAll};
        mask = std::move(replacement);
        return {};
    }
    Mask defaults{mask.id, kCanonicalMaskSchemaVersion, mask.kind};
    switch (field)
    {
    case MaskField::kOpacity:
        mask.common.opacity = defaults.common.opacity;
        return {};
    case MaskField::kInverted:
        mask.common.inverted = defaults.common.inverted;
        return {};
    case MaskField::kAnchorX:
    case MaskField::kAnchorY:
    case MaskField::kTransition:
    {
        auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
        const auto *identity = std::get_if<LinearGradientMask>(&defaults.payload);
        if (gradient == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kAnchorX)
            gradient->anchor_x = identity->anchor_x;
        else if (field == MaskField::kAnchorY)
            gradient->anchor_y = identity->anchor_y;
        else
            gradient->transition = identity->transition;
        return {};
    }
    case MaskField::kRotationDegrees:
    {
        if (auto *gradient = std::get_if<LinearGradientMask>(&mask.payload); gradient != nullptr)
        {
            gradient->rotation_degrees =
                std::get<LinearGradientMask>(defaults.payload).rotation_degrees;
            return {};
        }
        if (auto *ellipse = std::get_if<EllipseMask>(&mask.payload); ellipse != nullptr)
        {
            ellipse->rotation_degrees = std::get<EllipseMask>(defaults.payload).rotation_degrees;
            return {};
        }
        return type_mismatch(mask, target, field_name);
    }
    case MaskField::kCenterX:
    case MaskField::kCenterY:
    case MaskField::kRadius:
    case MaskField::kFeather:
    {
        auto *circle = std::get_if<CircleMask>(&mask.payload);
        const auto *identity = std::get_if<CircleMask>(&defaults.payload);
        if (circle == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kCenterX)
            circle->center_x = identity->center_x;
        else if (field == MaskField::kCenterY)
            circle->center_y = identity->center_y;
        else if (field == MaskField::kRadius)
            circle->radius = identity->radius;
        else
            circle->feather = identity->feather;
        return {};
    }
    case MaskField::kRadiusX:
    case MaskField::kRadiusY:
    {
        auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
        const auto *identity = std::get_if<EllipseMask>(&defaults.payload);
        if (ellipse == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kRadiusX)
            ellipse->radius_x = identity->radius_x;
        else
            ellipse->radius_y = identity->radius_y;
        return {};
    }
    case MaskField::kSource:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        parametric->source = identity->source;
        return {};
    }
    case MaskField::kChannel:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        parametric->channel = identity->channel;
        return {};
    }
    case MaskField::kThreshold0:
    case MaskField::kThreshold1:
    case MaskField::kThreshold2:
    case MaskField::kThreshold3:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        // A single threshold cannot always return to its scalar default
        // without crossing an unchanged neighbour. Restore only the ramp as
        // one atomic value; source and channel remain independently owned.
        parametric->thresholds = identity->thresholds;
        return {};
    }
    case MaskField::kWhole:
    case MaskField::kKind:
        break;
    }
    return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                           "unknown_develop_mask_field", target, field_name);
}

} // namespace

std::string_view develop_mask_target_name(const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return "color_harmonizer";
    case DevelopMaskTarget::kGraduatedNd:
        return "graduatednd";
    }
    return "unknown";
}

std::string_view
develop_mask_attachment_status_name(const DevelopMaskAttachmentStatus status) noexcept
{
    switch (status)
    {
    case DevelopMaskAttachmentStatus::kNoMask:
        return "no_mask";
    case DevelopMaskAttachmentStatus::kEditable:
        return "editable";
    case DevelopMaskAttachmentStatus::kExternalReadOnly:
        return "external_read_only";
    case DevelopMaskAttachmentStatus::kSharedReadOnly:
        return "shared_read_only";
    case DevelopMaskAttachmentStatus::kGroupReadOnly:
        return "group_read_only";
    case DevelopMaskAttachmentStatus::kInvalid:
        return "invalid";
    }
    return "invalid";
}

bool is_develop_mask_field(const std::string_view field) noexcept
{
    return field.starts_with(kColorHarmonizerMaskFieldPrefix) ||
           field.starts_with(kGraduatedMaskFieldPrefix);
}

DevelopMaskEditorState develop_mask_editor_state(const DevelopParams &params,
                                                 const DevelopMaskTarget target)
{
    DevelopMaskEditorState result;
    const auto &attachment = mask_attachment(params, target);
    if (!validate_mask_graph(params.masks) || !develop_mask_attachments_resolve(params))
    {
        result.attached = attachment.has_value();
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kInvalid;
        return result;
    }
    if (!attachment)
    {
        return result;
    }

    result.attached = true;
    result.can_detach = true;
    const auto *mask = find_mask(params.masks, *attachment);
    if (mask == nullptr)
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kInvalid;
        return result;
    }

    result.kind_index = mask_kind_index(mask->kind);
    result.kind_name = std::string(mask_kind_name(mask->kind));
    result.opacity = mask->common.opacity;
    result.inverted = mask->common.inverted;
    if (const auto *gradient = std::get_if<LinearGradientMask>(&mask->payload); gradient != nullptr)
    {
        result.anchor_x = gradient->anchor_x;
        result.anchor_y = gradient->anchor_y;
        result.rotation_degrees = gradient->rotation_degrees;
        result.transition = gradient->transition;
    }
    else if (const auto *circle = std::get_if<CircleMask>(&mask->payload); circle != nullptr)
    {
        result.center_x = circle->center_x;
        result.center_y = circle->center_y;
        result.radius = circle->radius;
        result.feather = circle->feather;
    }
    else if (const auto *ellipse = std::get_if<EllipseMask>(&mask->payload); ellipse != nullptr)
    {
        result.center_x = ellipse->center_x;
        result.center_y = ellipse->center_y;
        result.radius_x = ellipse->radius_x;
        result.radius_y = ellipse->radius_y;
        result.rotation_degrees = ellipse->rotation_degrees;
        result.feather = ellipse->feather;
    }
    else if (const auto *parametric = std::get_if<ParametricMask>(&mask->payload);
             parametric != nullptr)
    {
        result.source_index = parametric->source == ParametricMaskSource::kInput ? 0 : 1;
        result.channel_index = static_cast<std::int64_t>(parametric->channel);
        result.threshold0 = parametric->thresholds[0];
        result.threshold1 = parametric->thresholds[1];
        result.threshold2 = parametric->thresholds[2];
        result.threshold3 = parametric->thresholds[3];
    }

    if (mask->kind == MaskKind::kGroup)
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kGroupReadOnly;
    }
    else if (!studio_owns_mask_id(target, mask->id))
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kExternalReadOnly;
    }
    else if (is_attached_by_other_operation(params, target, mask->id) ||
             has_group_edge_to(params, mask->id))
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kSharedReadOnly;
    }
    else
    {
        result.status = DevelopMaskAttachmentStatus::kEditable;
    }
    return result;
}

Result<void> apply_develop_mask_field_strict(DevelopParams &params, const std::string_view field,
                                             const double value)
{
    const auto failed_target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                   DevelopMaskTarget::kGraduatedNd :
                                   DevelopMaskTarget::kColorHarmonizer;
    try
    {
        const auto parsed = parse_mask_field(field);
        if (!parsed)
        {
            const auto target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                    DevelopMaskTarget::kGraduatedNd :
                                    DevelopMaskTarget::kColorHarmonizer;
            return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                                   "unknown_develop_mask_field", target, field);
        }

        DevelopParams candidate = params;
        auto valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        if (parsed->field == MaskField::kWhole)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument,
                                   "The whole Develop mask field accepts reset/detach only",
                                   "invalid_develop_mask_field", parsed->target, field);
        }
        if (parsed->field == MaskField::kKind)
        {
            auto index =
                exact_index(value, 0, 5, parsed->target, field, "invalid_develop_mask_kind");
            if (!index)
            {
                return index.error();
            }
            if (index.value() == 0)
            {
                auto detached = detach_mask(candidate, parsed->target, field);
                if (!detached)
                    return detached.error();
                params = std::move(candidate);
                return {};
            }
            const auto kind = mask_kind_from_index(index.value());
            if (!kind)
            {
                return mask_edit_error(ErrorCode::kInvalidArgument,
                                       "Develop mask kind is unsupported for Studio authoring",
                                       "invalid_develop_mask_kind", parsed->target, field);
            }
            auto &attachment = mask_attachment(candidate, parsed->target);
            if (!attachment)
            {
                auto id = next_studio_mask_id(candidate, parsed->target, field);
                if (!id)
                    return id.error();
                candidate.masks.emplace_back(std::move(id).value(), kCanonicalMaskSchemaVersion,
                                             kind.value());
                attachment = candidate.masks.back().id;
                enable_target_operation(candidate, parsed->target);
            }
            else
            {
                auto editable = require_editable(candidate, parsed->target, field);
                if (!editable)
                    return editable.error();
                auto *mask = find_mask(candidate.masks, *attachment);
                if (mask == nullptr)
                {
                    return mask_edit_error(
                        ErrorCode::kValidation,
                        "Develop mask attachment does not resolve to a canonical node",
                        "missing_develop_mask_attachment", parsed->target, field);
                }
                if (mask->kind != kind.value())
                {
                    Mask replacement{mask->id, kCanonicalMaskSchemaVersion, kind.value()};
                    replacement.common = mask->common;
                    *mask = std::move(replacement);
                    enable_target_operation(candidate, parsed->target);
                }
            }
        }
        else
        {
            auto editable = require_editable(candidate, parsed->target, field);
            if (!editable)
                return editable.error();
            const auto &attachment = mask_attachment(candidate, parsed->target);
            auto *mask = find_mask(candidate.masks, *attachment);
            if (mask == nullptr)
            {
                return mask_edit_error(
                    ErrorCode::kValidation,
                    "Develop mask attachment does not resolve to a canonical node",
                    "missing_develop_mask_attachment", parsed->target, field);
            }
            auto applied = apply_mask_value(*mask, parsed->field, value, parsed->target, field);
            if (!applied)
                return applied.error();
        }
        valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        params = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return mask_edit_error(ErrorCode::kIo,
                               "Develop mask edit could not allocate its typed candidate",
                               "develop_mask_allocation_failed", failed_target, field);
    }
}

Result<void> reset_develop_mask_field(DevelopParams &params, const std::string_view field)
{
    const auto failed_target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                   DevelopMaskTarget::kGraduatedNd :
                                   DevelopMaskTarget::kColorHarmonizer;
    try
    {
        const auto parsed = parse_mask_field(field);
        if (!parsed)
        {
            const auto target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                    DevelopMaskTarget::kGraduatedNd :
                                    DevelopMaskTarget::kColorHarmonizer;
            return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                                   "unknown_develop_mask_field", target, field);
        }

        DevelopParams candidate = params;
        if (parsed->field == MaskField::kWhole)
        {
            auto detached = detach_mask(candidate, parsed->target, field);
            if (!detached)
                return detached.error();
            params = std::move(candidate);
            return {};
        }
        auto valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        auto editable = require_editable(candidate, parsed->target, field);
        if (!editable)
            return editable.error();
        const auto &attachment = mask_attachment(candidate, parsed->target);
        auto *mask = find_mask(candidate.masks, *attachment);
        if (mask == nullptr)
        {
            return mask_edit_error(ErrorCode::kValidation,
                                   "Develop mask attachment does not resolve to a canonical node",
                                   "missing_develop_mask_attachment", parsed->target, field);
        }
        auto reset = reset_mask_value(*mask, parsed->field, parsed->target, field);
        if (!reset)
            return reset.error();
        valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
            return valid.error();
        params = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return mask_edit_error(ErrorCode::kIo,
                               "Develop mask reset could not allocate its typed candidate",
                               "develop_mask_allocation_failed", failed_target, field);
    }
}

} // namespace ravo
