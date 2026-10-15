// XENO STORE TV app - The brand: the logo badge, its turning rim and the wordmark.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes the place of the procedural planet mark the app was built with. The
// badge is the home-screen icon cut to a circle (assets/brand/xeno-badge.png,
// made by tools/make-brand.py); the rim and the wordmark follow the web UI's
// .brand (web/src/css/10-base.css): a gold ring that turns once every 2.4 s,
// and "XENO" in the text colour followed by "STORE" in gold.

#pragma once

#include "gfx/draw_list.hpp"
#include "orbit/image.hpp"
#include "orbit/ui.hpp"

#include <string>
#include <string_view>

namespace orbit::brand
{

// The badge's place inside the app's assets folder.
inline constexpr const char *kBadgeFile = "brand/xeno-badge.png";

// The badge decoded and resized to size x size RGBA, transparent outside its
// circle. Empty when the file is missing or unreadable.
image::Pixels badge(int size, const std::string &assets_dir = "/app0/assets");

// The rim round a badge drawn at (x, y) with the given size: two 120-degree
// arcs facing each other, gold at the head and the accent blue at the tail.
// phase 0..1 is one clockwise turn.
void draw_rim(hui::gfx::DrawList &list, float x, float y, float size, float phase,
              float opacity = 1.0f);

// The wordmark: the name's first word in the text colour, the rest in gold,
// semibold with 0.12 em letter spacing. Returns its width.
float wordmark_width(const ui::Type &type, std::string_view name, float size);
float draw_wordmark(hui::gfx::DrawList &list, const ui::Type &type, std::string_view name, float x,
                    float baseline, float size);

} // namespace orbit::brand
