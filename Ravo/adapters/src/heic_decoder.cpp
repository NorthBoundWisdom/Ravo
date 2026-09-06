#include "qt_raster_internal.h"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <QFile>
#include <QFileInfo>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace ravo::qt_raster_internal
{
namespace
{
constexpr qint64 kMaximumEncodedBytes = 256LL * 1024LL * 1024LL;

TaskError heic_error(ErrorCode code, std::string message, std::string_view reason,
                     std::string_view source)
{
    return make_error(code, std::move(message),
                      {{"format", "heic"},
                       {"reason", std::string(reason)},
                       {"source", std::string(source)},
                       {"decoder", "ravo_imageio_heic/v1"}});
}

#if defined(__APPLE__)
constexpr std::uint64_t kMaximumPixels = 64'000'000;

Result<QByteArray> read_heic_bytes(std::string_view path, const CancellationToken &cancellation)
{
    QFile file(qstring_from_utf8(path));
    if (!file.open(QIODevice::ReadOnly))
        return heic_error(ErrorCode::kIo, "Unable to open HEIC/HEIF input", "heic_open_failed",
                          path);
    if (file.size() <= 0 || file.size() > kMaximumEncodedBytes)
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF input exceeds the encoded size limit",
                          "heic_input_size_limit", path);
    QByteArray bytes;
    bytes.reserve(file.size());
    while (!file.atEnd())
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        const auto chunk = file.read(256 * 1024);
        if (chunk.isEmpty() || chunk.size() > kMaximumEncodedBytes - bytes.size())
            return heic_error(ErrorCode::kIo, "Unable to read complete HEIC/HEIF input",
                              "heic_read_failed", path);
        bytes.append(chunk);
    }
    return bytes;
}

template <class T>
using CfOwner = std::unique_ptr<std::remove_pointer_t<T>, decltype(&CFRelease)>;

std::int64_t number(CFDictionaryRef properties, CFStringRef key, std::int64_t absent = 0)
{
    const auto value = CFDictionaryGetValue(properties, key);
    if (!value)
        return absent;
    std::int64_t result = 0;
    return CFGetTypeID(value) == CFNumberGetTypeID() &&
                   CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result) ?
               result :
               -1;
}

bool flag(CFDictionaryRef properties, CFStringRef key, bool absent = false)
{
    const auto value = CFDictionaryGetValue(properties, key);
    if (!value)
        return absent;
    return CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

[[nodiscard]] std::uint64_t read_u64_be(const std::span<const std::uint8_t> bytes) noexcept
{
    return (static_cast<std::uint64_t>(read_u32_be(bytes.subspan(0U, 4U))) << 32U) |
           static_cast<std::uint64_t>(read_u32_be(bytes.subspan(4U, 4U)));
}

// ImageIO on virtualized CI hosts can still emit a thumbnail from a truncated
// HEIC. Reject containers whose top-level boxes do not fit the supplied bytes.
[[nodiscard]] bool
heic_top_level_boxes_are_complete(const std::span<const std::uint8_t> bytes) noexcept
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 8U)
            return false;
        std::uint64_t box_size = read_u32_be(bytes.subspan(offset, 4U));
        std::size_t header = 8U;
        if (box_size == 1U)
        {
            if (bytes.size() - offset < 16U)
                return false;
            box_size = read_u64_be(bytes.subspan(offset + 8U, 8U));
            header = 16U;
            if (box_size < header)
                return false;
        }
        else if (box_size == 0U)
            return true;
        else if (box_size < header)
            return false;
        if (box_size > static_cast<std::uint64_t>(bytes.size() - offset))
            return false;
        offset += static_cast<std::size_t>(box_size);
    }
    return offset == bytes.size();
}

