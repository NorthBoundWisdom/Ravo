#include "studio_preview_handoff.h"

#include <algorithm>
#include <new>

#include <QColorSpace>

#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#endif

namespace ravo
{
Result<void> own_preview_pixels_for_handoff(PreviewResult &preview,
                                            const CancellationToken &cancellation)
try
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (preview.gpu_display_generation == 0U || !preview.rgb.empty())
    {
        preview.gpu_display_native_surface = 0U;
        return {};
    }
    if (preview.width != preview.gpu_display_width || preview.height != preview.gpu_display_height)
        return make_error(ErrorCode::kValidation, "GPU preview handoff dimensions do not match",
                          {{"reason", "invalid_gpu_preview_handoff"}});
#if defined(Q_OS_MACOS)
    auto snapshot = studio_metal::snapshot_iosurface_rgb8(preview.gpu_display_native_surface,
                                                          preview.width, preview.height);
    if (!snapshot)
        return snapshot.error();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(preview.width) * preview.height * 3U);
    for (std::uint32_t y = 0; y < preview.height; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        std::copy_n(snapshot.value().constScanLine(static_cast<int>(y)), preview.width * 3U,
                    pixels.data() + static_cast<std::size_t>(y) * preview.width * 3U);
    }
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = "srgb";
    const auto icc = snapshot.value().colorSpace().iccProfile();
    const auto *begin = reinterpret_cast<const std::uint8_t *>(icc.constData());
    profile.icc_bytes.assign(begin, begin + icc.size());
    if (auto active = cancellation.check(); !active)
        return active.error();
    preview.rgb = std::move(pixels);
    preview.color_profile = std::move(profile);
    preview.gpu_display_native_surface = 0U;
    return {};
#else
    return make_error(ErrorCode::kUnsupported,
                      "GPU preview handoff is unavailable on this platform",
                      {{"reason", "gpu_display_snapshot_unsupported"}});
#endif
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "GPU preview handoff allocation failed",
                      {{"reason", "allocation_failed"}});
}
} // namespace ravo
