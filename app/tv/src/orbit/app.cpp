// Orbit Store TV app - The app: top bar, tabs, the game page, status and dialogs.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.

#include "orbit/brand.hpp"
#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

constexpr const char *kTabs[] = {/* i18n */ "Discover", /* i18n */ "Browse", /* i18n */ "Saves", /* i18n */ "Library", /* i18n */ "Downloads"}; // by tab::
constexpr int kTabCount = tab::count;
constexpr int kSearchItem = kTabCount;
constexpr int kSettingsItem = kTabCount + 1;
constexpr int kTopItems = kTabCount + 2; // the tabs, search, settings
// The browser's .header: 5.2 rem tall, everything on its centre line.
constexpr float kTopY = 52.0f;
constexpr float kTabSize = 22.0f; // nav buttons: 1.1 rem
constexpr float kTabPad = 22.0f;  // 1.1 rem each side
constexpr float kTabHeight = 46.0f;
constexpr float kTabGap = 6.0f;
constexpr float kIcon = 56.0f; // .top-icon: 2.8 rem
constexpr float kSearchX = 1724.0f;
constexpr float kSettingsX = 1792.0f;
constexpr const char *kBrand = "XENO STORE";
// How long the opening screen shows before "Orbit isn't running".
constexpr float kOpeningGrace = 2.5f;

// Where the BETA label starts and how wide it is.
float beta_x(const ui::Type &type)
{
    return ui::kGutter + 40.0f + 14.0f + brand::wordmark_width(type, kBrand, 22.0f) + 14.0f;
}
float beta_width(const ui::Type &type)
{
    return type.semibold.measure(tr("BETA"), 13.0f) + 4.0f + 2.0f * 7.6f;
}

// The brand, as the web's .brand: the round badge in a gold ring, the
// wordmark, and the BETA label outlined in gold. x is the brand's left edge.
void draw_brand(hui::gfx::DrawList &list, const ui::Type &type, std::uint32_t badge, float x)
{
    if (badge != 0)
    {
        list.glow({x - 2.0f, kTopY - 22.0f, 44.0f, 44.0f}, 22.0f, 12.0f,
                  ui::color::gold.with_alpha(0.22f));
        list.ring(x + 20.0f, kTopY, 24.5f, 2.0f, ui::color::gold);
        list.image(badge, {x - 2.0f, kTopY - 22.0f, 44.0f, 44.0f}, hui::gfx::kFullUv,
                   ui::color::white);
    }
    const float name = brand::draw_wordmark(list, type, kBrand, x + 54.0f, kTopY + 8.0f, 22.0f);
    const ui::Rect beta{x + 54.0f + name + 14.0f, kTopY - 12.5f, beta_width(type), 25.0f};
    list.bordered_rect(beta, 4.0f, ui::color::gold_soft, 1.0f, ui::color::gold_line);
    ui::text(list, type.semibold, tr("BETA"), beta.cx(), kTopY + 4.7f, 13.0f, ui::color::action_hi,
             ui::Align::center);
}

ui::Rect tab_rect(const ui::Type &type, int index)
{
    // The nav starts 3 rem after the brand button (whose padding ends 0.9 rem
    // after the label).
    float x = beta_x(type) + beta_width(type) + 18.0f + 60.0f;
    for (int i = 0; i < index; ++i)
        x += type.regular.measure(tr(kTabs[i]), kTabSize) + 2.0f * kTabPad + kTabGap;
    return {x, kTopY - kTabHeight * 0.5f,
            type.regular.measure(tr(kTabs[index]), kTabSize) + 2.0f * kTabPad, kTabHeight};
}

bool starting_now(const Snapshot &state)
{
    return state.starting == Starting::starting || state.starting == Starting::waiting;
}

int active_jobs(const Snapshot &state)
{
    int count = 0;
    for (const Job &job : state.jobs)
    {
        if (!job_inactive(job) && job.status != "error")
            ++count;
    }
    return count;
}

} // namespace

App::App(Context &context)
    : ctx_(context), discover_(context), browse_(context), saves_(context), library_(context),
      downloads_(context), game_(context), settings_(context)
{
    top_focus_.resize(kTopItems);
    backdrop_in_.snap(1.0f);
}

Screen &App::screen()
{
    switch (tab_)
    {
    case tab::discover:
        return discover_;
    case tab::saves:
        return saves_;
    case tab::library:
        return library_;
    case tab::downloads:
        return downloads_;
    default:
        return browse_;
    }
}

const Screen &App::screen() const
{
    return const_cast<App *>(this)->screen();
}

