#include "ravo/foundation/color.h"

#include <bit>
#include <cstddef>
#include <string_view>

namespace ravo
{
namespace
{

void hash_byte(std::uint64_t &hash, const std::uint8_t value) noexcept
{
    hash ^= value;
    hash *= 1099511628211ULL;
}

void hash_text(std::uint64_t &hash, const std::string_view text) noexcept
{
    for (const char character : text)
    {
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    hash_byte(hash, 0U);
}

} // namespace

std::string color_profile_fingerprint(const ColorProfileState &profile)
{
    std::uint64_t hash = 14695981039346656037ULL;
    hash_byte(hash, static_cast<std::uint8_t>(profile.kind));
    hash_byte(hash, static_cast<std::uint8_t>(profile.model));
    hash_text(hash, profile.identifier);
    for (const auto byte : profile.icc_bytes)
    {
        hash_byte(hash, byte);
    }
    hash_byte(hash, profile.has_matrix ? 1U : 0U);
    hash_byte(hash, profile.camera_input ? 1U : 0U);
    if (profile.has_matrix)
    {
        for (const float value : profile.matrix_to_xyz_d50)
        {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (int shift = 24; shift >= 0; shift -= 8)
            {
                hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & 0xffU));
            }
        }
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index)
    {
        result[static_cast<std::size_t>(index)] = hex[hash & 0x0fU];
        hash >>= 4U;
    }
    return result;
}

} // namespace ravo
