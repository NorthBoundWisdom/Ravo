#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class RasterDecoder
{
public:
    virtual ~RasterDecoder() = default;

    [[nodiscard]] virtual Result<RasterInfo> probe(std::string_view path) const = 0;
    [[nodiscard]] virtual Result<EncodedPng>
    decode(std::string_view path, std::uint32_t max_edge,
           const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<EncodedPng>
    decode_memory(const std::vector<std::uint8_t> &encoded, std::uint32_t max_edge,
                  const CancellationToken &cancellation) const = 0;
};

} // namespace ravo