bool App::content_blocked() const
{
    // Library, Downloads and Saves don't depend on the download sources or the catalogue.
    if (tab_ == tab::downloads || tab_ == tab::library || tab_ == tab::saves)
        return false;
    const Snapshot &state = ctx_.store.state();
    return state.setup_needed() || (state.have_catalog && state.games.empty());
}

void App::show_tab(int tab)
{
    tab_ = std::clamp(tab, 0, kTabCount - 1);
    game_open_ = false;
    if (settings_open_)
        settings_.close();
    settings_open_ = false;
    area_ = Area::content;
    screen().focus();
}

void App::open_game(const std::string &game_id)
{
    game_.open(game_id);
    game_open_ = true;
    game_in_.snap(0.0f);
}

void App::open_settings(SettingsScreen::Section section, bool content)
{
    settings_open_ = true;
    game_open_ = false;
    settings_.open(section, content);
}

bool App::update_badge() const
{
    const Snapshot &state = ctx_.store.state();
    const std::string running = app_release_version(ctx_.app_version);
    const bool tv = state.have_tv_app && !state.tv_app.latest_version.empty() && !running.empty() &&
                    starter::compare_versions(state.tv_app.latest_version, running) > 0;
    return tv || (state.have_service_update && state.service.update_available);
}

void App::show_toast(std::string text, float seconds, ToastKind kind, std::string detail)
{
    toast_ = std::move(text);
    toast_detail_ = std::move(detail);
    toast_time_ = seconds;
    toast_length_ = seconds;
    toast_kind_ = kind;
}

// XENO STORE: a save install that ended while its details were not on screen
// (closed, another tab, a game page or App settings over them) says how it
// ended: the save, then the outcome its details would show. The card keeps
// the outcome until those details are seen.
void App::tell_save_install()
{
    const SaveInstall &install = ctx_.store.saves().install();
    if (!install.ended() || install.run == save_told_)
        return;
    save_told_ = install.run;
    if (tab_ == tab::saves && !game_open_ && !settings_open_ && !provider_open_ &&
        saves_.showing(install.save_id))
        return;
    const std::string name = !install.name.empty() ? install.name
                             : !install.game.empty() ? install.game
                                                     : std::string(tr("Untitled save"));
    const ToastKind kind = install.succeeded()   ? ToastKind::done
                           : install.cancelled() ? ToastKind::note
                                                 : ToastKind::problem;
    show_toast(name, install.succeeded() ? 5.0f : 9.0f, kind, install.outcome());
}

void App::handle_intent(const Intent &intent, Feedback &feedback)
{
    switch (intent.kind)
    {
    case Intent::Kind::provider_browser:
        provider_open_ = true;
        provider_start_pending_ = ctx_.store.browser_action().busy &&
                                  !ctx_.store.state().browser.active;
        provider_focus_ = 1;
        provider_seen_ = ctx_.store.state().browser.id;
        break;
    case Intent::Kind::top_bar:
        area_ = Area::top;
        top_index_ = tab_;
        feedback.play(hui::audio::Cue::focus);
        break;
    case Intent::Kind::open_game:
        open_game(intent.id);
        feedback.play(hui::audio::Cue::open, 1.0f, 0.0f, 0.7f);
        break;
    case Intent::Kind::close:
        if (settings_open_)
        {
            settings_open_ = false;
            settings_.close();
            break;
        }
        game_open_ = false;
        if (tab_ == tab::discover)
            discover_.return_to(game_.game_id());
        else if (tab_ == tab::browse)
            browse_.return_to(game_.game_id());
        break;
    case Intent::Kind::show_job:
        game_open_ = false;
        tab_ = tab::downloads;
        area_ = Area::content;
        downloads_.show_job(intent.id);
        break;
    case Intent::Kind::show_tab:
        show_tab(intent.tab);
        feedback.play(hui::audio::Cue::tab);
        break;
    case Intent::Kind::show_library:
        game_open_ = false;
        tab_ = tab::library;
        area_ = Area::content;
        library_.focus();
        library_.show_game(intent.id, intent.detail);
        break;
    case Intent::Kind::show_saves:
        // XENO STORE: a game page's Community saves.
        game_open_ = false;
        tab_ = tab::saves;
        area_ = Area::content;
        saves_.focus();
        saves_.show_game(intent.id);
        feedback.play(hui::audio::Cue::tab);
        break;
    default:
        break;
    }
    if (!intent.toast.empty())
    {
        show_toast(intent.toast);
    }
}

