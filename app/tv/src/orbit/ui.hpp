// Orbit Store TV app - The storefront's look: tokens and drawing helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
//
// The same design language as the browser storefront (docs/design.md): black
// layers, art that fills the screen, one white focus ring with a soft white
// glow, light large titles, pill buttons. Sizes are virtual 1920 x 1080 px.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::ui
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

namespace color
{
// XENO STORE's gold on deep blue, from the web UI's tokens
// (web/src/css/00-tokens.css). Contrast against night0 / night2 / night3:
// text 17.3 / 14.5 / 12.7, text2 9.5 / 7.9 / 7.0, text3 5.4 / 4.6 / 4.0.
// text3 is never used for text under 24 px on night3 or on a focused or
// selected surface; text2 is used there.
inline const Color night0 = Color::rgb(0x060b1c);     // --bg0
inline const Color night1 = Color::rgb(0x0a1330);     // --bg1
inline const Color night2 = Color::rgb(0x0f1d44);     // --card
inline const Color night3 = Color::rgb(0x15265a);     // --card2
inline const Color glow = Color::rgb(0x15295e);       // --glow-spot
inline const Color aurora1 = Color::rgb(0x1d3f9a);    // --aurora-1
inline const Color aurora2 = Color::rgb(0x4a2a9a);    // --aurora-2
inline const Color text = Color::rgb(0xeaf1ff);       // --txt
inline const Color text2 = Color::rgb(0x9fb4e6);      // --dim
inline const Color text3 = Color::rgb(0x6f86bf);      // --mute
inline const Color gold = Color::rgb(0xffce4a);       // --gold
inline const Color action = Color::rgb(0xffce4a);     // --gold: primary buttons, progress
inline const Color action_ink = Color::rgb(0x1a1200); // --gold-ink: text on gold
inline const Color action_hi = Color::rgb(0xffe08a);  // --gold2
inline const Color ok = Color::rgb(0x4aff8a);         // --good
inline const Color bad = Color::rgb(0xff5c6c);        // --bad
inline const Color warn = Color::rgb(0xff9a4a);       // --warn
inline const Color accent = Color::rgb(0x4aa3ff);     // --acc: work in progress
inline const Color ps5 = Color::rgb(0x2f7cf6);        // --ps5
inline const Color ps4 = Color::rgb(0x8a94b3);        // --ps4
inline const Color line = Color::rgb(0xffffff, 0.16f);
// Fields are tonal surfaces, not outlines (the browser's --field tokens).
inline const Color field = Color::rgb(0xffffff, 0.075f);
inline const Color field_open = Color::rgb(0xffffff, 0.145f);
inline const Color field_edge = Color::rgb(0xffffff, 0.06f);
// Over game art they are dark glass, so text stays readable on bright covers.
inline const Color glass = Color::rgb(0x0a1330, 0.72f); // --glass
inline const Color glass_on = Color::rgb(0x15265a, 0.82f);
// An open list: a frosted panel over the screen.
inline const Color popover = Color::rgb(0x060b1c, 0.9f);
// What dims the screen behind a dialog.
inline const Color scrim = Color::rgb(0x060b1c, 0.7f); // --scrim
// The selected tab, pill or option: gold with dark ink, as the web's .on.
inline const Color focus_fill = Color::rgb(0xffce4a);
inline const Color focus_ink = Color::rgb(0x1a1200);
inline const Color gold_soft = Color::rgb(0xffce4a, 0.12f); // --gold-soft
inline const Color gold_line = Color::rgb(0xffce4a, 0.42f); // --gold-line
// Not themed: an image or the glass drawn as it is (and the base of the
// faint white veils, with_alpha), and the black of shadows.
inline const Color white = Color::rgb(0xffffff);
inline const Color shadow = Color::rgb(0x000000);
} // namespace color

constexpr float kGutter = 100.0f;     // left and right page margin
constexpr float kArtRadius = 14.0f;   // covers and art
constexpr float kPanelRadius = 22.0f; // dialogs

// The four faces the app draws with (all Inter, SIL Open Font License).
struct Type
{
    hui::ui::FontRef light;    // Inter Display Light: large titles
    hui::ui::FontRef regular;  // body
    hui::ui::FontRef medium;   // tabs, buttons
    hui::ui::FontRef semibold; // headings, labels
};

// One line of text; x is the left edge, centre or right edge. Returns its width.
float text(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
           float baseline, float size, Color color, Align align = Align::left);
// The same, shortened with an ellipsis to fit max_width.
float text_fit(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
               float baseline, float size, float max_width, Color color, Align align = Align::left);
// The largest size from `size` down to `smallest` at which value fits max_width.
float fit_size(const hui::ui::FontRef &font, std::string_view value, float size, float smallest,
               float max_width);
