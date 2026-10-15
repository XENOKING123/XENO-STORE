// XENO STORE TV app - The brand: the logo badge, its turning rim and the wordmark.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/brand.hpp"

#include "core/save_file.hpp"
#include "ui/fonts.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::brand
{

namespace
{

constexpr float kTurn = 6.2831853f;
constexpr float kArcSweep = kTurn / 3.0f; // 120 degrees
constexpr int kArcSegments = 30;
constexpr float kTracking = 0.12f; // em, as the web's .brand-word
constexpr float kWordGap = 0.3f;   // em between the two words

// The rim's colour from its tail (0) to its head (1): the accent blue, then
// gold. It fades towards the page by colour rather than alpha, so the
// overlapping segments of an arc leave no seams.
ui::Color rim_colour(float t)
{
    const float gold = std::clamp((t - 0.35f) / 0.3f, 0.0f, 1.0f);
    const ui::Color hue = hui::gfx::mix(ui::color::accent, ui::color::gold, gold);
    return hui::gfx::mix(ui::color::night0, hue, 0.3f + 0.7f * t);
}

// The name split at its first space: "XENO STORE" -> "XENO", "STORE".
void split(std::string_view name, std::string_view *head, std::string_view *tail)
{
    const std::size_t space = name.find(' ');
    *head = name.substr(0, space);
    *tail = space == std::string_view::npos ? std::string_view{} : name.substr(space + 1);
}

} // namespace

image::Pixels badge(int size, const std::string &assets_dir)
{
    image::Pixels pixels;
    std::string png;
    if (size <= 0 || !hui::save::read_file(assets_dir + "/" + kBadgeFile, &png))
        return pixels;
    if (!image::decode(png, size, size, &pixels))
        return image::Pixels{};
    // decode() fits the image and never enlarges it; the badge is square, so
    // only a request above its 512 px needs the resize.
    if (pixels.width != size || pixels.height != size)
        pixels = image::resize(pixels, size, size);
    return pixels;
}

void draw_rim(hui::gfx::DrawList &list, float x, float y, float size, float phase, float opacity)
{
    if (opacity <= 0.01f || size <= 0.0f)
        return;
    const float cx = x + size * 0.5f;
    const float cy = y + size * 0.5f;
    const float thickness = std::max(2.0f, size * 0.035f);
    const float radius = size * 0.5f + std::max(3.0f, size * 0.05f) + thickness; // outer edge
    const float step = kArcSweep / static_cast<float>(kArcSegments);
    // Neighbouring segments overlap by a pixel, so no seam shows.
    const float overlap = 1.0f / radius;
    phase -= std::floor(phase);
    for (int arc = 0; arc < 2; ++arc)
    {
        const float head = phase * kTurn + static_cast<float>(arc) * kTurn * 0.5f;
        const float tail = head - kArcSweep;
        for (int k = 0; k < kArcSegments; ++k)
        {
            // t runs from the tail (0) to the head (1).
            const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(kArcSegments);
            const ui::Color ink = rim_colour(t).with_alpha(opacity);
            list.arc(cx, cy, radius, thickness, tail + static_cast<float>(k) * step, step + overlap,
                     ink, false);
        }
        // A round, lit head.
        const float hx = cx + std::sin(head) * (radius - thickness * 0.5f);
        const float hy = cy - std::cos(head) * (radius - thickness * 0.5f);
        list.glow({hx - thickness, hy - thickness, 2.0f * thickness, 2.0f * thickness}, thickness,
                  thickness * 3.0f, ui::color::gold.with_alpha(0.45f * opacity));
        list.circle(hx, hy, thickness * 0.5f, ui::color::action_hi.with_alpha(opacity));
    }
}

float wordmark_width(const ui::Type &type, std::string_view name, float size)
{
    std::string_view head;
    std::string_view tail;
    split(name, &head, &tail);
    const float tracking = kTracking * size;
    float width = type.semibold.measure(head, size, tracking);
    if (!tail.empty())
        width += kWordGap * size + type.semibold.measure(tail, size, tracking);
    return width;
}

float draw_wordmark(hui::gfx::DrawList &list, const ui::Type &type, std::string_view name, float x,
                    float baseline, float size)
{
    std::string_view head;
    std::string_view tail;
    split(name, &head, &tail);
    const float tracking = kTracking * size;
    float width = hui::ui::text(list, type.semibold, head, x, baseline, size, ui::color::text,
                                hui::gfx::Align::left, tracking);
    if (!tail.empty())
    {
        width += kWordGap * size;
        width += hui::ui::text(list, type.semibold, tail, x + width, baseline, size,
                               ui::color::gold, hui::gfx::Align::left, tracking);
    }
    return width;
}

} // namespace orbit::brand