void App::update(const InputFrame &input, float dt, Feedback &feedback)
{
    ctx_.clock += dt;
    ctx_.store.tick();
    const Snapshot &state = ctx_.store.state();
    ctx_.art.set_token(state.token);
    ctx_.art.tick();
    ctx_.store.set_fast(false);
    toast_time_ = std::max(0.0f, toast_time_ - dt);
    status_age_ += dt;

    const InputFrame idle{};
    const bool ready = state.have_system && !state.outdated && state.link != Link::connecting &&
                       (state.have_catalog || state.setup_needed());
    if (!ready)
    {
        // XENO STORE: an outdated store holds the port. Left and right choose
        // a button, Cross uses it; the screen clears by itself after an update.
        if (state.outdated)
        {
            StatusButton buttons[2];
            const int count = status_buttons(state, buttons);
            if (count == 0)
                return;
            if (input.nav == Direction::left)
                status_focus_ = 0;
            else if (input.nav == Direction::right)
                status_focus_ = count - 1;
            status_focus_ = std::clamp(status_focus_, 0, count - 1);
            if (input.is_pressed(Action::confirm))
            {
                feedback.play(hui::audio::Cue::launch);
                status_note_.clear();
                if (buttons[status_focus_] == StatusButton::replace)
                    ctx_.store.replace_outdated();
                else if (!ctx_.open_browser())
                    status_note_ = tr("The browser could not be opened.");
            }
            return;
        }
        // The opening and offline screens move on by themselves. Once Orbit is
        // known not to be running, Cross asks the store to start it again.
        if (status_offline(state) && ctx_.store.can_start() && input.is_pressed(Action::confirm))
        {
            ctx_.store.start_orbit();
            feedback.play(hui::audio::Cue::launch);
        }
        return;
    }

    // A finished restart of the download service, wherever the player is.
    if (state.restart == Restart::done)
    {
        bool paused = false;
        for (const Job &job : state.jobs)
            paused = paused || job.status == "paused";
        show_toast(paused ? tr("Download service {version} is running. Resume your downloads in "
                               "Downloads.",
                               {{"version", state.restart_detail}})
                          : tr("Download service {version} is running",
                               {{"version", state.restart_detail}}),
                   4.5f);
        ctx_.store.dismiss_restart();
    }
    // An update for either part shows once, then waits under App settings.
    if (!update_told_ && update_badge() && !settings_open_)
    {
        update_told_ = true;
        show_toast(tr("An update is available in App settings"), 4.5f, ToastKind::update);
    }
    tell_save_install();

    // Recover active verification after reopening the native app. Historical
    // terminal results never reopen themselves or undo a dismissal.
    if (state.have_browser && state.browser.active && state.browser.id != provider_seen_)
    {
        provider_seen_ = state.browser.id;
        // A locally requested start may return after the user closed this
        // panel. Record its ID without undoing that dismissal.
        if (!provider_start_pending_)
        {
            provider_open_ = true;
            provider_focus_ = 1;
        }
    }
    if (provider_start_pending_ && !ctx_.store.browser_action().busy)
        provider_start_pending_ = false;
    if (provider_open_)
    {
        ctx_.store.set_fast(true);
        update_provider(input, feedback);
    }
    else if (settings_open_)
    {
        handle_intent(settings_.update(input, dt, feedback, true), feedback);
    }
    else if (game_open_)
    {
        game_in_.target = 1.0f;
        handle_intent(game_.update(input, dt, feedback, true), feedback);
    }
    else
    {
        // L1 / R1 switch tabs from anywhere on a tab, as on the console's own apps.
        if (!screen().modal() &&
            (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next)))
        {
            const int next = tab_ + (input.is_pressed(Action::page_next) ? 1 : -1);
            if (next >= 0 && next < kTabCount)
            {
                tab_ = next;
                top_index_ = tab_;
                screen().focus();
                feedback.play(hui::audio::Cue::tab);
            }
            else
            {
                refuse(feedback, input);
            }
        }

        if (area_ == Area::top)
        {
            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = top_index_ + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next < kTopItems)
                {
                    top_index_ = next;
                    if (top_index_ < kTabCount && top_index_ != tab_)
                    {
                        // Moving along the tabs shows each one, like the console's store.
                        tab_ = top_index_;
                        screen().focus();
                        feedback.play(hui::audio::Cue::tab);
                    }
                    else
                    {
                        feedback.play(hui::audio::Cue::focus);
                    }
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            else if (input.nav == Direction::down)
            {
                area_ = Area::content;
                screen().focus();
                feedback.play(hui::audio::Cue::focus);
            }
            else if (input.nav == Direction::up)
            {
                refuse(feedback, input);
            }
            if (input.is_pressed(Action::confirm))
            {
                if (top_index_ < kTabCount)
                {
                    area_ = Area::content;
                    screen().focus();
                    feedback.play(hui::audio::Cue::select);
                }
                else if (top_index_ == kSearchItem)
                {
                    tab_ = tab::browse;
                    area_ = Area::content;
                    browse_.focus();
                    browse_.start_search();
                    feedback.play(hui::audio::Cue::modal_open);
                }
                else
                {
                    open_settings();
                    feedback.play(hui::audio::Cue::modal_open);
                }
            }
            screen().update(idle, dt, feedback, false);
        }
        else if (content_blocked())
        {
            if (input.nav == Direction::up)
            {
                Intent intent;
                intent.kind = Intent::Kind::top_bar;
                handle_intent(intent, feedback);
            }
            else if (input.is_pressed(Action::confirm))
            {
                open_settings(SettingsScreen::Section::sources, true);
                feedback.play(hui::audio::Cue::modal_open);
            }
            screen().update(idle, dt, feedback, false);
        }
        else
        {
            handle_intent(screen().update(input, dt, feedback, true), feedback);
        }
    }

    if (tab_ != last_tab_)
    {
        if (last_tab_ == tab::library)
            library_.blur();
        last_tab_ = tab_;
    }

    // ---- animation ----
    const std::string wanted = game_open_          ? game_.game_id()
                               : content_blocked() ? std::string()
                                                   : screen().backdrop_game();
    // On Discover the art waits for the focus to rest for 300 ms, so holding
    // the D-pad doesn't fetch a banner for every cover it passes (main 0.5.1).
    // A game page shows its art at once.
    if (wanted != settling_)
    {
        settling_ = wanted;
        settled_for_ = 0.0f;
    }
    else
    {
        settled_for_ += dt;
    }
    const bool wait = !game_open_ && tab_ == tab::discover && !backdrop_.empty();
    if (wanted != backdrop_ && (!wait || settled_for_ >= 0.3f))
    {
        previous_backdrop_ = backdrop_;
        backdrop_ = wanted;
        backdrop_in_.snap(0.0f);
    }
    backdrop_in_.target = 1.0f;
    backdrop_in_.update(dt, 4.0f);
    game_in_.update(dt, 10.0f);
    for (int i = 0; i < kTopItems; ++i)
    {
        top_focus_[static_cast<std::size_t>(i)].target =
            area_ == Area::top && !game_open_ && !settings_open_ && i == top_index_ ? 1.0f : 0.0f;
        top_focus_[static_cast<std::size_t>(i)].update(dt, 16.0f);
    }
}

