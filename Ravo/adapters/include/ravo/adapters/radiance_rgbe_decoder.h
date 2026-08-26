#pragma once

#include <cstddef>
#include <cstdint>

#include "ravo/domain/hdr_decoder.h"

namespace ravo
{

struct RadianceRgbeDecodeLimits
{
    std::uint64_t max_encoded_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_decoded_bytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t max_header_bytes = 1024U * 1024U;
    std::size_t max_header_line_bytes = 127U;
};

enum class RadianceRgbeDecodeCheckpoint : std::uint8_t
{
    kHeader,
    kPixels,
    kBeforeFileRead,
};

struct RadianceRgbeCheckpointObserver
{
    using Callback = void (*)(void *context, RadianceRgbeDecodeCheckpoint checkpoint,
                              std::size_t progress) noexcept;

    Callback callback = nullptr;
    // Non-owning. The caller must keep context alive for the duration of each
    // synchronous decode call made through this decoder instance.
    void *context = nullptr;
};

class RadianceRgbeDecoder final : public HdrDecoder
{
public:
    explicit RadianceRgbeDecoder(RadianceRgbeDecodeLimits limits = {},
                                 RadianceRgbeCheckpointObserver checkpoint_observer = {});

    [[nodiscard]] Result<DecodedHdrRaster>
    decode(std::string_view path, const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<DecodedHdrRaster>
    decode_memory(const std::vector<std::uint8_t> &encoded,
                  const CancellationToken &cancellation) const override;

private:
    RadianceRgbeDecodeLimits limits_;
    RadianceRgbeCheckpointObserver checkpoint_observer_;
};

} // namespace ravo