Result<DecodedRaster> decode_native(std::span<const std::uint8_t> bytes, std::uint32_t max_edge,
                                    const CancellationToken &cancellation, std::string_view source,
                                    int rotate_quarters)
{
    CFStringRef decode_request;
    CFStringRef decode_to_sdr;
    if (__builtin_available(macOS 14.0, *))
    {
        decode_request = kCGImageSourceDecodeRequest;
        decode_to_sdr = kCGImageSourceDecodeToSDR;
    }
    else
        return heic_unsupported_error(source);
    const auto invalid = [&]()
    {
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF image is incomplete or corrupt",
                          "invalid_heic_input", source);
    };
    if (!heic_top_level_boxes_are_complete(bytes))
        return invalid();
    // CFData borrows the immutable call input. Native objects are destroyed
    // before returning, and only the copied raster crosses this boundary.
    CfOwner<CFDataRef> data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes.data(),
                                                        static_cast<CFIndex>(bytes.size()),
                                                        kCFAllocatorNull),
                            &CFRelease);
    if (!data)
        return heic_error(ErrorCode::kIo, "Unable to allocate HEIC input owner",
                          "heic_allocation_failed", source);
    CfOwner<CGImageSourceRef> image_source(CGImageSourceCreateWithData(data.get(), nullptr),
                                           &CFRelease);
    if (!image_source || CGImageSourceGetStatus(image_source.get()) != kCGImageStatusComplete ||
        CGImageSourceGetCount(image_source.get()) == 0)
        return invalid();
    const auto type = CGImageSourceGetType(image_source.get());
    if (!type || (!CFEqual(type, CFSTR("public.heic")) && !CFEqual(type, CFSTR("public.heif")) &&
                  !CFEqual(type, CFSTR("public.heics")) && !CFEqual(type, CFSTR("public.heifs"))))
        return invalid();
    const auto primary = CGImageSourceGetPrimaryImageIndex(image_source.get());
    if (primary >= CGImageSourceGetCount(image_source.get()))
        return invalid();
    CfOwner<CFDictionaryRef> properties(
        CGImageSourceCopyPropertiesAtIndex(image_source.get(), primary, nullptr), &CFRelease);
    if (!properties)
        return invalid();
    const auto width = number(properties.get(), kCGImagePropertyPixelWidth);
    const auto height = number(properties.get(), kCGImagePropertyPixelHeight);
    const auto orientation = number(properties.get(), kCGImagePropertyOrientation, 1);
    if (width <= 0 || height <= 0 || orientation < 1 || orientation > 8)
        return invalid();
    const auto declares_alpha = flag(properties.get(), kCGImagePropertyHasAlpha);
    if (width > 32768 || height > 32768 ||
        static_cast<std::uint64_t>(width * height) > kMaximumPixels)
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF dimensions exceed the decode limit",
                          "heic_pixel_limit", source);
    QSize original(static_cast<int>(width), static_cast<int>(height));
    if (orientation >= 5)
        original.transpose();
    const int edge =
        static_cast<int>(max_edge == 0 ? std::max(width, height) :
                                         std::min<std::int64_t>(max_edge, std::max(width, height)));
    CfOwner<CFNumberRef> limit(CFNumberCreate(nullptr, kCFNumberIntType, &edge), &CFRelease);
    if (!limit)
        return heic_error(ErrorCode::kIo, "Unable to allocate HEIC decode options",
                          "heic_allocation_failed", source);
    const void *keys[] = {kCGImageSourceCreateThumbnailFromImageAlways,
                          kCGImageSourceCreateThumbnailWithTransform,
                          kCGImageSourceThumbnailMaxPixelSize,
                          kCGImageSourceShouldCacheImmediately,
                          kCGImageSourceShouldAllowFloat,
                          decode_request};
    const void *values[] = {kCFBooleanTrue, kCFBooleanTrue,  limit.get(),
                            kCFBooleanTrue, kCFBooleanFalse, decode_to_sdr};
    CfOwner<CFDictionaryRef> options(CFDictionaryCreate(nullptr, keys, values, 6,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks),
                                     &CFRelease);
    if (!options)
        return heic_error(ErrorCode::kIo, "Unable to allocate HEIC decode options",
                          "heic_allocation_failed", source);
    if (auto active = cancellation.check(); !active)
        return active.error();
    CfOwner<CGImageRef> image(
        CGImageSourceCreateThumbnailAtIndex(image_source.get(), primary, options.get()),
        &CFRelease);
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!image ||
        CGImageSourceGetStatusAtIndex(image_source.get(), primary) != kCGImageStatusComplete)
        return invalid();
    if (!CGImageGetColorSpace(image.get()))
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF image has no colour space",
                          "heic_colour_space_missing", source);
    const auto decoded_width = CGImageGetWidth(image.get());
    const auto decoded_height = CGImageGetHeight(image.get());
    if (decoded_width == 0 || decoded_height == 0 || decoded_width > 32768 ||
        decoded_height > 32768 || decoded_width * decoded_height > kMaximumPixels)
        return invalid();
    QImage output(static_cast<int>(decoded_width), static_cast<int>(decoded_height),
                  QImage::Format_RGBA8888);
    if (output.isNull())
        return heic_error(ErrorCode::kIo, "Unable to allocate HEIC output pixels",
                          "heic_allocation_failed", source);
    CfOwner<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB), &CFRelease);
    if (!srgb)
        return heic_error(ErrorCode::kIo, "Unable to create HEIC output colour space",
                          "heic_colour_context_failed", source);
    CfOwner<CGContextRef> context(
        CGBitmapContextCreate(output.bits(), decoded_width, decoded_height, 8,
                              static_cast<std::size_t>(output.bytesPerLine()), srgb.get(),
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
                                  kCGBitmapByteOrder32Big),
        &CFRelease);
    if (!context)
        return heic_error(ErrorCode::kIo, "Unable to create HEIC colour conversion context",
                          "heic_colour_context_failed", source);
    CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
    CGContextDrawImage(
        context.get(),
        CGRectMake(0, 0, static_cast<CGFloat>(decoded_width), static_cast<CGFloat>(decoded_height)),
        image.get());
    // ImageIO may allocate an alpha channel for an opaque photograph. Check
    // actual coverage before stripping the channel; never flatten transparency.
    for (int row = 0; row < output.height(); ++row)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        const auto *pixels = output.constScanLine(row);
        for (int column = 0; column < output.width(); ++column)
            if (pixels[column * 4 + 3] != 255)
                return declares_alpha ? heic_error(ErrorCode::kUnsupported,
                                                   "HEIC/HEIF transparent images are not supported",
                                                   "heic_alpha_unsupported", source) :
                                        invalid();
    }
    output.setColorSpace(QColorSpace(QColorSpace::SRgb));
    output = apply_display_rotation(std::move(output), rotate_quarters);
    return decode_raster(
        std::move(output), max_edge, cancellation, source,
        ColorProfileState{ColorProfileKind::kBuiltin, ColorModel::kRgb, "srgb", {}},
        apply_display_rotation_to_size(original, rotate_quarters));
}
#endif
} // namespace