void App::draw_top_bar(hui::gfx::DrawList &list) const
{
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    // The brand: badge, name and the BETA label, as on the web.
    draw_brand(list, type, ctx_.mark, ui::kGutter);

    // Tabs: text buttons, the current one in a pale pill over a gold bar.
    const int active = active_jobs(state);
    char count[16];
    std::snprintf(count, sizeof(count), "%d", active);
    // .queue-count sits inside the Downloads tab, which widens for it.
    const float count_width = std::max(24.0f, type.semibold.measure(count, 14.0f) + 18.0f);
    for (int i = 0; i < kTabCount; ++i)
    {
        ui::Rect r = tab_rect(type, i);
        const bool counted = i == tab::downloads && active > 0;
        if (counted)
            r.w += 10.0f + count_width;
        const bool current = i == tab_;
        const float focus = top_focus_[static_cast<std::size_t>(i)].value;
        ui::focus_ring(list, r, r.h * 0.5f, focus);
        if (current)
        {
            list.bordered_rect(r, r.h * 0.5f, ui::color::white.with_alpha(0.1f), 1.0f,
                               ui::color::white.with_alpha(0.16f));
            list.rounded_rect({r.x + kTabPad, r.y + r.h - 7.0f, r.w - 2.0f * kTabPad, 3.0f}, 1.5f,
                              ui::color::gold);
        }
        ui::text(list, type.regular, tr(kTabs[i]), r.x + kTabPad, kTopY + 8.0f, kTabSize,
                 current || focus > 0.5f ? ui::color::text : ui::color::text2);
        if (counted)
        {
            const ui::Rect pill{r.x + r.w - kTabPad - count_width, kTopY - 11.0f, count_width,
                                22.0f};
            list.rounded_rect(pill, 11.0f, ui::color::action);
            ui::text(list, type.semibold, count, pill.cx(), kTopY + 5.0f, 14.0f,
                     ui::color::action_ink, ui::Align::center);
        }
    }

    // Round tools on the right; each names itself when it has the focus.
    const struct
    {
        float x;
        const char *label;
        int index;
    } icons[] = {{kSearchX, tr("Search"), kSearchItem}, {kSettingsX, tr("App settings"), kSettingsItem}};
    for (const auto &icon : icons)
    {
        const float focus = top_focus_[static_cast<std::size_t>(icon.index)].value;
        ui::focus_ring(list, {icon.x - kIcon * 0.5f, kTopY - kIcon * 0.5f, kIcon, kIcon},
                       kIcon * 0.5f, focus);
        list.circle(icon.x, kTopY, kIcon * 0.5f, ui::color::white.with_alpha(0.08f));
        if (icon.index == kSearchItem)
        {
            ui::icon_search(list, icon.x, kTopY, 26.0f, ui::color::text);
        }
        else
        {
            ui::icon_settings(list, icon.x, kTopY, 26.0f, ui::color::text);
            if (update_badge())
            {
                // An update is waiting under App settings.
                list.circle(icon.x + 19.0f, kTopY - 19.0f, 8.5f, ui::color::night1);
                list.circle(icon.x + 19.0f, kTopY - 19.0f, 6.5f, ui::color::action_hi);
            }
        }
        if (focus > 0.01f)
        {
            list.push_opacity(focus);
            const float width = type.regular.measure(icon.label, 15.0f) + 26.0f;
            const ui::Rect tag{
                std::min(icon.x - width * 0.5f, 1920.0f - ui::kGutter - width + 28.0f),
                kTopY + kIcon * 0.5f + 11.0f, width, 28.0f};
            list.rounded_rect(tag, 14.0f, ui::color::popover);
            ui::text(list, type.regular, icon.label, tag.cx(), tag.cy() + 5.4f, 15.0f,
                     ui::color::text, ui::Align::center);
            list.pop_opacity();
        }
    }
}