// Wrapped text from its first baseline; returns the baseline after the last line.
float paragraph(DrawList &list, const hui::ui::FontRef &font, std::string_view value, float x,
                float baseline, float size, float width, float line_height, Color color,
                int max_lines, Align align = Align::left);

// The focus ring: gold, 3 px, outside the shape, with a soft gold glow.
// amount (0..1) fades it in and out. Draw it before the shape it surrounds,
// so the glow sits behind it like a box-shadow.
void focus_ring(DrawList &list, const Rect &r, float radius, float amount);

enum class ButtonKind : std::uint8_t
{
    primary,   // gold action with dark text
    light,     // gold storefront action
    secondary, // translucent white
    danger,
};
// The browser's .primary: 0.75 rem padding around a 1 rem label.
constexpr float kButtonHeight = 54.0f;
float button_width(const Type &type, std::string_view label, float size = 20.0f,
                   ButtonKind kind = ButtonKind::primary);
// A pill button. focus (0..1) draws the ring and grows a primary button.
void button(DrawList &list, const Type &type, const Rect &r, std::string_view label,
            ButtonKind kind, float focus, bool enabled = true, float size = 20.0f);

// A small rounded label ("PS5", a genre, a status). Returns its width.
float chip(DrawList &list, const Type &type, float x, float cy, std::string_view label, Color fill,
           Color ink, float size = 20.0f, Color border = Color{0, 0, 0, 0});

// The browser's .platform badge ("PS5"): outlined in white, 6 px corners.
// Returns its width.
float platform_badge(DrawList &list, const Type &type, float x, float cy, std::string_view label);

// Draws a texture to fill r, cropping the overflow (CSS object-fit: cover);
// anchor_x/y place the crop as object-position does (0.5 centres it).
void image_cover(DrawList &list, std::uint32_t texture, int width, int height, const Rect &r,
                 Color tint, float radius = 0.0f, float anchor_x = 0.5f, float anchor_y = 0.5f);
// What a cover shows before (or instead of) its art: a dark plate with the title,
// and the brand badge (a texture, when given) faintly behind it.
void cover_placeholder(DrawList &list, const Type &type, const Rect &r, std::string_view title,
                       float radius, std::uint32_t badge = 0);

void progress_bar(DrawList &list, const Rect &r, float fraction, Color fill);

// The browser's .modal: a dark panel with a hairline edge and a deep shadow.
void modal(DrawList &list, const Rect &r);

// A field's surface: the fill, with a hairline of light along its top edge.
void surface(DrawList &list, const Rect &r, float radius, Color fill);

// The browser's Select. A .field label above the trigger, when given.
constexpr float kFieldHeight = 58.0f; // .select-trigger: 2.9 rem
void field_label(DrawList &list, const Type &type, const Rect &field, std::string_view label);
enum class FieldLook : std::uint8_t
{
    box,   // forms and dialogs
    pill,  // filter rows, beside the search pill
    glass, // over game art
};
// .select-trigger: the chosen value in a tonal field, with a chevron.
void select_field(DrawList &list, const Type &type, const Rect &r, std::string_view value,
                  float focus, bool open = false, bool enabled = true,
                  FieldLook look = FieldLook::box);
// .select-menu under (or, near the bottom, over) its trigger: the chosen
// option has a check, the controller's option is lit gold. in fades it.
// With the frame's glass texture (set frame.glass) the panel is frosted.
void select_menu(DrawList &list, const Type &type, const Rect &trigger,
                 const std::vector<std::string> &options, int selected, int active, float in,
                 std::uint32_t glass_texture = 0);

// Line-art icons drawn from shapes.
void icon_search(DrawList &list, float cx, float cy, float size, Color ink);
void icon_settings(DrawList &list, float cx, float cy, float size, Color ink);
void icon_check(DrawList &list, float cx, float cy, float size, Color ink, float thickness = 3.0f);
void icon_download(DrawList &list, float cx, float cy, float size, Color ink);
void icon_chevron(DrawList &list, float cx, float cy, float size, Color ink, bool up = false);
void icon_back(DrawList &list, float cx, float cy, float size, Color ink);
void icon_heart(DrawList &list, float cx, float cy, float size, Color ink);
void icon_layers(DrawList &list, float cx, float cy, float size, Color ink);

// A hint row in the bottom-right corner, drawn as the browser's footer.
void hints(DrawList &list, const Type &type, const std::vector<hui::ui::Hint> &items);

// A value that eases toward a target (focus fades, scrolling, crossfades).
struct Ease
{
    float value = 0.0f;
    float target = 0.0f;
    void snap(float v)
    {
        value = target = v;
    }
    void update(float dt, float rate = 14.0f);
};

} // namespace orbit::ui
