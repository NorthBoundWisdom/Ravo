#include "export_delivery_watermark.h"

namespace ravo
{

Result<WatermarkParams> watermark_params_from_export_options(const ExportWatermarkOptions &options)
{
    auto valid = validate_export_watermark_options(options);
    if (!valid)
        return valid.error();
    auto alignment = parse_watermark_alignment(options.alignment);
    if (!alignment)
        return alignment.error();
    WatermarkParams params;
    params.text = options.text;
    params.color = options.color;
    params.opacity = options.opacity;
    params.scale_percent = options.scale_percent;
    params.x_offset = options.x_offset;
    params.y_offset = options.y_offset;
    params.alignment = alignment.value();
    params.rotation_degrees = options.rotation_degrees;
    auto canonical = watermark_to_parameters(params);
    if (!canonical)
        return canonical.error();
    return params;
}

Result<RenderedExportImage> apply_export_delivery_watermark(RenderedExportImage image,
                                                            const ExportWatermarkOptions &options,
                                                            const AssetDescriptor &asset,
                                                            const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (!options.enabled)
        return image;
    auto params = watermark_params_from_export_options(options);
    if (!params)
        return params.error();
    return apply_watermark_to_export_image(std::move(image), params.value(), asset, cancellation);
}

} // namespace ravo