// The offline screen shows once Orbit has had time to answer, or as soon as an
// attempt to start it has ended.
bool App::status_offline(const Snapshot &state) const
{
    const bool restarting = state.restart == Restart::stopping || state.restart == Restart::starting;
    return state.link == Link::offline && !starting_now(state) && !restarting &&
           (status_age_ > kOpeningGrace || state.starting != Starting::idle);
}

int App::status_buttons(const Snapshot &state, StatusButton (&out)[2]) const
{
    const SettingResult &result = ctx_.store.setting_result();
    // Only a store that names itself can be replaced or updated; nothing while
    // a replacement is under way.
    if (state.outdated_version.empty() || state.restart == Restart::stopping ||
        (result.busy && result.setting == Setting::service_replace))
        return 0;
    int count = 0;
    if (ctx_.store.can_start())
        out[count++] = StatusButton::replace;
    if (ctx_.open_browser)
        out[count++] = StatusButton::browser;
    return count;
}

// XENO STORE: something answers on the port without this app's API.
void App::draw_outdated(hui::gfx::DrawList &list, const Snapshot &state) const
{
    const ui::Type &type = ctx_.type;
    const bool known = !state.outdated_version.empty();
    const std::string title =
        known ? tr("XENO STORE {version} is too old for this app", {{"version", state.outdated_version}})
              : std::string(tr("Another program is using XENO STORE\xE2\x80\x99s port"));
    const char *body =
        !known ? tr("Close the program that uses port 9095, or restart your PS5 so this app can start "
                    "XENO STORE.")
        : ctx_.store.can_start()
            ? tr("Update it in the browser version under Settings \xE2\x86\x92 Updates, or replace it "
                 "with the copy in this app.")
            : tr("Update it in the browser version under Settings \xE2\x86\x92 Updates. This screen "
                 "reconnects by itself.");
    ui::text(list, type.light, title, 960.0f, 590.0f, 52.0f, ui::color::text, ui::Align::center);
    ui::text(list, type.regular, body, 960.0f, 646.0f, 26.0f, ui::color::text2, ui::Align::center);
    const SettingResult &result = ctx_.store.setting_result();
    std::string detail;
    if (state.restart == Restart::stopping || (result.busy && result.setting == Setting::service_replace))
        detail = tr("Stopping the download service\xE2\x80\xA6");
    else if (result.setting == Setting::service_replace && !result.error.empty())
        detail = result.error;
    else
        detail = status_note_;
    if (!detail.empty())
        ui::text(list, type.regular, detail, 960.0f, 694.0f, 21.0f, ui::color::text3,
                 ui::Align::center);
    StatusButton buttons[2];
    const int count = status_buttons(state, buttons);
    const char *labels[2] = {};
    float widths[2] = {};
    float total = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        labels[i] = buttons[i] == StatusButton::replace ? tr("Replace it now") : tr("Open browser version");
        widths[i] = ui::button_width(type, labels[i]);
        total += widths[i] + (i > 0 ? 24.0f : 0.0f);
    }
    float x = 960.0f - total * 0.5f;
    const int focus = std::clamp(status_focus_, 0, std::max(0, count - 1));
    for (int i = 0; i < count; ++i)
    {
        ui::button(list, type, {x, 760.0f, widths[i], ui::kButtonHeight}, labels[i],
                   i == 0 ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
                   i == focus ? 1.0f : 0.0f);
        x += widths[i] + 24.0f;
    }
}

