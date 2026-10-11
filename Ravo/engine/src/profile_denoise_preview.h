#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "image_ops.h"

namespace ravo::detail
{
// Private, synchronous child of one InteractivePreviewRenderCache. The caller
// binds keys to the immutable source generation and all non-mix dependencies.
class ProfileDenoisePreviewCache final
{
public:
    static constexpr std::uint64_t kPixelByteLimit = 64U * 1024U * 1024U;
    [[nodiscard]] static bool admits(const WorkingImage &input) noexcept;
    [[nodiscard]] bool matches(std::string_view key) const noexcept;
    [[nodiscard]] std::uint64_t pixel_bytes() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_;
    }
    void clear() noexcept
    {
        entry_.reset();
    }
    [[nodiscard]] Result<WorkingImage> prepare(WorkingImage input,
                                               const OperationInstance &operation, std::string key,
                                               const CancellationToken &cancellation);
    [[nodiscard]] Result<WorkingImage> mix(const OperationInstance &operation,
                                           const CancellationToken &cancellation) const;

private:
    struct Entry
    {
        std::string key;
        WorkingImage input;
        std::vector<float> candidate;
    };
    std::unique_ptr<Entry> entry_;
    std::uint64_t generation_ = 0U;
};
} // namespace ravo::detail
