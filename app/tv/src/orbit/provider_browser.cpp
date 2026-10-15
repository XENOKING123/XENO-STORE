// Orbit Store TV app - Provider verification without the browser storefront.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

namespace orbit
{

bool App::provider_actionable() const
{
    const auto &state = ctx_.store.state();
    const auto &browser = state.browser;
    return state.have_browser && state.link == Link::online && !ctx_.store.browser_action().busy &&
           ((!browser.active && browser.state == "queued" && !browser.job_id.empty()) ||
            (browser.active && browser.state != "cancelled" && !browser.id.empty()));
}

void App::update_provider(const InputFrame &input, Feedback &feedback)
{
    if (!provider_actionable()) provider_focus_ = 1;
    if (input.is_pressed(Action::back))
    {
        provider_open_ = false; // Going back never cancels or starts a transfer.
        feedback.play(hui::audio::Cue::back);
        return;
    }
    if (input.nav != Direction::none)
    {
        const bool previous = input.nav == Direction::left || input.nav == Direction::up;
        provider_focus_ = previous && provider_actionable() ? 0 : 1;
        feedback.play(hui::audio::Cue::focus);
    }
    if (!input.is_pressed(Action::confirm)) return;
    feedback.play(hui::audio::Cue::select);
    if (provider_focus_ == 1)
    {
        provider_open_ = false;
        return;
    }
    if (!provider_actionable()) return;
    const auto &browser = ctx_.store.state().browser;
    if (browser.active) ctx_.store.cancel_browser(browser.id);
    else
    {
        provider_open_ = false;
        Intent next;
        next.kind = Intent::Kind::show_job;
        next.id = browser.job_id;
        handle_intent(next, feedback);
    }
}

void App::draw_provider(Frame &frame) const
{
    auto &list = frame.overlay;
    const auto &state = ctx_.store.state();
    const auto &browser = state.browser;
    const auto &action = ctx_.store.browser_action();
    const auto &type = ctx_.type;
    list.rounded_rect({0,0,1920,1080},0,ui::color::scrim);
    const ui::Rect panel{280,220,1360,660};
    list.shadow(panel,30,40,ui::color::shadow.with_alpha(0.4f));
    ui::surface(list,panel,30,ui::color::night2);
    ui::text(list,type.medium,tr("Provider verification"),330,286,30,ui::color::text2);
    ui::text_fit(list,type.medium,browser.title.empty() ? tr("Provider verification") : browser.title,
                 330,345,36,1260,ui::color::text);
    const char *heading = action.busy ? tr("Please wait\xE2\x80\xA6")
        : browser.state == "queued" ? tr("Added to Downloads")
        : !state.have_browser || browser.state == "failed" || browser.state == "idle"
            ? tr("Verification couldn't finish")
        : browser.state == "cancelled" ? tr("Verification cancelled")
        : browser.state == "validating" ? tr("Checking your file")
        : browser.state == "waiting-link" ? tr("Waiting for the download link")
        : browser.state == "opening" ? tr("Opening the download page on your PS5…")
        : tr("Select Download, then return to XENO STORE.");
    ui::paragraph(list,type.medium,heading,330,411,26,1260,35,ui::color::text,3);
    const std::string message = !state.have_browser ? state.browser_error
        : browser.state == "idle" && !action.busy
            ? tr("This verification session is no longer active.") : browser.message;
    ui::paragraph(list,type.regular,std::string(tr(std::string_view(message))),330,517,22,1260,31,
                  ui::color::text2,4);
    if (!action.error.empty())
        ui::paragraph(list,type.regular,std::string(tr(std::string_view(action.error))),330,665,21,1260,29,
                      ui::color::bad,3);
    const bool queued = browser.state == "queued" && !browser.active;
    const char *label = queued ? tr("View download")
        : action.busy || browser.state == "cancelled" ? tr("Cancelling…") : tr("Cancel verification");
    if (browser.active || queued)
        ui::button(list,type,{330,773,605,65},label,ui::ButtonKind::secondary,
                   provider_focus_ == 0 ? 1 : 0,provider_actionable(),23);
    ui::button(list,type,{965,773,625,65},tr("Back"),ui::ButtonKind::light,
               provider_focus_ == 1 ? 1 : 0,true,23);
}

} // namespace orbit