void App::draw_status(Frame &frame) const
{
    hui::gfx::DrawList &list = frame.scene;
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    const bool offline = status_offline(state);
    const bool replacing = state.restart == Restart::stopping || state.restart == Restart::starting;
    const bool dim = state.outdated ? !replacing : offline;
    const float size = 160.0f;
    const float x = 960.0f - size * 0.5f;
    const float y = 330.0f - size * 0.5f;
    // A breathing blue glow behind the badge, and the gold rim turning round it
    // once every 2.4 s, as the web's brand badge does.
    const float breathe = 0.5f + 0.5f * std::sin(ctx_.clock * 2.6f);
    list.glow({x + 20.0f, y + 20.0f, size - 40.0f, size - 40.0f}, size * 0.5f, 120.0f,
              ui::color::accent.with_alpha((dim ? 0.08f : 0.16f) + 0.1f * breathe));
    const float in = std::clamp(ctx_.clock / 0.7f, 0.0f, 1.0f);
    list.push_opacity(in);
    list.push_transform(0.88f + 0.12f * in, 960.0f, 330.0f, 0.0f, 0.0f);
    const std::uint32_t badge = ctx_.loader_mark != 0 ? ctx_.loader_mark : ctx_.mark;
    if (badge != 0)
        list.image(badge, {x, y, size, size}, hui::gfx::kFullUv,
                   ui::color::white.with_alpha(dim ? 0.55f : 1.0f));
    if (!dim)
        brand::draw_rim(list, x, y, size, std::fmod(ctx_.clock / 2.4f, 1.0f));
    list.pop_transform();
    list.pop_opacity();

    if (state.outdated)
    {
        draw_outdated(list, state);
        return;
    }
    if (!offline)
    {
        if (starting_now(state) || replacing)
        {
            ui::text(list, type.light, tr("Starting XENO STORE\xE2\x80\xA6"), 960.0f, 590.0f, 44.0f,
                     ui::color::text, ui::Align::center);
            ui::text(list, type.regular,
                     tr("XENO STORE keeps running in the background after you close this app."), 960.0f,
                     646.0f, 26.0f, ui::color::text2, ui::Align::center);
            return;
        }
        ui::text(list, type.light, tr("Opening XENO STORE\xE2\x80\xA6"), 960.0f, 590.0f, 44.0f,
                 ui::color::text, ui::Align::center);
        return;
    }
    // A replacement of an outdated store that could not start this app's copy
    // reads like a start of its own.
    const bool restart_ended = state.restart == Restart::manual || state.restart == Restart::failed;
    const Starting shown = state.starting != Starting::idle   ? state.starting
                           : state.restart == Restart::manual ? Starting::no_loader
                           : state.restart == Restart::failed ? Starting::failed
                                                              : Starting::idle;
    const char *title = shown == Starting::failed ? tr("XENO STORE didn\xE2\x80\x99t start")
                                                  : tr("XENO STORE isn\xE2\x80\x99t running");
    const char *body = tr("Start XENO STORE from your payload manager. This screen reconnects by itself.");
    if (shown == Starting::no_loader)
        body = tr("Start XENO STORE from your payload manager, or run your jailbreak again so this app "
               "can start it.");
    else if (shown == Starting::failed)
        body = tr("Check that XENO STORE is set up in your payload manager, then try again.");
    ui::text(list, type.light, title, 960.0f, 590.0f, 52.0f, ui::color::text, ui::Align::center);
    ui::text(list, type.regular, body, 960.0f, 646.0f, 26.0f, ui::color::text2, ui::Align::center);
    // The connection error itself stays as the system reports it.
    const std::string detail =
        !state.start_detail.empty()                       ? state.start_detail
        : restart_ended && !state.restart_detail.empty() ? state.restart_detail
            : tr("Waiting for XENO STORE on this console ({detail})",
                 {{"detail", state.link_error.empty() ? std::string(tr("no answer"))
                                                      : state.link_error}});
    ui::text(list, type.regular, detail, 960.0f, 694.0f, 21.0f, ui::color::text3,
             ui::Align::center);
    if (ctx_.store.can_start())
    {
        const char *label = tr("Try again");
        const float width = ui::button_width(type, label);
        ui::button(list, type, {960.0f - width * 0.5f, 760.0f, width, ui::kButtonHeight}, label,
                   ui::ButtonKind::primary, 1.0f);
    }
}

