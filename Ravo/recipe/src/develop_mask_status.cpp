#include "ravo/recipe/develop_mask.h"

namespace ravo
{
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

bool develop_mask_parametric_assist_allowed(const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kLocal:
    case DevelopMaskTarget::kColorBalanceRgb:
    case DevelopMaskTarget::kExposure:
    case DevelopMaskTarget::kRgbCurve:
    case DevelopMaskTarget::kToneCurve:
    case DevelopMaskTarget::kHighlights:
    case DevelopMaskTarget::kShadows:
    case DevelopMaskTarget::kWhites:
    case DevelopMaskTarget::kBlacks:
        return true;
    case DevelopMaskTarget::kColorHarmonizer:
    case DevelopMaskTarget::kGraduatedNd:
        return false;
    }
    return false;
}
} // namespace ravo
