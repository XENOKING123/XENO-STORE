// XENO STORE TV app - The brand: badge decoding, the turning rim and the wordmark.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/brand.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace orbit
{
namespace
{

const std::string kAssets = std::string(HUI_SOURCE_DIR) + "/assets";

bool same(const float *rgba, const ui::Color &color)
{
    return std::fabs(rgba[0] - color.r) < 0.03f && std::fabs(rgba[1] - color.g) < 0.03f &&
           std::fabs(rgba[2] - color.b) < 0.03f;
}

TEST(Brand, BadgeDecodesToTheRequestedSizeWithClearCorners)
{
    // The app's two sizes, a small one, the file's own and one above it.
    for (const int size : {44, 128, 256, 512, 600})
    {
        SCOPED_TRACE(size);
        const image::Pixels badge = brand::badge(size, kAssets);
        ASSERT_EQ(badge.width, size);
        ASSERT_EQ(badge.height, size);
        ASSERT_EQ(badge.rgba.size(), static_cast<std::size_t>(size) * size * 4);
        const auto alpha = [&](int x, int y)
        { return badge.rgba[(static_cast<std::size_t>(y) * size + x) * 4 + 3]; };
        EXPECT_EQ(alpha(0, 0), 0);
        EXPECT_EQ(alpha(size - 1, 0), 0);
        EXPECT_EQ(alpha(0, size - 1), 0);
        EXPECT_EQ(alpha(size - 1, size - 1), 0);
        EXPECT_EQ(alpha(size / 2, size / 2), 255);
        // Round, not square: the middle of every edge is covered.
        EXPECT_GT(alpha(size / 2, 1), 128);
        EXPECT_GT(alpha(1, size / 2), 128);
    }
}

TEST(Brand, BadgeIsEmptyWithoutItsFile)
{
    EXPECT_TRUE(brand::badge(128, kAssets + "/missing").empty());
    EXPECT_TRUE(brand::badge(0, kAssets).empty());
}

TEST(Brand, RimTurnsTwoArcsFromBlueToAGoldHead)
{
    hui::gfx::DrawList list;
    brand::draw_rim(list, 880.0f, 250.0f, 160.0f, 0.25f);
    ASSERT_FALSE(list.instances().empty());
    // Each arc's first segment is its blue tail, faded towards the page; its
    // last is its gold head.
    const auto &shapes = list.instances();
    const float *tail = shapes.front().color_top;
    EXPECT_GT(tail[2], tail[1]);
    EXPECT_GT(tail[1], tail[0]);
    EXPECT_LT(tail[2], ui::color::accent.b);
    bool gold_head = false;
    for (const auto &shape : shapes)
        gold_head = gold_head || same(shape.color_top, ui::color::gold);
    EXPECT_TRUE(gold_head);
    // Hidden, it draws nothing.
    hui::gfx::DrawList hidden;
    brand::draw_rim(hidden, 880.0f, 250.0f, 160.0f, 0.25f, 0.0f);
    EXPECT_TRUE(hidden.instances().empty());
}

TEST(Brand, WordmarkSetsTheSecondWordInGold)
{
    test::TestFonts fonts;
    hui::gfx::DrawList list;
    const float width = brand::draw_wordmark(list, fonts.type, "XENO STORE", 100.0f, 60.0f, 22.0f);
    EXPECT_NEAR(width, brand::wordmark_width(fonts.type, "XENO STORE", 22.0f), 0.01f);
    EXPECT_GT(width, brand::wordmark_width(fonts.type, "XENO", 22.0f));
    int text = 0;
    int gold = 0;
    for (const auto &glyph : list.instances())
    {
        text += same(glyph.color_top, ui::color::text) ? 1 : 0;
        gold += same(glyph.color_top, ui::color::gold) ? 1 : 0;
    }
    EXPECT_EQ(text, 4); // XENO
    EXPECT_EQ(gold, 5); // STORE
}

} // namespace
} // namespace orbit