void App::draw_blocked(hui::gfx::DrawList &list) const
{
    const ui::Type &type = ctx_.type;
    const Snapshot &state = ctx_.store.state();
    const bool setup = state.setup_needed() && !state.sources.acknowledged;
    const char *title = setup                           ? tr("Finish setting up XENO STORE")
                        : state.sources.enabled.empty() ? tr("No sources enabled")
                                                        : tr("No games from your selected sources");
    const char *body = setup ? tr("Choose where XENO STORE gets your downloads and accept the download "
                               "notice. It takes a moment, and you can change it anytime.")
                             : tr("Choose another source in App settings. This screen updates by "
                               "itself.");
    ui::text(list, type.light, title, ui::kGutter, 380.0f, 60.0f, ui::color::text);
    ui::paragraph(list, type.regular, body, ui::kGutter, 446.0f, 27.0f, 980.0f, 40.0f,
                  ui::color::text2, 3);
    ui::button(list, type,
               {ui::kGutter, 560.0f, ui::button_width(type, tr("Choose sources")), ui::kButtonHeight},
               tr("Choose sources"), ui::ButtonKind::primary, area_ == Area::content ? 1.0f : 0.0f);
}

void App::draw_game_header(hui::gfx::DrawList &list) const
{
    // A game page is its own screen: Back and the brand, no tabs or tools.
    const ui::Type &type = ctx_.type;
    list.circle(ui::kGutter + kIcon * 0.5f, kTopY, kIcon * 0.5f,
                ui::color::white.with_alpha(0.08f));
    ui::icon_back(list, ui::kGutter + kIcon * 0.5f, kTopY, 24.0f, ui::color::text);
    draw_brand(list, type, ctx_.mark, ui::kGutter + kIcon + 36.0f);
}

