/**
 * @file color.h
 * @brief Packed RGBA8 color helpers for the .pix decode/composite pipeline.
 *
 * Pure CPU, no Vulkan. The packed byte order (r in the low byte) intentionally
 * matches coopa::ui::UiVertex::pack_color() / VK_FORMAT_R8G8B8A8_UNORM, so a
 * packed value here can be written straight into an atlas pixel buffer and
 * uploaded without any channel reordering.
 */

#ifndef PIXENGINE_PIX_COLOR_H
#define PIXENGINE_PIX_COLOR_H

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace coopa {
namespace pix {

/// @brief Packs four [0,255] channels into R8G8B8A8_UNORM byte order (r in the low byte).
inline uint32_t pack_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

inline uint8_t rgba8_r(uint32_t c) { return static_cast<uint8_t>(c & 0xFFu); }
inline uint8_t rgba8_g(uint32_t c) { return static_cast<uint8_t>((c >> 8) & 0xFFu); }
inline uint8_t rgba8_b(uint32_t c) { return static_cast<uint8_t>((c >> 16) & 0xFFu); }
inline uint8_t rgba8_a(uint32_t c) { return static_cast<uint8_t>((c >> 24) & 0xFFu); }

namespace detail {

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline uint8_t hex_byte(const char* p) {
    int hi = hex_nibble(p[0]);
    int lo = hex_nibble(p[1]);
    if (hi < 0 || lo < 0) {
        throw std::invalid_argument("parse_hex_color: invalid hex digit");
    }
    return static_cast<uint8_t>((hi << 4) | lo);
}

} // namespace detail

/**
 * @brief Parses a "#RRGGBBAA" string (coopixel's on-disk pixel color format)
 *        into a packed RGBA8 value. Accepts upper or lower case hex.
 * @throws std::invalid_argument if the string isn't exactly '#' + 8 hex digits.
 */
inline uint32_t parse_hex_color(const std::string& s) {
    if (s.size() != 9 || s[0] != '#') {
        throw std::invalid_argument("parse_hex_color: expected '#RRGGBBAA', got '" + s + "'");
    }
    uint8_t r = detail::hex_byte(&s[1]);
    uint8_t g = detail::hex_byte(&s[3]);
    uint8_t b = detail::hex_byte(&s[5]);
    uint8_t a = detail::hex_byte(&s[7]);
    return pack_rgba8(r, g, b, a);
}

/**
 * @brief Straight-alpha "source-over" compositing of src over dst, both packed RGBA8.
 *
 * Standard Porter-Duff over: out_a = src_a + dst_a*(1-src_a); out_rgb is the
 * associated weighted average, converted back to straight alpha. Matches
 * coopixel's QPainter-based compositing closely enough for pixel art (no
 * gamma correction — coopixel doesn't do any either).
 */
inline uint32_t source_over(uint32_t src, uint32_t dst) {
    float sa = rgba8_a(src) / 255.0f;
    float da = rgba8_a(dst) / 255.0f;
    float oa = sa + da * (1.0f - sa);

    if (oa <= 0.0f) {
        return pack_rgba8(0, 0, 0, 0);
    }

    auto blend_channel = [&](uint8_t sc, uint8_t dc) -> uint8_t {
        float sc_f = sc / 255.0f;
        float dc_f = dc / 255.0f;
        float oc = (sc_f * sa + dc_f * da * (1.0f - sa)) / oa;
        return static_cast<uint8_t>(std::clamp(oc, 0.0f, 1.0f) * 255.0f + 0.5f);
    };

    uint8_t r = blend_channel(rgba8_r(src), rgba8_r(dst));
    uint8_t g = blend_channel(rgba8_g(src), rgba8_g(dst));
    uint8_t b = blend_channel(rgba8_b(src), rgba8_b(dst));
    uint8_t a = static_cast<uint8_t>(std::clamp(oa, 0.0f, 1.0f) * 255.0f + 0.5f);
    return pack_rgba8(r, g, b, a);
}

/**
 * @brief Multiplies a layer opacity [0,1] into a straight-alpha packed color's alpha channel.
 */
inline uint32_t scale_alpha(uint32_t c, float opacity) {
    float a = rgba8_a(c) / 255.0f * std::clamp(opacity, 0.0f, 1.0f);
    return pack_rgba8(rgba8_r(c), rgba8_g(c), rgba8_b(c), static_cast<uint8_t>(a * 255.0f + 0.5f));
}

/**
 * @brief Converts a straight-alpha packed RGBA8 color to premultiplied alpha.
 *
 * The sprite atlas is stored premultiplied throughout (see pix_composite.h) —
 * it's the only representation where a partially-transparent atlas texel has
 * a meaningful color at frame edges and where the stroke effect blends cleanly.
 */
inline uint32_t premultiply(uint32_t straight) {
    float a = rgba8_a(straight) / 255.0f;
    auto mul = [&](uint8_t c) { return static_cast<uint8_t>(std::clamp(c * a, 0.0f, 255.0f) + 0.5f); };
    return pack_rgba8(mul(rgba8_r(straight)), mul(rgba8_g(straight)), mul(rgba8_b(straight)), rgba8_a(straight));
}

} // namespace pix
} // namespace coopa

#endif // PIXENGINE_PIX_COLOR_H