Result<DecodedRaster> decode_heic_bytes(std::span<const std::uint8_t> bytes, std::uint32_t max_edge,
                                        const CancellationToken &cancellation,
                                        std::string_view source, int rotate_quarters)
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (bytes.size() > kMaximumEncodedBytes)
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF input exceeds the encoded size limit",
                          "heic_input_size_limit", source);
    if (!is_heic_heif_payload(bytes))
        return heic_error(ErrorCode::kValidation, "Input is not a HEIC/HEIF container",
                          "invalid_heic_input", source);
#if defined(__APPLE__)
    return decode_native(bytes, max_edge, cancellation, source, rotate_quarters);
#else
    static_cast<void>(max_edge);
    static_cast<void>(rotate_quarters);
    return heic_unsupported_error(source);
#endif
}

Result<DecodedRaster> decode_heic_file(std::string_view path, std::uint32_t max_edge,
                                       const CancellationToken &cancellation)
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (QFileInfo(qstring_from_utf8(path)).size() > kMaximumEncodedBytes)
        return heic_error(ErrorCode::kValidation, "HEIC/HEIF input exceeds the encoded size limit",
                          "heic_input_size_limit", path);
#if !defined(__APPLE__)
    static_cast<void>(max_edge);
    return heic_unsupported_error(path);
#else
    auto bytes = read_heic_bytes(path, cancellation);
    if (!bytes)
        return bytes.error();
    return decode_heic_bytes(byte_span(bytes.value()), max_edge, cancellation, path, 0);
#endif
}

Result<RasterInfo> probe_heic_file(std::string_view path)
{
    auto image = decode_heic_file(path, 32, {});
    if (!image)
        return image.error();
    return RasterInfo{"image/heic", image.value().source_width, image.value().source_height};
}
} // namespace ravo::qt_raster_internal