void App::draw(Frame &frame) const
{
    const Snapshot &state = ctx_.store.state();
    // The web UI's canvas: deep blue with its slow aurora clouds. The shader's
    // first colour is the top: the web's glow spot sits there, over --bg0.
    frame.backdrop.mode = hui::gfx::BackdropMode::aurora;
    frame.backdrop.colors[0] = ui::color::glow;
    frame.backdrop.colors[1] = ui::color::night0;
    frame.backdrop.colors[2] = ui::color::aurora1;
    frame.backdrop.colors[3] = ui::color::aurora2;
    frame.backdrop.params[0] = 0.08f;
    frame.backdrop.params[1] = 0.0f;
    frame.backdrop.params[2] = 1.0f;
    frame.backdrop.time = ctx_.clock;

    hui::gfx::DrawList &scene = frame.scene;
    const bool ready = state.have_system && !state.outdated && state.link != Link::connecting &&
                       (state.have_catalog || state.setup_needed());
    if (!ready)
    {
        draw_status(frame);
        return;
    }

    // ---- art behind everything, crossfading between games ----
    const bool discover_art = !game_open_ && tab_ == tab::discover;
    const float art_height = discover_art ? 800.0f : 1080.0f;
    if (backdrop_in_.value < 1.0f && !previous_backdrop_.empty())
    {
        if (const Game *game = state.game(previous_backdrop_))
            draw_backdrop_art(ctx_, scene, *game, 1.0f - backdrop_in_.value, art_height);
    }
    if (!backdrop_.empty())
    {
        if (const Game *game = state.game(backdrop_))
            draw_backdrop_art(ctx_, scene, *game, backdrop_in_.value, art_height);
    }
    // The veils are always there, as the web's .backdrop is: with no art
    // behind them they darken the navy page.
    if (game_open_ || discover_art)
        draw_veils(scene, discover_art);
    // App settings are read over that art: a scrim keeps their text legible.
    if (settings_open_ && (game_open_ || discover_art))
        scene.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, ui::color::scrim);

    if (settings_open_)
    {
        settings_.draw(frame, !provider_open_);
        draw_game_header(scene);
    }
    else if (game_open_)
    {
        game_.draw(frame, !provider_open_);
        draw_game_header(scene);
    }
    else
    {
        if (content_blocked())
            draw_blocked(scene);
        else
            screen().draw(frame, area_ == Area::content && !settings_open_ && !provider_open_);
        draw_top_bar(scene);
    }

    hui::gfx::DrawList &overlay = frame.overlay;

    // ---- banners and the toast ----
    const ui::Type &type = ctx_.type;
    const char *banner = nullptr;
    const bool restarting =
        state.restart == Restart::stopping || state.restart == Restart::starting;
    if (restarting && !settings_open_)
        banner = tr("Restarting the download service\xE2\x80\xA6");
    else if (state.link == Link::offline && !restarting)
        banner = tr("Connection lost. Reconnecting to XENO STORE\xE2\x80\xA6");
    else if (!state.system.state_healthy)
        banner = tr("XENO STORE cannot save its queue. Check free space on the console\xE2\x80\x99s "
                 "internal storage.");
    if (banner != nullptr)
    {
        const float width = type.medium.measure(banner, 21.0f) + 64.0f;
        const ui::Rect pill{960.0f - width * 0.5f, 150.0f, width, 52.0f};
        overlay.rounded_rect(pill, 26.0f,
                             restarting ? ui::color::night3.with_alpha(0.95f)
                                        : ui::color::night3.with_alpha(0.92f));
        ui::text(overlay, type.medium, banner, pill.cx(), pill.cy() + 7.0f, 21.0f,
                 restarting ? ui::color::text : ui::color::bad, ui::Align::center);
    }
    if (toast_time_ > 0.0f && !toast_.empty())
    {
        const float alpha = std::min(1.0f, toast_time_ / 0.3f) *
                            std::min(1.0f, (toast_length_ - toast_time_) / 0.2f);
        // XENO STORE: a detail wraps under the line, up to three lines, and
        // the toast grows upward from where a one-line toast sits.
        constexpr float kDetailWidth = 1240.0f;
        std::vector<std::string> detail;
        if (!toast_detail_.empty())
            detail = type.regular.font->wrap(toast_detail_, 20.0f, kDetailWidth);
        const int lines = std::min(3, static_cast<int>(detail.size()));
        float width = type.medium.measure(toast_, 23.0f);
        if (lines > 0)
            width = std::min(width, kDetailWidth);
        for (int i = 0; i < lines; ++i)
            width = std::max(width, std::min(type.regular.measure(detail[static_cast<std::size_t>(i)],
                                                                  20.0f),
                                             kDetailWidth));
        width += 110.0f;
        const float height = 64.0f + (lines > 0 ? 12.0f + 28.0f * static_cast<float>(lines) : 0.0f);
        const ui::Rect pill{960.0f - width * 0.5f, 964.0f - height, width, height};
        const float mark_y = lines > 0 ? pill.y + 32.0f : pill.cy();
        overlay.push_opacity(std::clamp(alpha, 0.0f, 1.0f));
        overlay.shadow({pill.x, pill.y + 10.0f, pill.w, pill.h}, 32.0f, 30.0f,
                       ui::color::shadow.with_alpha(0.5f));
        overlay.rounded_rect(pill, 32.0f, ui::color::night3);
        if (toast_kind_ == ToastKind::update)
        {
            overlay.circle(pill.x + 40.0f, mark_y, 16.0f, ui::color::action);
            ui::icon_download(overlay, pill.x + 40.0f, mark_y, 18.0f, ui::color::action_ink);
        }
        else if (toast_kind_ == ToastKind::done)
        {
            overlay.circle(pill.x + 40.0f, mark_y, 16.0f, ui::color::ok);
            ui::icon_check(overlay, pill.x + 40.0f, mark_y, 16.0f, ui::color::night1, 3.0f);
        }
        else
        {
            overlay.circle(pill.x + 40.0f, mark_y, 16.0f,
                           toast_kind_ == ToastKind::problem ? ui::color::bad : ui::color::text2);
            ui::text(overlay, type.semibold, "!", pill.x + 40.0f, mark_y + 7.0f, 20.0f,
                     ui::color::night1, ui::Align::center);
        }
        ui::text_fit(overlay, type.medium, toast_, pill.x + 70.0f, mark_y + 8.0f, 23.0f,
                     pill.w - 110.0f, ui::color::text);
        if (lines > 0)
            ui::paragraph(overlay, type.regular, toast_detail_, pill.x + 70.0f, mark_y + 44.0f, 20.0f,
                          kDetailWidth, 28.0f, ui::color::text2, lines);
        overlay.pop_opacity();
    }

    if (provider_open_) draw_provider(frame);

    // ---- controller hints, over a fade that lets content scroll beneath ----
    std::vector<hui::ui::Hint> items;
    if (provider_open_)
        items = {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Back")}};
    else if (settings_open_)
        items = settings_.hints();
    else if (game_open_)
        items = game_.hints();
    else if (area_ == Area::top)
        items = {{hui::ui::Button::cross, tr("Select")},
                 {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    else if (content_blocked())
        items = {{hui::ui::Button::cross, tr("Choose sources")},
                 {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    else
        items = screen().hints();
    if (items.empty())
        return;
    const float hints_left =
        !game_open_ && (tab_ == tab::discover || tab_ == tab::library) ? 1250.0f : 0.0f;
    overlay.gradient_rect({hints_left, 960.0f, 1920.0f - hints_left, 50.0f}, 0.0f,
                          ui::color::night0.with_alpha(0.0f), ui::color::night0.with_alpha(0.95f));
    overlay.rounded_rect({hints_left, 1010.0f, 1920.0f - hints_left, 70.0f}, 0.0f,
                         ui::color::night0.with_alpha(0.95f));
    ui::hints(overlay, type, items);
}

} // namespace orbit
