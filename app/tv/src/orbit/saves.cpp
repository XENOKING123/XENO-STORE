// XENO STORE TV app - Saves: the community saves of the Save Data Store, and installing one.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser version's Community Saves (web/src/js/47-saves.js) on the TV:
// the saves XENO STORE lists from the XenoKing site and the GitHub index
// (GET /api/saves/store), by game, by the games installed, by category or by
// search, as cards; a save's details, and Install for a console profile,
// followed to its end (POST /api/saves/store/{id}/install, then the job).
// Covers are the catalogue's, through the store as everywhere in the app: a
// save's own cover link points outside the console, and the app has no TLS.

#include "orbit/brand.hpp"
#include "orbit/i18n.hpp"
#include "orbit/screens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

// Browse's header (title, search beside it), then the chips and two columns of cards.
constexpr float kTitleBaseline = 176.0f;
constexpr float kSubtitleBaseline = 216.0f;
constexpr float kFieldY = 122.0f;
constexpr float kFieldWidth = 660.0f;
constexpr float kFieldHeight = 66.0f;
constexpr float kChipsY = 244.0f;
constexpr float kChipHeight = 42.0f;
constexpr float kChipGap = 8.0f;
constexpr float kGameChipWidth = 360.0f; // the longest game name a chip shows
constexpr float kGridTop = 318.0f;
constexpr int kColumns = 2;
constexpr float kCardGap = 24.0f;
constexpr float kCardWidth = (1920.0f - 2.0f * ui::kGutter - kCardGap) / kColumns;
constexpr float kCardHeight = 196.0f;
constexpr float kCover = 152.0f;
constexpr float kKeyboardTop = 560.0f;
// The details sheet.
constexpr float kSheetX = 150.0f;
constexpr float kSheetY = 96.0f;
constexpr float kSheetW = 1620.0f;
constexpr float kSheetH = 868.0f;
constexpr float kSheetPad = 56.0f;
constexpr float kSheetCover = 300.0f;
constexpr float kButtonsY = kSheetY + kSheetH - kSheetPad - ui::kButtonHeight;
constexpr float kProfileWidth = 420.0f;
// The facts row sits under the cover; long notes push it down this far at
// most (the compatibility sentence under it stays clear of the install's
// progress), and keep this much room above its labels.
constexpr float kFactsY = kSheetY + kSheetPad + kSheetCover + 46.0f;
constexpr float kFactsLowest = kFactsY + 62.0f;
constexpr float kFactsGap = 52.0f;

ui::Color compat_ink(SaveCompat compat)
{
    switch (compat)
    {
    case SaveCompat::match:
    case SaveCompat::older_save:
        return ui::color::ok;
    case SaveCompat::update_game:
    case SaveCompat::other_region:
        return ui::color::warn;
    default:
        return ui::color::text2;
    }
}

// The badge: tinted for a verdict, quiet when there is none.
float compat_badge(hui::gfx::DrawList &list, const ui::Type &type, float x, float cy,
                   const SaveRow &save, float size)
{
    const SaveCompat compat = save_compat(save);
    const ui::Color ink = compat_ink(compat);
    const bool verdict = compat != SaveCompat::not_installed && compat != SaveCompat::unknown;
    return ui::chip(list, type, x, cy, save_compat_label(compat),
                    verdict ? ink.with_alpha(0.14f) : ui::color::white.with_alpha(0.08f), ink, size);
}

std::string game_line(const SaveRow &save)
{
    if (save.title.empty())
        return save.title_id;
    return save.title_id.empty() ? save.title : save.title + " \xC2\xB7 " + save.title_id;
}

std::string shared_line(const SaveRow &save)
{
    if (save.author.empty())
        return save.source_name.empty() ? std::string()
                                        : tr("Shared on {source}", {{"source", save.source_name}});
    return save.source_name.empty()
               ? tr("Shared by {name}", {{"name", save.author}})
               : tr("Shared by {name} on {source}", {{"name", save.author}, {"source", save.source_name}});
}

std::string profile_label(const SaveProfile &profile)
{
    const std::string name = profile.name.empty() ? std::to_string(profile.id) : profile.name;
    return profile.foreground ? tr("{name} (signed in)", {{"name", name}}) : name;
}

// The profile a save installs for: the one chosen, or (0) the signed-in one.
const SaveProfile *find_profile(const std::vector<SaveProfile> &profiles, std::uint32_t user)
{
    for (const SaveProfile &profile : profiles)
    {
        if (profile.id == user || (user == 0 && profile.foreground))
            return &profile;
    }
    return nullptr;
}

// A step with no size (installing, Garlic Save Manager's steps): a stretch
// of gold that sweeps across the track instead of a fraction.
void busy_bar(hui::gfx::DrawList &list, const ui::Rect &r, float clock)
{
    ui::progress_bar(list, r, 0.0f, ui::color::action);
    const float stretch = r.w * 0.28f;
    const float x = r.x - stretch + (r.w + stretch) * std::fmod(clock / 1.6f, 1.0f);
    const float left = std::max(r.x, x);
    const float right = std::min(r.x + r.w, x + stretch);
    if (right - left > r.h)
        list.rounded_rect({left, r.y, right - left, r.h}, r.h * 0.5f, ui::color::action);
}

// A save's cover: its game's catalogue art, or a plate with the game's initial.
void draw_save_cover(Context &ctx, hui::gfx::DrawList &list, const SaveRow &save, const Game *game,
                     const ui::Rect &r, float radius)
{
    if (game != nullptr)
    {
        draw_cover(ctx, list, *game, r, radius);
        return;
    }
    if (save.game_id.empty())
    {
        ui::cover_placeholder(list, ctx.type, r, save.title.empty() ? save.title_id : save.title,
                              radius, ctx.loader_mark);
        return;
    }
    // A game the enabled sources leave out still has the catalogue's art.
    Game stand_in;
    stand_in.id = save.game_id;
    stand_in.title = save.title;
    draw_cover(ctx, list, stand_in, r, radius);
}

} // namespace

SavesScreen::SavesScreen(Context &context) : Screen(context)
{
    keyboard_.style.theme = keyboard_theme();
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.max_length = 40;
    keyboard_.style.done_label = tr("Done");
    keyboard_.set_bounds({ui::kGutter + 220.0f, kKeyboardTop + 40.0f, 1280.0f, 400.0f});
}

