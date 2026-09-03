#include "export_delivery_frame.h"

namespace ravo
{

Result<FrameParams> frame_params_from_export_options(const ExportFrameOptions &options)
{
    auto valid = validate_export_frame_options(options);
    if (!valid)
        return valid.error();
    auto orientation = parse_frame_orientation(options.orientation);
    if (!orientation)
        return orientation.error();
    auto basis = parse_frame_basis(options.basis);
    if (!basis)
        return basis.error();
    FrameParams params;
    params.border_color = options.border_color;
    params.aspect = options.aspect;
    params.orientation = orientation.value();
    params.size = options.size;
    params.position_h = options.position_h;
    params.position_v = options.position_v;
    params.frame_size = options.frame_size;
    params.frame_offset = options.frame_offset;
    params.frame_color = options.frame_color;
    params.basis = basis.value();
    auto canonical = frame_to_parameters(params);
    if (!canonical)
        return canonical.error();
    return params;
}

Result<RenderedExportImage> apply_export_delivery_frame(RenderedExportImage image,
                                                        const ExportFrameOptions &options,
                                                        const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (!options.enabled)
        return image;
    auto params = frame_params_from_export_options(options);
    if (!params)
        return params.error();
    return apply_frame_to_export_image(std::move(image), params.value(), cancellation);
}

} // namespace ravo
