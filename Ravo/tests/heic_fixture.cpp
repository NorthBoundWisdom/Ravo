#include "heic_fixture.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <array>
#include <memory>
#include <type_traits>

template <class T>
using FixtureOwner = std::unique_ptr<std::remove_pointer_t<T>, decltype(&CFRelease)>;
#endif

QByteArray make_heic_fixture(int orientation, bool display_p3)
{
#if defined(__APPLE__)
    constexpr int width = 96, height = 64;
    std::array<unsigned char, width * height * 4> pixels{};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const auto index = static_cast<std::size_t>((y * width + x) * 4);
            pixels[index] = (y < height / 2 ? x < width / 2 : x >= width / 2) ? 230 : 20;
            pixels[index + 1] = x >= width / 2 ? 220 : 20;
            pixels[index + 2] = y >= height / 2 && x < width / 2 ? 230 : 20;
            pixels[index + 3] = 255;
        }
    FixtureOwner<CGColorSpaceRef> colour(
        CGColorSpaceCreateWithName(display_p3 ? kCGColorSpaceDisplayP3 : kCGColorSpaceSRGB),
        &CFRelease);
    FixtureOwner<CGDataProviderRef> provider(
        CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr), &CFRelease);
    FixtureOwner<CGImageRef> image(
        CGImageCreate(width, height, 8, 32, width * 4, colour.get(),
                      static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast) |
                          kCGBitmapByteOrder32Big,
                      provider.get(), nullptr, false, kCGRenderingIntentDefault),
        &CFRelease);
    FixtureOwner<CFMutableDataRef> bytes(CFDataCreateMutable(nullptr, 0), &CFRelease);
    FixtureOwner<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithData(bytes.get(), CFSTR("public.heic"), 1, nullptr),
        &CFRelease);
    if (!image || !destination)
        return {};
    FixtureOwner<CFNumberRef> orient(CFNumberCreate(nullptr, kCFNumberIntType, &orientation),
                                     &CFRelease);
    const double quality = 1.0;
    FixtureOwner<CFNumberRef> quality_value(CFNumberCreate(nullptr, kCFNumberDoubleType, &quality),
                                            &CFRelease);
    const void *keys[] = {kCGImagePropertyOrientation, kCGImageDestinationLossyCompressionQuality};
    const void *values[] = {orient.get(), quality_value.get()};
    FixtureOwner<CFDictionaryRef> properties(CFDictionaryCreate(nullptr, keys, values, 2,
                                                                &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks),
                                             &CFRelease);
    CGImageDestinationAddImage(destination.get(), image.get(), properties.get());
    if (!CGImageDestinationFinalize(destination.get()))
        return {};
    return QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(bytes.get())),
                      CFDataGetLength(bytes.get()));
#else
    static_cast<void>(orientation);
    static_cast<void>(display_p3);
    return {};
#endif
}