void SavesScreen::focus()
{
    const Snapshot &state = ctx_.store.state();
    if (saves_serial_ != ctx_.store.saves().state().serial || catalog_serial_ != state.catalog_serial)
        refilter();
}

void SavesScreen::show_game(const std::string &game_id)
{
    game_id_ = game_id;
    mine_ = false;
    category_ = -1;
    query_.clear();
    keyboard_.set_length(0);
    close_details();
    index_ = 0;
    refilter();
    area_ = results_.empty() ? Area::chips : Area::grid;
    chip_ = current_chip();
}

void SavesScreen::set_query(const std::string &query)
{
    query_ = query;
    index_ = 0;
    refilter();
    keyboard_.set_length(static_cast<int>(query_.size()));
}

void SavesScreen::open_save(const std::string &save_id)
{
    const auto &saves = ctx_.store.saves().state().listing.saves;
    for (int i = 0; i < static_cast<int>(saves.size()); ++i)
    {
        if (saves[static_cast<std::size_t>(i)].id == save_id)
        {
            open_details(i);
            return;
        }
    }
}

void SavesScreen::start_search()
{
    area_ = Area::field;
    keyboard_open_ = true;
    keyboard_.enter();
    keyboard_.set_length(static_cast<int>(query_.size()));
}

// The catalogue game each save belongs to (looked up again only when the
// saves or the catalogue change, not for every key typed), the saves that
// match, and the chips.
void SavesScreen::refilter()
{
    const SavesState &saves = ctx_.store.saves().state();
    const Snapshot &state = ctx_.store.state();
    const std::vector<SaveRow> &rows = saves.listing.saves;
    if (saves_serial_ != saves.serial || catalog_serial_ != state.catalog_serial ||
        games_.size() != rows.size())
    {
        saves_serial_ = saves.serial;
        catalog_serial_ = state.catalog_serial;
        games_.assign(rows.size(), -1);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            for (std::size_t g = 0; g < state.games.size() && games_[i] < 0; ++g)
            {
                if (!rows[i].game_id.empty() && state.games[g].id == rows[i].game_id)
                    games_[i] = static_cast<int>(g);
            }
            for (std::size_t g = 0; g < state.games.size() && games_[i] < 0; ++g)
            {
                if (save_for_game(rows[i], state.games[g]))
                    games_[i] = static_cast<int>(g);
            }
        }
    }
    if (!game_id_.empty())
    {
        const Game *game = state.game(game_id_);
        game_ = game != nullptr ? *game : Game{};
        game_.id = game_id_;
        // A game the enabled sources leave out is named by its saves.
        for (std::size_t i = 0; game_.title.empty() && i < rows.size(); ++i)
        {
            if (rows[i].game_id == game_id_)
                game_.title = rows[i].title;
        }
    }

    SaveFilters filters;
    filters.query = query_;
    filters.game = game_id_.empty() ? nullptr : &game_;
    filters.mine = mine_;
    filters.category = category_;
    results_ = filter_saves(rows, filters);

    // The chips, with how many saves each would show for this search.
    const auto count_for = [&](bool mine, int category, const Game *game)
    {
        SaveFilters chip;
        chip.query = query_;
        chip.mine = mine;
        chip.category = category;
        chip.game = game;
        return static_cast<int>(filter_saves(rows, chip).size());
    };
    chips_.clear();
    if (!game_id_.empty())
        chips_.push_back({Chip::Kind::game, SaveCategory::other,
                          game_.title.empty() ? game_id_ : game_.title, count_for(false, -1, &game_)});
    chips_.push_back({Chip::Kind::all, SaveCategory::other, tr("All"), count_for(false, -1, nullptr)});
    chips_.push_back({Chip::Kind::mine, SaveCategory::other, tr("For my games"),
                      count_for(true, -1, nullptr)});
    for (const SaveCategory category : save_categories(rows))
        chips_.push_back({Chip::Kind::category, category, save_category_label(category),
                          count_for(false, static_cast<int>(category), nullptr)});
    chip_focus_.resize(chips_.size(), 0.0f);
    chip_ = std::clamp(chip_, 0, static_cast<int>(chips_.size()) - 1);
    index_ = std::clamp(index_, 0, std::max(0, static_cast<int>(results_.size()) - 1));
    card_focus_.assign(results_.size(), 0.0f);
}

int SavesScreen::current_chip() const
{
    for (int i = 0; i < static_cast<int>(chips_.size()); ++i)
    {
        const Chip &chip = chips_[static_cast<std::size_t>(i)];
        if ((chip.kind == Chip::Kind::game && !game_id_.empty()) ||
            (chip.kind == Chip::Kind::mine && game_id_.empty() && mine_) ||
            (chip.kind == Chip::Kind::category && game_id_.empty() && !mine_ &&
             static_cast<int>(chip.category) == category_) ||
            (chip.kind == Chip::Kind::all && game_id_.empty() && !mine_ && category_ < 0))
            return i;
    }
    return 0;
}

// A chip with no saves for this search is dimmed and passed over, unless it
// is the filter in use; the game shown and All always stay.
bool SavesScreen::chip_usable(int index) const
{
    if (index < 0 || index >= static_cast<int>(chips_.size()))
        return false;
    const Chip &chip = chips_[static_cast<std::size_t>(index)];
    return chip.kind == Chip::Kind::game || chip.kind == Chip::Kind::all || chip.count > 0 ||
           index == current_chip();
}

// Try again: the saves could not be loaded, or the store said it has none.
bool SavesScreen::can_retry() const
{
    const SavesLoad load = ctx_.store.saves().state().load;
    return load == SavesLoad::failed || load == SavesLoad::unsupported;
}

// One filter at a time, as the Downloads views: All clears the others.
void SavesScreen::choose_chip(int index)
{
    const Chip chip = chips_[static_cast<std::size_t>(index)];
    if (chip.kind == Chip::Kind::game)
        return;
    game_id_.clear();
    mine_ = chip.kind == Chip::Kind::mine;
    category_ = chip.kind == Chip::Kind::category ? static_cast<int>(chip.category) : -1;
    index_ = 0;
    refilter();
    chip_ = current_chip();
}

ui::Rect SavesScreen::card_rect(int index) const
{
    const int row = index / kColumns;
    const int column = index % kColumns;
    return {ui::kGutter + static_cast<float>(column) * (kCardWidth + kCardGap),
            kGridTop + static_cast<float>(row) * (kCardHeight + kCardGap), kCardWidth, kCardHeight};
}

