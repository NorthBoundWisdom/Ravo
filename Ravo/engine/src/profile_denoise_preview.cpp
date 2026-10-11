#include "profile_denoise_preview.h"

#include <limits>
#include <new>
#include <utility>

#include "capability_ops.h"

namespace ravo::detail
{
bool ProfileDenoisePreviewCache::admits(const WorkingImage &input) noexcept
{
    const auto pixels = static_cast<std::uint64_t>(input.width) * input.height;
    return input.width != 0U && input.height != 0U &&
           pixels <= std::numeric_limits<std::size_t>::max() / 3U &&
           pixels * 3U == input.rgb.size() &&
           input.rgb.capacity() <= kPixelByteLimit / (2U * sizeof(float)) &&
           input.color_profile.icc_bytes.capacity() <= 1024U * 1024U &&
           input.color_profile.identifier.capacity() <= 256U;
}

bool ProfileDenoisePreviewCache::matches(const std::string_view key) const noexcept
{
    return entry_ != nullptr && entry_->key == key;
}

std::uint64_t ProfileDenoisePreviewCache::pixel_bytes() const noexcept
{
    return entry_ == nullptr ? 0U :
                               (static_cast<std::uint64_t>(entry_->input.rgb.capacity()) +
                                entry_->candidate.capacity()) *
                                   sizeof(float);
}

Result<WorkingImage> ProfileDenoisePreviewCache::prepare(WorkingImage input,
                                                         const OperationInstance &operation,
                                                         std::string key,
                                                         const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    clear();
    if (operation.id != "ravo.detail.denoiseprofile" || operation.schema_version != 1 ||
        !operation.enabled || operation.bypass || operation.mask_id || !operation.children.empty())
        return make_error(ErrorCode::kValidation, "Operation cannot own a denoise preview basis",
                          {{"reason", "unsupported_denoise_preview_basis_operation"}});
    if (!admits(input) || key.capacity() > 1024U * 1024U)
    {
        auto applied = apply_denoise_profile(input, operation, cancellation);
        if (!applied)
            return applied.error();
        return input;
    }
    WorkingImage result = input;
    std::vector<float> candidate;
    auto applied = apply_denoise_profile(result, operation, cancellation, &candidate);
    if (!applied)
        return applied.error();
    active = cancellation.check();
    if (!active)
        return active.error();
    if (!candidate.empty() &&
        candidate.capacity() <= kPixelByteLimit / sizeof(float) - input.rgb.capacity())
    {
        entry_ =
            std::make_unique<Entry>(Entry{std::move(key), std::move(input), std::move(candidate)});
        ++generation_;
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Unable to allocate a denoise preview basis",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> ProfileDenoisePreviewCache::mix(const OperationInstance &operation,
                                                     const CancellationToken &cancellation) const
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (entry_ == nullptr)
        return make_error(ErrorCode::kConflict, "Denoise preview basis is unavailable",
                          {{"reason", "denoise_preview_basis_unavailable"}});
    WorkingImage output = entry_->input;
    auto mixed = mix_denoise_profile_candidate(output, entry_->candidate, operation, cancellation);
    if (!mixed)
        return mixed.error();
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Unable to allocate denoise preview mixing pixels",
                      {{"reason", "allocation_failed"}});
}
} // namespace ravo::detail