ui::Rect SavesScreen::chip_rect(int index) const
{
    const ui::Type &type = ctx_.type;
    float x = ui::kGutter;
    ui::Rect r{};
    for (int i = 0; i <= index && i < static_cast<int>(chips_.size()); ++i)
    {
        const Chip &chip = chips_[static_cast<std::size_t>(i)];
        char number[16];
        std::snprintf(number, sizeof(number), "%d", chip.count);
        float label = type.medium.measure(chip.label, 18.0f);
        if (chip.kind == Chip::Kind::game)
            label = std::min(label, kGameChipWidth);
        const float width = label + 7.0f + type.medium.measure(number, 18.0f) + 2.0f * 18.0f;
        r = {x, kChipsY, width, kChipHeight};
        x += width + kChipGap;
    }
    return r;
}

const Game *SavesScreen::game_of(int save) const
{
    const Snapshot &state = ctx_.store.state();
    if (save < 0 || save >= static_cast<int>(games_.size()))
        return nullptr;
    const int game = games_[static_cast<std::size_t>(save)];
    return game >= 0 && game < static_cast<int>(state.games.size())
               ? &state.games[static_cast<std::size_t>(game)]
               : nullptr;
}

// The cards show once the store answered with saves, or with none for a filter.
bool SavesScreen::list_shown() const
{
    const SavesState &saves = ctx_.store.saves().state();
    return saves.load == SavesLoad::ready && !saves.listing.saves.empty();
}

void SavesScreen::open_details(int save)
{
    const auto &saves = ctx_.store.saves().state();
    if (save < 0 || save >= static_cast<int>(saves.listing.saves.size()))
        return;
    open_ = saves.listing.saves[static_cast<std::size_t>(save)].id;
    // The signed-in profile for every save opened, until the player picks another.
    user_ = saves.profiles.empty() ? 0 : saves.profiles.front().id;
    for (const SaveProfile &profile : saves.profiles)
    {
        if (profile.foreground)
            user_ = profile.id;
    }
    // Install, unless this save's install runs or ended, or another one runs.
    const SaveInstall &install = ctx_.store.saves().install();
    control_ = (install.save_id == open_ && install.ended()) || install.active ? Control::back
                                                                                : Control::install;
    sheet_in_.snap(0.0f);
}

void SavesScreen::close_details()
{
    SaveStore &store = ctx_.store.saves();
    // A finished install's outcome was shown here: the next look starts afresh.
    if (!open_.empty() && store.install().save_id == open_ && store.install().ended())
        store.clear_install();
    open_.clear();
    picker_.open = false;
}

std::vector<SavesScreen::Control> SavesScreen::controls() const
{
    const SaveStore &store = ctx_.store.saves();
    const SaveInstall &install = store.install();
    if (install.save_id == open_ && install.active)
        return {Control::cancel, Control::back};
    if (install.save_id == open_ && install.ended())
    {
        if (install.have_job && install.job.state == "done")
            return {Control::done};
        return {Control::install, Control::back};
    }
    std::vector<Control> list;
    if (store.state().profiles.size() > 1)
        list.push_back(Control::profile);
    list.push_back(Control::install);
    list.push_back(Control::back);
    return list;
}

ui::Rect SavesScreen::control_rect(int index) const
{
    const ui::Type &type = ctx_.type;
    const std::vector<Control> list = controls();
    const SaveInstall &install = ctx_.store.saves().install();
    float x = kSheetX + kSheetPad;
    ui::Rect r{};
    for (int i = 0; i <= index && i < static_cast<int>(list.size()); ++i)
    {
        float width = kProfileWidth;
        switch (list[static_cast<std::size_t>(i)])
        {
        case Control::install:
            width = ui::button_width(type, install.save_id == open_ && install.ended()
                                               ? tr("Install again")
                                               : tr("Install"),
                                     22.0f);
            width = std::max(width, 240.0f);
            break;
        case Control::cancel:
            width = ui::button_width(type, tr("Cancel"), 22.0f, ui::ButtonKind::secondary);
            break;
        case Control::done:
            width = std::max(240.0f, ui::button_width(type, tr("Done"), 22.0f));
            break;
        case Control::back:
            width = ui::button_width(type, tr("Back"), 22.0f, ui::ButtonKind::secondary);
            break;
        default:
            break;
        }
        r = {x, list[static_cast<std::size_t>(i)] == Control::profile ? kButtonsY - 2.0f : kButtonsY,
             width, list[static_cast<std::size_t>(i)] == Control::profile ? ui::kFieldHeight
                                                                           : ui::kButtonHeight};
        x += width + 20.0f;
    }
    return r;
}

Intent SavesScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    SaveStore &store = ctx_.store.saves();
    store.want();
    const SavesState &saves = store.state();
    const Snapshot &state = ctx_.store.state();
    if (saves_serial_ != saves.serial || catalog_serial_ != state.catalog_serial)
        refilter();
    const SaveRow *open = open_.empty() ? nullptr : saves.save(open_);
    if (!open_.empty() && open == nullptr)
        close_details(); // the list changed under it

    if (focused && keyboard_open_)
    {
        const hui::ui::Event event = keyboard_.handle(input, feedback);
        bool changed = false;
        for (int i = 0; i < keyboard_.erased(); ++i)
        {
            pop_character(query_);
            changed = true;
        }
        if (!keyboard_.typed().empty())
        {
            query_ += keyboard_.typed();
            changed = true;
        }
        if (changed)
        {
            index_ = 0;
            refilter();
        }
        if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
        {
            keyboard_open_ = false;
            // Done goes to the saves; back returns to the field.
            area_ = event == hui::ui::Event::activated && !results_.empty() ? Area::grid : Area::field;
            feedback.play(hui::audio::Cue::modal_close);
        }
    }
    else if (focused && picker_.open)
    {
        const int chosen = picker_.update(input, static_cast<int>(saves.profiles.size()), feedback);
        if (chosen >= 0 && chosen < static_cast<int>(saves.profiles.size()))
            user_ = saves.profiles[static_cast<std::size_t>(chosen)].id;
    }
    else if (focused && open != nullptr)
    {
        const std::vector<Control> list = controls();
        const SaveInstall &install = store.install();
        const bool other_install = install.active && install.save_id != open_;
        int at = 0;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            at = list[static_cast<std::size_t>(i)] == control_ ? i : at;
        if (std::find(list.begin(), list.end(), control_) == list.end())
        {
            // The buttons changed (an install started or ended): a harmless one.
            at = static_cast<int>(list.size()) - 1;
            control_ = list.back();
        }
        if (input.is_pressed(Action::back))
        {
            close_details();
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            int next = at + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < static_cast<int>(list.size()) &&
                list[static_cast<std::size_t>(next)] == Control::install && other_install)
                next += input.nav == Direction::right ? 1 : -1;
            if (next >= 0 && next < static_cast<int>(list.size()))
            {
                control_ = list[static_cast<std::size_t>(next)];
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav != Direction::none)
        {
            refuse(feedback, input);
        }
        else if (input.is_pressed(Action::confirm))
        {
            switch (control_)
            {
            case Control::profile:
            {
                int selected = 0;
                for (int i = 0; i < static_cast<int>(saves.profiles.size()); ++i)
                    selected = saves.profiles[static_cast<std::size_t>(i)].id == user_ ? i : selected;
                picker_.show(selected);
                feedback.play(hui::audio::Cue::modal_open);
                break;
            }
            case Control::install:
                if (other_install || install.active)
                {
                    refuse(feedback, input);
                    break;
                }
                store.install_save(*open, user_);
                control_ = Control::back;
                feedback.play(hui::audio::Cue::launch);
                break;
            case Control::cancel:
                if (install.cancelling)
                {
                    refuse(feedback, input);
                    break;
                }
                store.cancel_install();
                feedback.play(hui::audio::Cue::erase);
                break;
            case Control::done:
            case Control::back:
                close_details();
                feedback.play(hui::audio::Cue::modal_close);
                break;
            }
        }
    }
    else if (focused)
    {
        const auto move_to = [&](Area area)
        {
            area_ = area;
            feedback.play(hui::audio::Cue::focus);
        };
        const bool shown = list_shown();
        if (!shown && (area_ == Area::chips || area_ == Area::grid))
            area_ = Area::field;
        // Nothing to search: Try again has the focus where there is one.
        if (can_retry())
            area_ = Area::retry;
        else if (area_ == Area::retry)
            area_ = Area::field;
        switch (area_)
        {
        case Area::field:
            if (input.nav == Direction::up)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            if (input.nav == Direction::down && shown)
            {
                move_to(Area::chips);
                chip_ = current_chip();
            }
            else if (input.nav != Direction::none)
                refuse(feedback, input);
            if (input.is_pressed(Action::confirm) && shown)
            {
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            else if (input.is_pressed(Action::west) && !query_.empty())
            {
                query_.clear();
                index_ = 0;
                refilter();
                feedback.play(hui::audio::Cue::erase);
            }
            break;
        case Area::retry:
            if (input.nav == Direction::up)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            if (input.nav != Direction::none)
                refuse(feedback, input);
            if (input.is_pressed(Action::confirm))
            {
                store.reload();
                feedback.play(hui::audio::Cue::select);
            }
            break;
        case Area::chips:
        {
            const int count = static_cast<int>(chips_.size());
            if (input.nav == Direction::up)
                move_to(Area::field);
            else if (input.nav == Direction::down)
            {
                if (!results_.empty())
                {
                    area_ = Area::grid;
                    index_ = std::min(index_, kColumns - 1);
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int step = input.nav == Direction::right ? 1 : -1;
                int next = chip_ + step;
                while (next >= 0 && next < count && !chip_usable(next))
                    next += step;
                if (next >= 0 && next < count)
                {
                    chip_ = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            if (input.is_pressed(Action::confirm) && chip_usable(chip_))
            {
                choose_chip(chip_);
                feedback.play(hui::audio::Cue::toggle);
            }
            break;
        }
        case Area::grid:
        {
            const int count = static_cast<int>(results_.size());
            if (count == 0)
            {
                area_ = Area::chips;
                break;
            }
            int next = index_;
            if (input.nav == Direction::left)
                next = index_ % kColumns == 0 ? -1 : index_ - 1;
            else if (input.nav == Direction::right)
                next = index_ % kColumns == kColumns - 1 || index_ + 1 >= count ? -1 : index_ + 1;
            else if (input.nav == Direction::down)
                next = index_ + kColumns < count                    ? index_ + kColumns
                       : index_ / kColumns < (count - 1) / kColumns ? count - 1
                                                                    : -1;
            else if (input.nav == Direction::up)
                next = index_ - kColumns;
            if (input.nav != Direction::none)
            {
                if (input.nav == Direction::up && next < 0)
                {
                    move_to(Area::chips);
                    chip_ = current_chip();
                }
                else if (next >= 0 && next < count)
                {
                    index_ = next;
                    feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
                }
                else
                    refuse(feedback, input);
            }
            if (input.is_pressed(Action::confirm))
            {
                open_details(results_[static_cast<std::size_t>(index_)]);
                feedback.play(hui::audio::Cue::modal_open);
            }
            else if (input.is_pressed(Action::north))
            {
                area_ = Area::field;
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            break;
        }
        }
    }

    // A running install is followed every second; the frame only animates.
    keyboard_.set_active(keyboard_open_);
    keyboard_.update(dt);
    keyboard_in_.target = keyboard_open_ ? 1.0f : 0.0f;
    keyboard_in_.update(dt, 14.0f);
    picker_.animate(dt);
    sheet_in_.target = open_.empty() ? 0.0f : 1.0f;
    sheet_in_.update(dt, 16.0f);
    const bool live = focused && !keyboard_open_ && open_.empty();
    field_focus_.target = focused && area_ == Area::field && open_.empty() ? 1.0f : 0.0f;
    field_focus_.update(dt, 16.0f);
    retry_focus_ += ((live && area_ == Area::retry ? 1.0f : 0.0f) - retry_focus_) *
                    std::min(1.0f, dt * 16.0f);
    for (int i = 0; i < static_cast<int>(chip_focus_.size()); ++i)
    {
        const bool on = live && area_ == Area::chips && i == chip_;
        float &amount = chip_focus_[static_cast<std::size_t>(i)];
        amount += ((on ? 1.0f : 0.0f) - amount) * std::min(1.0f, dt * 16.0f);
    }
    for (int i = 0; i < static_cast<int>(card_focus_.size()); ++i)
    {
        const bool on = live && area_ == Area::grid && i == index_;
        float &amount = card_focus_[static_cast<std::size_t>(i)];
        amount += ((on ? 1.0f : 0.0f) - amount) * std::min(1.0f, dt * 16.0f);
    }
    // The page scrolls under the top bar to keep the focused row in view.
    float target = 0.0f;
    if (area_ == Area::grid && !results_.empty())
    {
        const float top = card_rect(index_).y;
        target = std::max(0.0f, top + kCardHeight + kCardGap - 1000.0f);
        target = std::min(target, std::max(0.0f, top - 140.0f));
    }
    scroll_.target = target;
    scroll_.update(dt, 10.0f);
    return intent;
}

void SavesScreen::draw_card(hui::gfx::DrawList &list, const SaveRow &save, const Game *game,
                            const ui::Rect &r, float focus) const
{
    const ui::Type &type = ctx_.type;
    ui::focus_ring(list, r, 20.0f, focus);
    ui::surface(list, r, 20.0f, focus > 0.5f ? ui::color::field_open : ui::color::field);
    draw_save_cover(ctx_, list, save, game, {r.x + 22.0f, r.y + 22.0f, kCover, kCover}, 12.0f);
    const float tx = r.x + 22.0f + kCover + 26.0f;
    const float width = r.x + r.w - 24.0f - tx;
    const ui::Color quiet = focus > 0.5f ? ui::color::text2 : ui::color::text3;
    ui::text_fit(list, type.medium, save.name.empty() ? std::string(tr("Untitled save")) : save.name,
                 tx, r.y + 52.0f, 24.0f, width, ui::color::text);
    ui::text_fit(list, type.regular, game_line(save), tx, r.y + 84.0f, 20.0f, width,
                 ui::color::text2);
    const std::string shared = shared_line(save);
    if (!shared.empty())
        ui::text_fit(list, type.regular, shared, tx, r.y + 114.0f, 18.0f, width, quiet);
    const float badge = compat_badge(list, type, tx, r.y + 154.0f, save, 15.0f);
    // Its size and date, or where its install is: running (with how far it
    // got), installed, or failed, until its details have shown the outcome.
    const SaveInstall &install = ctx_.store.saves().install();
    const bool installing = install.save_id == save.id;
    std::string facts;
    ui::Color ink = quiet;
    bool marked = true;
    if (installing && install.active)
    {
        const SaveJob &job = install.job;
        facts = install.cancelling ? tr("Cancelling\xE2\x80\xA6") : tr("Installing\xE2\x80\xA6");
        if (!install.cancelling && install.have_job && job.total > 0.0 && job.done > 0.0)
            facts += " " + i18n::percent(std::clamp(job.done / job.total, 0.0, 1.0) * 100.0);
        ink = ui::color::action;
    }
    else if (installing && install.succeeded())
    {
        facts = tr("Installed");
        ink = ui::color::ok;
    }
    else if (installing && install.ended() && !install.cancelled())
    {
        facts = tr("Install failed");
        ink = ui::color::bad;
    }
    else
    {
        marked = false;
        if (save.size > 0.0)
            facts = format_save_size(save.size);
        if (save.created_at.size() >= 10)
            facts += (facts.empty() ? "" : " \xC2\xB7 ") + format_date(save.created_at.substr(0, 10));
    }
    ui::text_fit(list, marked ? type.medium : type.regular, facts, r.x + r.w - 24.0f, r.y + 160.0f,
                 17.0f, width - badge - 20.0f, ink, ui::Align::right);
}

void SavesScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const SavesState &saves = ctx_.store.saves().state();
    const float scroll = scroll_.value;

    list.push_clip({0.0f, 104.0f, 1920.0f, 976.0f});
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll);

    // The heading: Saves, how many match, and where they come from.
    const float title = ui::text(list, type.semibold, tr("Saves"), ui::kGutter, kTitleBaseline, 60.0f,
                                 ui::color::text);
    if (list_shown())
        ui::text(list, type.regular,
                 trn(static_cast<long>(results_.size()), "{count} save", "{count} saves"),
                 ui::kGutter + title + 24.0f, kTitleBaseline, 20.0f, ui::color::text3);
    const std::string &site = saves.listing.backend_name;
    const std::string source = site.empty() ? std::string(tr("Community saves shared by players"))
                                            : tr("Community saves from {source}", {{"source", site}});
    const float source_width =
        ui::text(list, type.regular, source, ui::kGutter, kSubtitleBaseline, 21.0f, ui::color::text2);
    // What the list leaves out, after the source.
    std::string note;
    if (saves.load == SavesLoad::ready)
    {
        for (const SaveSource &from : saves.listing.sources)
        {
            if (!from.ok && note.empty())
                note = tr("{source} could not be reached. Showing the saves that loaded.",
                          {{"source", from.name.empty() ? std::string("GitHub") : from.name}});
        }
        if (note.empty() && !saves.listing.backend_error.empty())
            note = tr("Showing the GitHub index only: {reason}",
                      {{"reason", saves.listing.backend_error}});
        if (note.empty() && saves.listing.building && !saves.listing.saves.empty())
            note = tr("Updating the list\xE2\x80\xA6");
    }
    if (!note.empty())
        ui::text_fit(list, type.regular, note, ui::kGutter + source_width + 28.0f, kSubtitleBaseline,
                     18.0f, 1920.0f - 2.0f * ui::kGutter - source_width - 28.0f, ui::color::warn);

    // .search-field, as Browse's.
    const ui::Rect field{1920.0f - ui::kGutter - kFieldWidth, kFieldY, kFieldWidth, kFieldHeight};
    ui::focus_ring(list, field, field.h * 0.5f, field_focus_.value);
    ui::surface(list, field, field.h * 0.5f, keyboard_open_ ? ui::color::field_open : ui::color::field);
    ui::icon_search(list, field.x + 34.0f, field.cy(), 24.0f, ui::color::text2);
    if (query_.empty())
    {
        ui::text_fit(list, type.regular, tr("Search saves, games or who shared them"), field.x + 60.0f,
                     field.cy() + 7.0f, 20.0f, field.w - 90.0f,
                     field_focus_.value > 0.5f ? ui::color::text2 : ui::color::text3);
    }
    else
    {
        const float w = ui::text_fit(list, type.regular, query_, field.x + 60.0f, field.cy() + 7.0f,
                                     20.0f, field.w - 90.0f, ui::color::text);
        if (keyboard_open_ && static_cast<int>(ctx_.clock * 2.0f) % 2 == 0)
            list.rounded_rect({field.x + 63.0f + w, field.cy() - 12.0f, 2.0f, 24.0f}, 1.0f,
                              ui::color::text);
    }

    if (list_shown())
    {
        // .download-tabs chips: the filter in gold, each with its count.
        const ui::Rect focused_chip = chip_rect(area_ == Area::chips ? chip_ : current_chip());
        const float slide = std::max(0.0f, focused_chip.x + focused_chip.w - (1920.0f - ui::kGutter));
        list.push_clip({ui::kGutter - 12.0f, kChipsY - 12.0f, 1920.0f - 2.0f * ui::kGutter + 24.0f,
                        kChipHeight + 24.0f});
        const int current = current_chip();
        for (int i = 0; i < static_cast<int>(chips_.size()); ++i)
        {
            const Chip &chip = chips_[static_cast<std::size_t>(i)];
            ui::Rect pill = chip_rect(i);
            pill.x -= slide;
            const bool on = i == current;
            ui::focus_ring(list, pill, pill.h * 0.5f, chip_focus_[static_cast<std::size_t>(i)]);
            if (on)
                list.rounded_rect(pill, pill.h * 0.5f, ui::color::focus_fill);
            else
                list.bordered_rect(pill, pill.h * 0.5f, ui::color::white.with_alpha(0.05f), 1.0f,
                                   ui::color::white.with_alpha(0.14f));
            // A filter with nothing for this search is dimmed (and passed over).
            const ui::Color ink = on                ? ui::color::focus_ink
                                  : chip_usable(i) ? ui::color::text
                                                   : ui::color::text3.with_alpha(0.7f);
            char number[16];
            std::snprintf(number, sizeof(number), "%d", chip.count);
            const float label = ui::text_fit(list, type.medium, chip.label, pill.x + 18.0f,
                                             pill.cy() + 6.5f, 18.0f, kGameChipWidth, ink);
            ui::text(list, type.medium, number, pill.x + 18.0f + label + 7.0f, pill.cy() + 6.5f, 18.0f,
                     ink.with_alpha(0.65f));
        }
        list.pop_clip();

        if (results_.empty())
        {
            ui::text(list, type.light, tr("No saves found"), ui::kGutter, kGridTop + 70.0f, 40.0f,
                     ui::color::text);
            ui::text(list, type.regular, tr("Try another search or filter."), ui::kGutter,
                     kGridTop + 114.0f, 20.0f, ui::color::text2);
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < static_cast<int>(results_.size()); ++i)
            {
                const ui::Rect r = card_rect(i);
                if (r.y - scroll > 1080.0f || r.y + r.h - scroll < 104.0f)
                    continue;
                const float amount = card_focus_[static_cast<std::size_t>(i)];
                if ((pass == 0) == (amount > 0.5f))
                    continue; // the focused card is drawn last, over its neighbours
                const int save = results_[static_cast<std::size_t>(i)];
                // update() refilters after a new list; until then (a panel over
                // the tab keeps it from running) an index may be past the end.
                if (save < 0 || save >= static_cast<int>(saves.listing.saves.size()))
                    continue;
                draw_card(list, saves.listing.saves[static_cast<std::size_t>(save)], game_of(save), r,
                          amount);
            }
        }
    }
    list.pop_transform();
    list.pop_clip();

    if (!list_shown())
    {
        // .empty: the badge turning while the saves load, else a word on why none show.
        const bool waiting = saves.load == SavesLoad::idle || saves.load == SavesLoad::loading ||
                             saves.building();
        const char *heading = nullptr;
        std::string body;
        if (waiting && !saves.building())
        {
            heading = tr("Loading community saves\xE2\x80\xA6");
        }
        else if (waiting)
        {
            heading = tr("Fetching the community saves\xE2\x80\xA6");
            body = tr("The first time takes a few seconds. This screen updates by itself.");
        }
        else if (saves.load == SavesLoad::unsupported)
        {
            heading = tr("This XENO STORE has no Save Data Store");
            body = tr("Update XENO STORE to browse and install community saves here.");
        }
        else if (saves.load == SavesLoad::failed)
        {
            heading = tr("Community saves could not be loaded");
            body = saves.error;
        }
        else
        {
            heading = tr("No community saves yet");
            body = tr("Saves that players share appear here.");
        }
        if (waiting)
        {
            const float size = 96.0f;
            if (ctx_.mark != 0)
                list.image(ctx_.mark, {960.0f - size * 0.5f, 440.0f - size * 0.5f, size, size},
                           hui::gfx::kFullUv, ui::color::white);
            brand::draw_rim(list, 960.0f - size * 0.5f, 440.0f - size * 0.5f, size,
                            std::fmod(ctx_.clock / 2.4f, 1.0f));
        }
        else
        {
            ui::icon_layers(list, 960.0f, 440.0f, 56.0f, ui::color::text2);
        }
        ui::text(list, type.light, heading, 960.0f, 548.0f, 34.0f, ui::color::text, ui::Align::center);
        if (!body.empty())
            ui::paragraph(list, type.regular, body, 960.0f, 594.0f, 21.0f, 1100.0f, 31.0f,
                          ui::color::text2, 3, ui::Align::center);
        if (can_retry())
        {
            const char *label = tr("Try again");
            const float width = ui::button_width(type, label);
            ui::button(list, type, {960.0f - width * 0.5f, 668.0f, width, ui::kButtonHeight}, label,
                       ui::ButtonKind::primary, retry_focus_);
        }
    }

    if (sheet_in_.value > 0.01f && !open_.empty())
        draw_details(frame, focused);

    // ---- keyboard, as Browse's ----
    if (keyboard_in_.value > 0.01f)
    {
        hui::gfx::DrawList &overlay = frame.overlay;
        frame.glass = true;
        const float in = keyboard_in_.value;
        overlay.push_opacity(in);
        overlay.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 60.0f * (1.0f - in));
        const ui::Rect panel{0.0f, kKeyboardTop, 1920.0f, 1080.0f - kKeyboardTop};
        overlay.glass(frame.glass_texture, panel, 0.0f, ui::color::white);
        overlay.rounded_rect(panel, 0.0f, ui::color::night1.with_alpha(0.78f));
        overlay.rounded_rect({0.0f, kKeyboardTop, 1920.0f, 1.5f}, 0.0f, ui::color::line);
        hui::ui::Canvas canvas{overlay, ctx_.fonts, frame.glass_texture, ctx_.clock};
        keyboard_.draw(canvas);
        overlay.pop_transform();
        overlay.pop_opacity();
    }
}

void SavesScreen::draw_details(Frame &frame, bool focused) const
{
    const SaveStore &store = ctx_.store.saves();
    const SavesState &saves = store.state();
    const SaveRow *save = saves.save(open_);
    if (save == nullptr)
        return;
    const ui::Type &type = ctx_.type;
    hui::gfx::DrawList &list = frame.overlay;
    const float in = sheet_in_.value;
    list.push_opacity(in);
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::color::scrim);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 16.0f * (1.0f - in));
    ui::modal(list, {kSheetX, kSheetY, kSheetW, kSheetH});

    // The cover, then the save: its name, game, who shared it and its badges.
    const float left = kSheetX + kSheetPad;
    const float right = kSheetX + kSheetW - kSheetPad;
    int index = -1;
    for (int i = 0; i < static_cast<int>(saves.listing.saves.size()); ++i)
        index = saves.listing.saves[static_cast<std::size_t>(i)].id == open_ ? i : index;
    draw_save_cover(ctx_, list, *save, game_of(index),
                    {left, kSheetY + kSheetPad, kSheetCover, kSheetCover}, ui::kArtRadius);
    const float x = left + kSheetCover + 48.0f;
    const float width = right - x;
    const std::string name = save->name.empty() ? std::string(tr("Untitled save")) : save->name;
    const auto lines = type.light.font->wrap(name, 44.0f, width);
    const int name_lines = std::min(2, static_cast<int>(lines.size()));
    float y = ui::paragraph(list, type.light, name, x, kSheetY + kSheetPad + 40.0f, 44.0f, width,
                            52.0f, ui::color::text, name_lines);
    y = std::max(y, kSheetY + kSheetPad + 40.0f + 52.0f * static_cast<float>(name_lines));
    ui::text_fit(list, type.regular, game_line(*save), x, y + 2.0f, 24.0f, width, ui::color::text2);
    y += 36.0f;
    const std::string shared = shared_line(*save);
    if (!shared.empty())
    {
        ui::text_fit(list, type.regular, shared, x, y, 21.0f, width, ui::color::text2);
        y += 22.0f;
    }
    // The badges: the verdict, the category, the region and the version.
    float cx = x;
    const float cy = y + 26.0f;
    cx += compat_badge(list, type, cx, cy, *save, 16.0f) + 10.0f;
    cx += ui::chip(list, type, cx, cy, save_category_label(save_category(*save)),
                   ui::color::white.with_alpha(0.08f), ui::color::text2, 16.0f) +
          10.0f;
    if (!save->region.empty())
        cx += ui::chip(list, type, cx, cy, save->region, ui::color::white.with_alpha(0.08f),
                       ui::color::text2, 16.0f) +
              10.0f;
    y = cy + 34.0f;
    // The notes, in the room left above the facts (a long name leaves less);
    // a long note pushes the facts down a little.
    float facts_y = kFactsY;
    if (!save->description.empty() && save->description != save->name)
    {
        const float first = y + 20.0f;
        const int room = static_cast<int>(std::floor((kFactsLowest - kFactsGap - first) / 31.0f)) + 1;
        const float after = ui::paragraph(list, type.regular, save->description, x, first, 21.0f, width,
                                          31.0f, ui::color::text2, std::clamp(room, 1, 4));
        facts_y = std::max(kFactsY, after - 31.0f + kFactsGap);
    }

    // The facts, under the cover.
    const std::pair<std::string, std::string> facts[] = {
        {tr("Game version"), save->game_version.empty() ? std::string(tr("Not given")) : save->game_version},
        {tr("Your version"), save->installed_version.empty()
                                 ? std::string(save->installed ? tr("Unknown") : tr("Not installed"))
                                 : save->installed_version},
        {tr("Size"), save->size > 0.0 ? format_save_size(save->size) : std::string("\xE2\x80\x94")},
        {tr("Shared on"), save->created_at.size() >= 10 ? format_date(save->created_at.substr(0, 10))
                                                        : std::string("\xE2\x80\x94")},
    };
    const float column = (right - left) / 4.0f;
    for (int i = 0; i < 4; ++i)
    {
        const float fx = left + static_cast<float>(i) * column;
        ui::text_fit(list, type.regular, facts[i].first, fx, facts_y, 18.0f, column - 24.0f,
                     ui::color::text3);
        ui::text_fit(list, type.medium, facts[i].second, fx, facts_y + 34.0f, 23.0f, column - 24.0f,
                     ui::color::text);
    }
    // What the save needs from the installed game.
    const SaveCompat compat = save_compat(*save);
    const ui::Color ink = compat_ink(compat);
    const bool verdict = compat != SaveCompat::not_installed && compat != SaveCompat::unknown;
    const float compat_y = facts_y + 92.0f;
    list.circle(left + 12.0f, compat_y - 8.0f, 12.0f, ink.with_alpha(0.18f));
    if (compat == SaveCompat::match || compat == SaveCompat::older_save)
        ui::icon_check(list, left + 12.0f, compat_y - 8.0f, 13.0f, ink, 2.5f);
    else
        list.circle(left + 12.0f, compat_y - 8.0f, 3.5f, ink);
    ui::paragraph(list, type.regular, save_compat_sentence(*save), left + 38.0f, compat_y, 22.0f,
                  right - left - 38.0f, 31.0f, verdict ? ink : ui::color::text, 2);

    // Install: the profile, the button, then the job's progress and outcome.
    const SaveInstall &install = store.install();
    const bool mine = install.save_id == open_;
    const float status_y = kButtonsY - 44.0f;
    if (mine && install.active)
    {
        // A step without a size (installing, Garlic Save Manager's steps)
        // sweeps instead of falling back to 0%.
        const SaveJob &job = install.job;
        const bool measured = job.total > 0.0;
        const float fraction =
            measured ? static_cast<float>(std::clamp(job.done / job.total, 0.0, 1.0)) : 0.0f;
        std::string step = install.cancelling ? std::string(tr("Cancelling\xE2\x80\xA6"))
                           : job.message.empty() ? std::string(tr("Starting the install\xE2\x80\xA6"))
                                                 : std::string(tr(std::string_view(job.message)));
        ui::text_fit(list, type.medium, step, left, status_y - 66.0f, 22.0f, right - left - 200.0f,
                     ui::color::text);
        const ui::Rect bar{left, status_y - 46.0f, right - left, 10.0f};
        if (measured)
        {
            ui::text(list, type.regular, i18n::percent(fraction * 100.0f), right, status_y - 66.0f, 20.0f,
                     ui::color::text2, ui::Align::right);
            ui::progress_bar(list, bar, fraction, ui::color::action);
            ui::text(list, type.regular, format_save_size(job.done) + " / " + format_save_size(job.total), left,
                     status_y - 6.0f, 18.0f, ui::color::text2);
        }
        else
        {
            busy_bar(list, bar, ctx_.clock);
        }
    }
    else if (mine && install.ended())
    {
        const bool done = install.succeeded();
        const std::string outcome = install.outcome();
        const ui::Color tone = done ? ui::color::ok : install.cancelled() ? ui::color::text2 : ui::color::bad;
        list.circle(left + 16.0f, status_y - 52.0f, 16.0f, tone);
        if (done)
            ui::icon_check(list, left + 16.0f, status_y - 52.0f, 16.0f, ui::color::night1, 3.0f);
        else
            ui::text(list, type.semibold, "!", left + 16.0f, status_y - 45.0f, 20.0f, ui::color::night1,
                     ui::Align::center);
        const float after = ui::paragraph(list, type.medium, outcome, left + 46.0f, status_y - 44.0f, 22.0f,
                                          right - left - 46.0f, 30.0f, done ? ui::color::text : tone, 2);
        // Where the save it replaced went (its file; the folder is the
        // profile's), and what the engine noted.
        std::string detail;
        if (done && !install.job.pre_backup.empty())
        {
            const std::string &path = install.job.pre_backup;
            const std::size_t slash = path.find_last_of('/');
            detail = tr("Your previous save was backed up as {file}",
                        {{"file", slash == std::string::npos ? path : path.substr(slash + 1)}});
        }
        if (done && !install.job.note.empty())
            detail += (detail.empty() ? "" : " \xC2\xB7 ") + install.job.note;
        if (!detail.empty())
            ui::text_fit(list, type.regular, detail, left + 46.0f, after + 2.0f, 18.0f,
                         right - left - 46.0f, ui::color::text2);
    }

    const std::vector<Control> buttons = controls();
    const bool other = install.active && !mine;
    // Beside the buttons, clear of the profile list that opens above them:
    // the backup (and for whom, when no profile field says it), or the
    // install that has to finish first.
    if (std::find(buttons.begin(), buttons.end(), Control::install) != buttons.end())
    {
        const bool field = buttons.front() == Control::profile;
        const SaveProfile *profile = find_profile(saves.profiles, user_);
        std::string line;
        if (other)
            line = tr("Another save is installing ({save}). Wait for it to finish.",
                      {{"save", install.name.empty() ? install.game : install.name}});
        else if (!field && profile != nullptr)
            line = tr("Installs for {profile}. Your current save of this game is backed up first.",
                      {{"profile", profile_label(*profile)}});
        else
            line = tr("Your current save of this game is backed up first.");
        const ui::Rect last = control_rect(static_cast<int>(buttons.size()) - 1);
        const float note_x = last.x + last.w + 36.0f;
        const float note_w = right - note_x;
        const int lines = std::min(2, static_cast<int>(type.regular.font->wrap(line, 20.0f, note_w).size()));
        const float middle = kButtonsY + ui::kButtonHeight * 0.5f;
        ui::paragraph(list, type.regular, line, note_x, lines > 1 ? middle - 7.0f : middle + 7.0f, 20.0f,
                      note_w, 28.0f, ui::color::text2, 2);
    }
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i)
    {
        const Control control = buttons[static_cast<std::size_t>(i)];
        const ui::Rect r = control_rect(i);
        const float focus = focused && !picker_.open && control == control_ ? 1.0f : 0.0f;
        switch (control)
        {
        case Control::profile:
        {
            std::string value;
            for (const SaveProfile &profile : saves.profiles)
            {
                if (profile.id == user_)
                    value = profile_label(profile);
            }
            ui::field_label(list, type, r, tr("Profile"));
            ui::select_field(list, type, r, value, focus, picker_.open);
            break;
        }
        case Control::install:
            ui::button(list, type, r, mine && install.ended() ? tr("Install again") : tr("Install"),
                       ui::ButtonKind::primary, focus, !other, 22.0f);
            break;
        case Control::cancel:
            ui::button(list, type, r, tr("Cancel"), ui::ButtonKind::secondary, focus, !install.cancelling,
                       22.0f);
            break;
        case Control::done:
            ui::button(list, type, r, tr("Done"), ui::ButtonKind::primary, focus, true, 22.0f);
            break;
        case Control::back:
            ui::button(list, type, r, tr("Back"), ui::ButtonKind::secondary, focus, true, 22.0f);
            break;
        }
    }
    list.pop_transform();
    list.pop_opacity();

    // The profile list, over the sheet.
    if (picker_.in.value > 0.01f && !buttons.empty() && buttons.front() == Control::profile)
    {
        std::vector<std::string> options;
        int selected = 0;
        for (int i = 0; i < static_cast<int>(saves.profiles.size()); ++i)
        {
            options.push_back(profile_label(saves.profiles[static_cast<std::size_t>(i)]));
            selected = saves.profiles[static_cast<std::size_t>(i)].id == user_ ? i : selected;
        }
        // It opens above the field's label, which stays in view.
        ui::Rect trigger = control_rect(0);
        trigger.y -= 30.0f;
        trigger.h += 30.0f;
        frame.glass = true;
        ui::select_menu(list, type, trigger, options, selected, picker_.active, picker_.in.value,
                        frame.glass_texture);
    }
}

std::vector<hui::ui::Hint> SavesScreen::hints() const
{
    if (keyboard_open_)
        return {{hui::ui::Button::square, tr("Delete")},
                {hui::ui::Button::triangle, tr("Space")},
                {hui::ui::Button::circle, tr("Close")}};
    if (picker_.open)
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Close")}};
    if (!open_.empty())
        return {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Back")}};
    if (area_ == Area::grid)
        return {{hui::ui::Button::cross, tr("View save")},
                {hui::ui::Button::triangle, tr("Search")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (area_ == Area::chips)
        return {{hui::ui::Button::cross, tr("Select")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (area_ == Area::retry)
        return {{hui::ui::Button::cross, tr("Try again")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    // No saves to search yet (loading, building, none).
    if (!list_shown())
        return {{hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (!query_.empty())
        return {{hui::ui::Button::cross, tr("Type")},
                {hui::ui::Button::square, tr("Clear")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    return {{hui::ui::Button::cross, tr("Type")}, {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
