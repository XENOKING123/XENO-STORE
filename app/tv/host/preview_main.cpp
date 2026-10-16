// Orbit Store TV app - PC preview: runs the app headless and writes PNG pictures.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
//
// usage: xeno_preview <assets dir> <output dir> [scenario[,scenario...]|all] [width height]
//        xeno_preview --list   (every scenario as one JSON line; nothing is rendered)
//
// The same App, Store, ArtCache and drawing code as the console, rendered by
// Mesa's software OpenGL through surfaceless EGL, with a fixed 60 Hz clock.
// Data comes from the stand-in backend in fixture.cpp (invented games and
// generated art), or from a running XENO STORE backend when XENO_BACKEND is
// set to <ipv4>[:<port>] (port 9095 when omitted). Each
// scenario drives the app with scripted controller input and takes named
// pictures. Adapted from ps5-homebrew-ui's host/snapshot_main.cpp.
//
// Environment (all optional):
//   XENO_BACKEND=<ipv4>[:<port>]  a live backend; only scenarios marked live_ok run
//   XENO_SCENARIOS=a,b,c          the scenarios to run, in that order (else the argument)
//   XENO_LANG=de                  the interface language, from app/tv/i18n
//   XENO_APP_VERSION=01.002.001   this app's contentVersion (else sce_sys/param.json's)
//   XENO_TODAY=YYYY-MM-DD         the day Latest releases counts back from (2026-10-04)
//   XENO_LOADER_PORT=<port>       live: the real starter sends to an ELF loader on
//                                 127.0.0.1:<port> the copies named by
//   XENO_STARTER_SAVED_ELF, XENO_STARTER_SAVED_VERSION,
//   XENO_STARTER_BUNDLED_ELF, XENO_STARTER_BUNDLED_VERSION
//
// After each scenario one stdout line reads TVSTATE {...}: the store's state at
// the end, every toast shown and every error met on the way. A step may print
// TVMARK {...} first: the point where a harness changes the world (stops the
// backend, removes a drive) for the steps that follow.

#include "fixture.hpp"
#include "core/version.hpp"

#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "orbit/art.hpp"
#include "orbit/brand.hpp"
#include "orbit/gl_textures.hpp"
#include "orbit/http.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "orbit/starter.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include "../third_party/stb/stb_image_write.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using hui::Action;
using hui::Direction;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::string data;
    char buffer[65536];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, n);
    std::fclose(file);
    if (!font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

struct Step
{
    float wait = 0.4f; // simulated seconds before the step acts
    std::uint32_t press = 0;
    Direction nav = Direction::none;
    const char *capture = nullptr; // picture taken after the wait, before the input
    std::function<void(orbit::App &)> act;
    // After the wait, frames run until this holds, for at most timeout seconds
    // of real time: a live backend answers in its own time.
    std::function<bool()> until;
    float timeout = 0.0f;
    // When this is set and does not hold, the step does nothing more after its
    // wait: the data decides (a capability, a button that may be absent).
    std::function<bool()> when;
    int times = 1;              // the input, given this many times, each after the wait
    const char *mark = nullptr; // a TVMARK line: a harness changes the world here
    bool quick = false;         // the picture without waiting for art to arrive
};

struct Scenario
{
    const char *name;
    orbit::host::FixtureMode mode;
    bool live_ok; // runs against a live backend too
    std::vector<Step> steps;
    bool live_only = false; // needs a live backend: start-up, restart, banners
    bool own_world = false; // live: needs a world of its own, so "all" leaves it out
    bool wait_ready = true; // live: first wait until the backend answers with its data
};

Step pause(float seconds, const char *capture = nullptr)
{
    Step step;
    step.wait = seconds;
    step.capture = capture;
    return step;
}

Step move(Direction direction, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.nav = direction;
    return step;
}

Step tap(Action action, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.press = hui::action_bit(action);
    return step;
}

Step run(float seconds, std::function<void(orbit::App &)> act)
{
    Step step;
    step.wait = seconds;
    step.act = std::move(act);
    return step;
}

// Waits (real time, at most timeout seconds) until the condition holds, then
// takes the picture if one is named.
Step until(float timeout, std::function<bool()> condition, const char *capture = nullptr)
{
    Step step;
    step.wait = 0.0f;
    step.until = std::move(condition);
    step.timeout = timeout;
    step.capture = capture;
    return step;
}

// The step's input, given count times. Navigation stops at an edge, so "down
// twelve times" reaches the last row whatever the data.
Step times(int count, Step step)
{
    step.times = count;
    return step;
}

// The step, only when the condition holds by then.
Step when(std::function<bool()> condition, Step step)
{
    step.when = std::move(condition);
    return step;
}

// The step's picture taken at once, for screens that last a moment.
Step quick(Step step)
{
    step.quick = true;
    return step;
}

// Prints TVMARK: the harness acts on the world (stops the backend, removes a
// drive), then the following steps wait for the app to notice.
Step mark(const char *name, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.mark = name;
    return step;
}

// What a scenario showed and met on the way, for its TVSTATE line. Sampled
// after every frame.
struct Watch
{
    std::vector<std::string> toasts;
    float toast_left = 0.0f;
    std::string toast; // the one on screen
    std::vector<std::pair<std::string, std::string>> errors; // where, sentence: each once
    std::vector<std::string> starting, restart;            // states, as they changed
    bool restart_ended = false; // a restart went from stopping or starting to its end
    std::vector<std::string> captures, marks, timeouts, opened;
    std::mutex log_mutex; // the starter logs from the store's worker
    std::vector<std::string> log;
    std::string save_install; // XENO STORE: the last save install that ended, "state[:code]"
};

// The running scenario's store and what its steps picked: what conditions and
// picks look at.
orbit::Store *g_store = nullptr;
Watch *g_watch = nullptr;
std::string g_job;

const orbit::Snapshot &now()
{
    return g_store->state();
}

const char *link_name(orbit::Link link)
{
    switch (link)
    {
    case orbit::Link::online:
        return "online";
    case orbit::Link::offline:
        return "offline";
    default:
        return "connecting";
    }
}

const char *starting_name(orbit::Starting starting)
{
    switch (starting)
    {
    case orbit::Starting::starting:
        return "starting";
    case orbit::Starting::waiting:
        return "waiting";
    case orbit::Starting::no_loader:
        return "no-loader";
    case orbit::Starting::no_payload:
        return "no-payload";
    case orbit::Starting::failed:
        return "failed";
    default:
        return "idle";
    }
}

const char *restart_name(orbit::Restart restart)
{
    switch (restart)
    {
    case orbit::Restart::stopping:
        return "stopping";
    case orbit::Restart::starting:
        return "starting";
    case orbit::Restart::done:
        return "done";
    case orbit::Restart::manual:
        return "manual";
    case orbit::Restart::failed:
        return "failed";
    case orbit::Restart::older:
        return "older";
    default:
        return "idle";
    }
}

void observe(const orbit::App &app, Watch &watch)
{
    const orbit::Snapshot &state = g_store->state();
    // A toast's count starts again when it is shown; one toast can also
    // replace another that had longer left.
    const bool showing = app.toast_left() > 0.0f && !app.toast().empty();
    if (showing && (app.toast_left() > watch.toast_left + 1e-4f || app.toast() != watch.toast))
        watch.toasts.push_back(app.toast_detail().empty() ? app.toast()
                                                          : app.toast() + " | " + app.toast_detail());
    watch.toast_left = app.toast_left();
    watch.toast = showing ? app.toast() : std::string();
    const auto error = [&watch](const char *where, const std::string &text)
    {
        if (text.empty())
            return;
        for (const auto &seen : watch.errors)
            if (seen.first == where && seen.second == text)
                return;
        watch.errors.emplace_back(where, text);
    };
    error("link", state.link_error);
    error("library", state.library_error);
    error("browser", state.browser_error);
    error("download", g_store->action().error);
    error("favourite", g_store->favourite_action().error);
    error("libraryAction", g_store->library_action().error);
    error("browserAction", g_store->browser_action().error);
    error("setting", g_store->setting_result().error);
    // XENO STORE: the Save Data Store's list, and the save install.
    const orbit::SavesState &saves = g_store->saves().state();
    if (saves.load == orbit::SavesLoad::failed || saves.load == orbit::SavesLoad::unsupported)
        error("saves", saves.error);
    const orbit::SaveInstall &install = g_store->saves().install();
    error("saveInstall", install.error);
    if (install.ended())
    {
        const orbit::SaveJob &job = install.job;
        watch.save_install = !install.have_job ? std::string("refused")
                             : job.state == "done" || job.state == "cancelled" || job.error_code.empty()
                                 ? job.state
                                 : job.state + ":" + job.error_code;
        if (install.have_job && job.state == "failed")
            error("saveInstall", job.error);
    }
    if (state.starting == orbit::Starting::no_loader ||
        state.starting == orbit::Starting::no_payload || state.starting == orbit::Starting::failed)
        error("start", state.start_detail);
    if (state.restart == orbit::Restart::failed || state.restart == orbit::Restart::manual ||
        state.restart == orbit::Restart::older)
        error("restart", state.restart_detail);
    const auto changed = [](std::vector<std::string> &states, const char *name)
    {
        if (states.empty() || states.back() != name)
            states.emplace_back(name);
    };
    changed(watch.starting, starting_name(state.starting));
    if (!watch.restart.empty() &&
        (watch.restart.back() == "stopping" || watch.restart.back() == "starting") &&
        state.restart != orbit::Restart::stopping && state.restart != orbit::Restart::starting)
        watch.restart_ended = true;
    changed(watch.restart, restart_name(state.restart));
}

// The backend answers and the app has what its screens show: the catalogue
// and the queue, or the first-run setup.
bool ready_now()
{
    const orbit::Snapshot &state = now();
    return state.link == orbit::Link::online && state.have_system && state.have_sources &&
           (state.setup_needed() || (state.have_catalog && state.have_queue));
}

// The store has sent a copy to the loader, or is sending one, now or earlier
// in the scenario (on the host the backend may answer within a frame or two).
bool start_seen()
{
    for (const std::string &state : g_watch->starting)
        if (state == "starting" || state == "waiting")
            return true;
    return false;
}

bool settled()
{
    return !g_store->action().busy && !g_store->setting_result().busy &&
           !g_store->library_action().busy && !g_store->favourite_action().busy;
}

bool active_job(const std::string &game_id)
{
    for (const orbit::Job &job : now().jobs)
        if (job.game_id == game_id && orbit::job_view(job) == orbit::JobView::active)
            return true;
    return false;
}

bool any_job(const std::string &game_id)
{
    for (const orbit::Job &job : now().jobs)
        if (job.game_id == game_id && job.status != "cancelled")
            return true;
    return false;
}

bool in_library(const orbit::Game &game)
{
    for (const orbit::LibraryGame &owned : now().library_games())
        for (const orbit::Release &release : game.releases)
            if (!release.title_id.empty() && release.title_id == owned.title_id)
                return true;
    return false;
}

// Kinds of game the scenarios open.
bool downloadable(const orbit::Game &game)
{
    return game.platform == "ps5" && !game.releases.front().browser_only() && !any_job(game.id) &&
           !in_library(game);
}
bool browser_only(const orbit::Game &game)
{
    return game.releases.front().browser_only();
}
bool for_ps4(const orbit::Game &game)
{
    return game.platform == "ps4";
}
bool owned(const orbit::Game &game)
{
    return in_library(game);
}
bool queued(const orbit::Game &game)
{
    return active_job(game.id);
}

// The fixture's own game when the catalogue has it, so the fixture pictures
// stay as they were; else the first game of the kind (a live catalogue has
// its own games).
void open_pick(orbit::App &app, const char *fixture_id, bool (*fits)(const orbit::Game &))
{
    std::string id = fixture_id;
    if (now().game(fixture_id) == nullptr)
    {
        for (const orbit::Game &game : now().games)
        {
            if (!game.releases.empty() && fits(game))
            {
                id = game.id;
                break;
            }
        }
    }
    app.open_game(id);
}

bool job_cancelled(const orbit::Job &job)
{
    return job.status == "cancelled";
}
bool job_complete(const orbit::Job &job)
{
    return job.status == "complete";
}
bool job_downloading(const orbit::Job &job)
{
    return job.status == "downloading";
}
bool job_running(const orbit::Job &job)
{
    return orbit::job_active(job);
}

// The same for jobs; the pick is kept in g_job for later steps.
std::string pick_job(const char *fixture_id, bool (*fits)(const orbit::Job &),
                     bool (*fallback)(const orbit::Job &) = nullptr)
{
    const std::vector<orbit::Job> &jobs = now().jobs;
    for (const orbit::Job &job : jobs)
        if (job.id == fixture_id)
            return g_job = job.id;
    for (auto *test : {fits, fallback})
        for (const orbit::Job &job : jobs)
            if (test != nullptr && test(job))
                return g_job = job.id;
    return g_job = fixture_id;
}

void show_job(orbit::App &app, const char *fixture_id, bool (*fits)(const orbit::Job &),
              bool (*fallback)(const orbit::Job &) = nullptr)
{
    app.show_tab(orbit::tab::downloads);
    app.downloads().show_job(pick_job(fixture_id, fits, fallback));
}

std::string job_status(const std::string &id)
{
    for (const orbit::Job &job : now().jobs)
        if (job.id == id)
            return job.status;
    return {};
}

// A Library game the copy and move actions take: on a drive, not mounted.
const orbit::LibraryGame *copyable()
{
    for (const orbit::LibraryGame &game : now().library_games())
        if (game.managed && game.can_manage_source && game.on_drive && !game.mounted)
            return &game;
    return nullptr;
}

void show_copyable(orbit::App &app)
{
    app.show_tab(orbit::tab::library);
    if (const orbit::LibraryGame *game = copyable())
        app.library().show_game(game->title_id, game->source_key);
}

bool library_can(const char *capability)
{
    return now().library.can(capability);
}

bool library_ready()
{
    return now().have_library && now().library.ready();
}

bool storage_job_seen()
{
    const orbit::LibrarySnapshot &library = now().library;
    return library.has_storage_job && library.storage_job.id > 0;
}

bool storage_job_ended()
{
    const orbit::LibrarySnapshot &library = now().library;
    return storage_job_seen() && !library.storage_job.active && !library.storage_busy;
}

// XENO STORE: the Save Data Store answered with its saves (not still building).
bool saves_listed()
{
    const orbit::SavesState &saves = g_store->saves().state();
    return saves.load == orbit::SavesLoad::ready && !saves.listing.building &&
           !saves.listing.saves.empty();
}

bool saves_building()
{
    return g_store->saves().state().building();
}

bool saves_settled()
{
    const orbit::SavesState &saves = g_store->saves().state();
    return saves.load != orbit::SavesLoad::idle && saves.load != orbit::SavesLoad::loading &&
           !saves.building();
}

bool save_install_ended()
{
    return g_store->saves().install().ended();
}

// The list answered by the store's GitHub index alone (saves_partial's reload).
bool saves_github_only()
{
    const orbit::SavesState &saves = g_store->saves().state();
    return saves.load == orbit::SavesLoad::ready && saves.listing.backend == "github";
}

// A save install is downloading, with bytes arrived.
bool save_install_running()
{
    const orbit::SaveInstall &install = g_store->saves().install();
    return install.active && install.have_job && install.job.state == "running" &&
           install.job.done > 0.0;
}

// A game with at least two community saves.
bool has_saves(const orbit::Game &game)
{
    return orbit::count_saves(g_store->saves().state().listing.saves, game) >= 2;
}

// The Saves tab once its list is in, then one save's details (the live
// world's saves have the fixture's IDs: tools/tv/world.py).
std::vector<Step> save_details(const char *save_id)
{
    return {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
            until(30.0f, saves_listed),
            run(0.6f, [save_id](orbit::App &app) { app.saves().open_save(save_id); })};
}

// The service's install button shows (a newer or the same release, not saved yet).
bool service_installable()
{
    const orbit::ServiceUpdate &service = now().service;
    const std::string running =
        service.running_version.empty() ? now().system.version : service.running_version;
    const bool saved_latest = !service.latest_version.empty() &&
                              service.saved_version == service.latest_version &&
                              service.restart_required;
    return now().have_service_update && service.available && !service.latest_version.empty() &&
           !saved_latest && orbit::starter::compare_versions(service.latest_version, running) >= 0;
}

using Section = orbit::SettingsScreen::Section;

// Opens a Settings section once its data is in, with the focus on its first
// control (opening it asks the store for the statuses it shows).
std::vector<Step> settings_at(Section section, std::function<bool()> loaded)
{
    return {run(0.5f, [section](orbit::App &app) { app.open_settings(section); }),
            until(15.0f, std::move(loaded)),
            run(0.3f, [section](orbit::App &app) { app.open_settings(section, true); })};
}

std::vector<Step> join(std::vector<std::vector<Step>> parts)
{
    std::vector<Step> steps;
    for (std::vector<Step> &part : parts)
        for (Step &step : part)
            steps.push_back(std::move(step));
    return steps;
}

// Stands in for the ELF loader: the scenario's name says what starting XENO STORE does.
class PreviewStarter final : public orbit::starter::Starter
{
  public:
    explicit PreviewStarter(orbit::starter::Outcome outcome) : outcome_(outcome)
    {
    }
    orbit::starter::Result start() override
    {
        orbit::starter::Result result;
        result.outcome = outcome_;
        result.detail = outcome_ == orbit::starter::Outcome::sent
                            ? "Sent XENO STORE 1.2.1 to the ELF loader."
                            : "No ELF loader is listening on port 9021.";
        return result;
    }

  private:
    orbit::starter::Outcome outcome_;
};

// The port gives starter::Locations a loader_port and Context an
// open_game_browser; with or without them, the preview sets what exists.
template <typename Where>
std::unique_ptr<orbit::starter::Starter>
loader_starter(Where where, std::uint16_t port, std::function<void(const std::string &)> log)
{
    using orbit::starter::LoaderStarter;
    if constexpr (requires { where.loader_port = port; } &&
                  std::is_constructible_v<LoaderStarter, Where,
                                          std::function<void(const std::string &)>>)
    {
        where.loader_port = port;
        return std::make_unique<LoaderStarter>(std::move(where), std::move(log));
    }
    else
    {
        return std::make_unique<LoaderStarter>(std::move(where), port, std::move(log));
    }
}

template <typename Context, typename Open>
void set_open_game_browser(Context &context, Open open)
{
    if constexpr (requires { context.open_game_browser = open; })
        context.open_game_browser = std::move(open);
}

std::vector<Scenario> scenarios()
{
    using Mode = orbit::host::FixtureMode;
    std::vector<Scenario> list;
    list.push_back(
        {"opening", Mode::offline, false, {pause(0.9f, "opening"), pause(3.0f, "offline")}});
    list.push_back({"starting", Mode::offline, false, {pause(1.0f, "starting")}});
    // Live: nothing answers and the ELF loader's port is closed (XENO_LOADER_PORT),
    // or no loader port is given and a stand-in reports none.
    list.push_back({"no-loader",
                    Mode::offline,
                    true,
                    {pause(1.0f),
                     until(15.0f,
                           [] {
                               const orbit::Starting starting = now().starting;
                               return starting == orbit::Starting::no_loader ||
                                      starting == orbit::Starting::no_payload ||
                                      starting == orbit::Starting::failed;
                           },
                           "no-loader")},
                    false,
                    true,
                    false});
    // First run: choose sources in the app, then on to Discover.
    list.push_back({"setup",
                    Mode::setup,
                    true,
                    {pause(1.5f, "setup"), tap(Action::confirm), pause(0.8f, "setup-sources"),
                     move(Direction::right), tap(Action::confirm), move(Direction::down),
                     tap(Action::confirm), move(Direction::down), pause(0.8f, "setup-ready"),
                     tap(Action::confirm), pause(2.5f, "setup-done")},
                    false,
                    true});
    list.push_back({"discover",
                    Mode::normal,
                    true,
                    {run(2.0f, [](orbit::App &app) { app.show_tab(orbit::tab::discover); }),
                     pause(1.2f, "discover"), move(Direction::down), move(Direction::right),
                     move(Direction::right), pause(1.2f, "discover-rail"), move(Direction::down),
                     pause(1.0f, "discover-added"), move(Direction::down),
                     pause(1.0f, "discover-all"), move(Direction::up), move(Direction::up), move(Direction::up),
                     move(Direction::up), pause(0.6f, "top-bar"),
                     // Square on a rail's cover saves it to favourites: the heart.
                     move(Direction::down), move(Direction::down), tap(Action::west),
                     until(10.0f, settled), pause(1.0f, "discover-favourite")}});
    list.push_back({"game", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { open_pick(app, "ember-hollow", downloadable); }),
         pause(1.2f, "game"), tap(Action::confirm), pause(0.8f, "game-options"),
         move(Direction::up), move(Direction::left), move(Direction::left), tap(Action::confirm),
         pause(0.8f, "game-source"), move(Direction::down), tap(Action::confirm),
         move(Direction::right), move(Direction::right), tap(Action::confirm),
         pause(0.8f, "game-drive-menu"), tap(Action::back), move(Direction::left), tap(Action::confirm),
         pause(0.8f, "game-delivery"), tap(Action::back), tap(Action::back),
         move(Direction::right), move(Direction::right), tap(Action::confirm), pause(0.8f, "game-about"),
         // Back to the page, then Download to PS5 from the options.
         tap(Action::back), move(Direction::left), move(Direction::left), tap(Action::confirm),
         pause(0.8f, "game-download-ready"), tap(Action::confirm), until(15.0f, settled),
         pause(1.5f, "game-download-added")}});
    list.push_back({"game-browser", Mode::normal, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("ironclad-skies"); }),
         pause(1.0f, "game-browser-hub"), tap(Action::confirm), pause(1.0f, "game-browser"),
         move(Direction::left), pause(0.6f, "game-source-notes"), move(Direction::down),
         pause(0.6f, "game-source-notes-scroll"), tap(Action::back), move(Direction::right),
         move(Direction::right), tap(Action::confirm), pause(0.6f, "game-browser-about")}});
    list.push_back({"game-ps4", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { open_pick(app, "copper-circuit-ps4", for_ps4); }),
         pause(1.0f, "game-ps4"), tap(Action::confirm), pause(1.0f, "game-ps4-options")}});
    list.push_back({"game-library", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { open_pick(app, "quiet-harbour", owned); }),
         tap(Action::confirm), pause(1.0f, "game-library")}});
    // A Vikingfile-only game: the browser-only notice and Open browser version.
    list.push_back({"game-viking", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { open_pick(app, "ironclad-skies", browser_only); }),
         pause(1.0f, "game-viking"), tap(Action::confirm), pause(1.0f, "game-viking-notice"),
         tap(Action::confirm), until(10.0f, settled), pause(1.2f, "game-viking-open")}});
    list.push_back({"debrid", Mode::debrid, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("neon-courier"); }),
         tap(Action::confirm), move(Direction::up), move(Direction::left), tap(Action::confirm),
         pause(0.6f, "debrid-choice"), move(Direction::down), tap(Action::confirm),
         move(Direction::down), pause(0.8f, "debrid-ready"), tap(Action::confirm), pause(1.5f, "debrid-queued")}});
    list.push_back({"delete-cancelled",
                    Mode::normal,
                    true,
                    {run(1.5f, [](orbit::App &app) { show_job(app, "job105", job_cancelled); }),
                     pause(0.8f, "cancelled-download"),
                     move(Direction::right), tap(Action::confirm),
                     pause(0.8f, "delete-cancelled"), move(Direction::left),
                     move(Direction::left), tap(Action::confirm),
                     pause(0.8f, "forget-cancelled")}});
    list.push_back({"queued",
                    Mode::normal,
                    true,
                    {run(1.5f, [](orbit::App &app) { open_pick(app, "tidewater", queued); }),
                     pause(1.2f, "game-queued")}});
    list.push_back(
        {"download",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { open_pick(app, "neon-courier", downloadable); }),
          move(Direction::down, 0.6f), move(Direction::down), pause(0.6f, "download-ready"),
          tap(Action::confirm, 0.2f), pause(1.5f, "download-added")}});
    list.push_back(
        {"downloads",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::downloads); }),
          pause(1.2f, "downloads"), move(Direction::down), pause(0.8f, "downloads-row"),
          move(Direction::right), tap(Action::confirm), pause(0.8f, "downloads-dialog"),
          tap(Action::back), move(Direction::up), move(Direction::up), move(Direction::right),
          move(Direction::right), pause(0.8f, "downloads-failed"),
          // The other two views: cancelled, then finished.
          move(Direction::right), pause(0.8f, "downloads-cancelled"), move(Direction::left),
          move(Direction::left), pause(0.8f, "downloads-complete")}});
    // The cancel dialog of a running download.
    list.push_back({"downloads-dialog", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { show_job(app, "job100", job_downloading, job_running); }),
         pause(1.0f), move(Direction::right), tap(Action::confirm),
         pause(0.8f, "downloads-dialog-cancel"), tap(Action::back), pause(0.6f)}});
    // Pause and resume a download, then delete a cancelled one.
    list.push_back({"downloads-actions", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { show_job(app, "job100", job_downloading, job_running); }),
         pause(1.0f, "downloads-actions-active"), tap(Action::confirm),
         until(15.0f, [] { return settled() && job_status(g_job) == "paused"; }),
         run(0.3f, [](orbit::App &app) { app.downloads().show_job(g_job); }),
         pause(0.8f, "downloads-actions-paused"), tap(Action::confirm),
         until(15.0f, [] { return settled() && job_status(g_job) != "paused"; }),
         run(0.3f, [](orbit::App &app) { app.downloads().show_job(g_job); }),
         pause(0.8f, "downloads-actions-resumed"),
         run(0.5f, [](orbit::App &app) { show_job(app, "job105", job_cancelled); }),
         pause(0.8f, "downloads-actions-cancelled"), move(Direction::right),
         tap(Action::confirm), move(Direction::left), pause(0.6f, "downloads-actions-delete"),
         tap(Action::confirm),
         until(15.0f, [] { return settled() && job_status(g_job).empty(); }),
         pause(1.0f, "downloads-actions-deleted")}});
    list.push_back({"downloads-finished", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { show_job(app, "job104", job_complete); }),
         pause(1.0f, "downloads-finished")}});
    list.push_back(
        {"browse",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::browse); }),
          pause(1.2f, "browse"), tap(Action::confirm),
          run(0.3f, [](orbit::App &app) { app.browse().set_query("o"); }),
          pause(0.9f, "browse-keyboard"), tap(Action::back), move(Direction::down),
          move(Direction::left), move(Direction::left), tap(Action::confirm),
          pause(0.8f, "browse-filters"), move(Direction::down), tap(Action::confirm),
          move(Direction::down), move(Direction::right), pause(1.0f, "browse-results")}});
    list.push_back({"browse-platform", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::browse); }),
         move(Direction::down), move(Direction::right), move(Direction::right), move(Direction::right),
         tap(Action::confirm), pause(0.5f, "browse-platform-menu"), move(Direction::down), move(Direction::down),
         tap(Action::confirm), move(Direction::down), pause(0.8f, "browse-ps4"),
         tap(Action::confirm), pause(0.8f, "browse-ps4-details")}});
    // The region and format menus, and the Favourites collection.
    list.push_back({"browse-filters", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::browse); }),
         move(Direction::down), tap(Action::confirm), pause(0.6f, "browse-region-menu"),
         move(Direction::down), tap(Action::confirm), pause(0.8f, "browse-region"),
         tap(Action::confirm), move(Direction::up), tap(Action::confirm),
         move(Direction::left), tap(Action::confirm), pause(0.6f, "browse-format-menu"),
         tap(Action::back), move(Direction::left), move(Direction::left), tap(Action::confirm),
         pause(1.0f, "browse-favourites")}});
    // XENO STORE: the Save Data Store. The list, For my games, a search and a save's details.
    list.push_back({"saves", Mode::normal, true,
        {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         until(30.0f, saves_listed), pause(1.2f, "saves"), move(Direction::down),
         move(Direction::right), tap(Action::confirm), pause(0.8f, "saves-filter"),
         move(Direction::up), tap(Action::confirm),
         run(0.3f, [](orbit::App &app) { app.saves().set_query("ember"); }),
         pause(0.9f, "saves-search"), tap(Action::back), move(Direction::down), move(Direction::down),
         pause(0.6f, "saves-results"), tap(Action::confirm), pause(1.0f, "saves-detail")},
        false, true});
    // A game page's Community saves, then that game's saves.
    list.push_back({"game-saves", Mode::normal, true,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         until(30.0f, saves_listed),
         run(0.5f, [](orbit::App &app) { open_pick(app, "ember-hollow", has_saves); }),
         pause(1.2f, "game-saves"), times(3, move(Direction::right)), tap(Action::confirm),
         pause(1.0f, "saves-game")},
        false, true});
    // Install for the second profile, followed to its end.
    list.push_back({"saves-install", Mode::normal, true,
        join({save_details("xk-ember-forge"),
              {pause(0.8f), move(Direction::left), tap(Action::confirm), pause(0.6f, "saves-profiles"),
               move(Direction::down), tap(Action::confirm), move(Direction::right), tap(Action::confirm),
               until(60.0f, save_install_ended), pause(0.8f, "saves-installed")}}),
        false, true});
    // A slow download: the progress, then Cancel.
    list.push_back({"saves-progress", Mode::normal, true,
        join({save_details("xk-starfall-season"),
              {pause(0.8f), tap(Action::confirm), until(30.0f, save_install_running),
               pause(1.0f, "saves-installing"), move(Direction::left), tap(Action::confirm),
               until(30.0f, save_install_ended), pause(0.8f, "saves-cancelled")}}),
        false, true});
    // A save with a long name and long notes: the facts stay clear of them.
    list.push_back({"saves-long", Mode::normal, true,
        join({save_details("sb:5a1d0000-0000-4000-8000-000000000003"),
              {pause(1.0f, "saves-detail-long")}}),
        false, true});
    // Install, then close the details at once: the card follows the install,
    // and a toast says how it ended.
    list.push_back({"saves-closed", Mode::normal, true,
        join({save_details("xk-ember-forge"),
              {pause(0.8f), tap(Action::confirm, 0.1f), tap(Action::back, 0.1f),
               quick(pause(0.3f, "saves-card-installing")), until(60.0f, save_install_ended),
               quick(pause(0.5f, "saves-toast"))}}),
        false, true});
    // The game is running: the store refuses before downloading.
    list.push_back({"saves-running", Mode::normal, true,
        join({save_details("xk-orchard-bloom"),
              {pause(0.8f), tap(Action::confirm), until(30.0f, save_install_ended),
               pause(0.8f, "saves-title-running")}}),
        false, true});
    // No native engine and no Garlic Save Manager: the store's own sentence.
    list.push_back({"saves-garlic", Mode::saves_garlic, true,
        join({save_details("xk-ember-forge"),
              {pause(0.8f), tap(Action::confirm), until(60.0f, save_install_ended),
               pause(0.8f, "saves-garlic")}}),
        false, true});
    // The store's first list builds its index: the screen waits, then has none.
    list.push_back({"saves-building", Mode::saves_empty, true,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         until(30.0f, saves_building), pause(0.6f, "saves-building"), until(60.0f, saves_settled),
         pause(0.8f, "saves-empty")},
        false, true});
    // A store without the Save Data Store.
    list.push_back({"saves-older", Mode::saves_older, false,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         until(10.0f, saves_settled), pause(0.8f, "saves-unsupported")}});
    // The list on its way, then a store that cannot list the saves: Try again.
    list.push_back({"saves-failed", Mode::saves_failed, false,
        {run(0.1f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         quick(pause(0.3f, "saves-loading")), until(10.0f, saves_settled),
         pause(0.8f, "saves-failed")}});
    // The XenoKing site cannot be reached, then the Settings backend is not usable.
    list.push_back({"saves-partial", Mode::saves_partial, false,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::saves); }),
         until(10.0f, saves_listed), pause(1.0f, "saves-partial"),
         run(0.3f, [](orbit::App &) { g_store->saves().reload(true); }),
         until(10.0f, saves_github_only), pause(1.0f, "saves-override")}});
    list.push_back(
        {"library",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::library); }),
          pause(1.2f, "library"), move(Direction::down), move(Direction::down),
          move(Direction::left), move(Direction::left), move(Direction::left),
          pause(0.8f, "library-grid"), tap(Action::confirm), pause(0.8f, "library-details"),
          when([] { return library_can("mount_game"); }, move(Direction::right)),
          tap(Action::confirm), pause(1.2f, "library-copy"),
          tap(Action::back), tap(Action::back), move(Direction::up), move(Direction::up),
          tap(Action::confirm), pause(1.2f, "library-storage"),
          // Back to the games: the Installed view, then Unavailable once a drive
          // has gone (live: the harness removes one at the mark).
          tap(Action::confirm), move(Direction::down), times(6, move(Direction::left, 0.2f)),
          move(Direction::right), tap(Action::confirm), pause(1.0f, "library-installed"),
          mark("remove-drive"),
          // The harness removes the drive after the mark, in its own time. Ask the
          // Library to look again every 5 s while waiting, as a player pressing
          // Refresh would, so a slow machine does not have to sit out the 30 s
          // re-check (the first refresh can land before the drive has gone).
          until(90.0f,
                [last = std::make_shared<std::chrono::steady_clock::time_point>()] {
                    for (const orbit::LibraryGame &game : now().library_games())
                        if (!game.on_drive)
                            return true;
                    const auto t = std::chrono::steady_clock::now();
                    if (*last == std::chrono::steady_clock::time_point{} || t - *last >= std::chrono::seconds(5)) {
                        *last = t;
                        g_store->refresh_library();
                    }
                    return false;
                }),
          times(2, move(Direction::right)), tap(Action::confirm),
          pause(1.0f, "library-unavailable")}});
    // A game's details panel and its Copy confirmation, left without copying.
    list.push_back({"library-details", Mode::normal, true,
        {until(15.0f, [] { return copyable() != nullptr; }), run(0.5f, show_copyable),
         pause(1.0f, "library-details-panel"),
         when([] { return library_can("mount_game"); }, move(Direction::right)),
         tap(Action::confirm), pause(1.2f, "library-details-copy"), tap(Action::back),
         tap(Action::back), pause(0.5f)}});
    // Copy a game to the other drive and follow the storage job to its end.
    list.push_back({"library-copy", Mode::normal, true,
        {until(15.0f, [] { return copyable() != nullptr; }), run(0.5f, show_copyable),
         pause(1.0f), when([] { return library_can("mount_game"); }, move(Direction::right)),
         tap(Action::confirm),
         until(15.0f, [] { return now().have_library_storage &&
                                  !now().library_storage.destinations.empty(); }),
         pause(1.0f, "library-copy-confirm"), tap(Action::confirm),
         pause(0.6f, "library-copy-destinations"), move(Direction::down), tap(Action::confirm),
         move(Direction::down), pause(0.6f, "library-copy-ready"), tap(Action::confirm),
         until(30.0f, storage_job_seen), pause(1.0f, "library-copy-progress"),
         until(120.0f, storage_job_ended), pause(1.0f, "library-copy-done")},
        true});
    // The Storage section: the drives, then one drive's games.
    list.push_back({"library-storage", Mode::normal, true,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::library); }),
         until(15.0f, library_ready), pause(0.5f), tap(Action::confirm),
         until(15.0f, [] { return now().have_library_storage; }),
         pause(1.2f, "library-storage-drives"), move(Direction::down), tap(Action::confirm),
         pause(1.0f, "library-storage-drive")}});
    list.push_back({"settings",
                    Mode::normal,
                    true,
                    {run(1.5f, [](orbit::App &app) { app.open_settings(); }),
                     pause(0.8f, "settings"),
                     move(Direction::down),
                     pause(0.8f, "settings-storage"),
                     move(Direction::right),
                     move(Direction::down),
                     move(Direction::down),
                     tap(Action::confirm),
                     pause(1.0f, "settings-storage-saved"),
                     move(Direction::left),
                     move(Direction::down),
                     pause(1.0f, "settings-pairing"),
                     move(Direction::down),
                     move(Direction::right),
                     pause(0.8f, "settings-catalogue"),
                     tap(Action::confirm),
                     pause(0.8f, "settings-catalogue-refreshed"),
                     move(Direction::left),
                     move(Direction::down),
                     pause(1.0f, "settings-updates-current"),
                     move(Direction::down),
                     pause(0.8f, "settings-more")}});
    // One Settings section each.
    list.push_back({"settings-sources", Mode::normal, true,
        join({settings_at(Section::sources, [] { return now().have_sources; }),
              {pause(1.0f, "settings-sources-list")}})});
    list.push_back({"settings-storage", Mode::normal, true,
        join({settings_at(Section::storage, [] { return now().have_queue; }),
              {pause(1.0f, "settings-storage-drives"), tap(Action::confirm),
               until(10.0f, settled), pause(1.0f, "settings-storage-auto")}})});
    // The stepper stops at 4 (XENO STORE's limit), then the archive switch.
    list.push_back({"settings-downloads", Mode::normal, true,
        join({settings_at(Section::storage, [] { return now().have_queue; }),
              {times(12, move(Direction::down, 0.15f)), move(Direction::up),
               times(4, move(Direction::right, 0.15f)), pause(0.8f, "settings-downloads-limit")},
              [] {
                  std::vector<Step> presses;
                  for (int i = 0; i < 4; ++i)
                  {
                      presses.push_back(tap(Action::confirm));
                      presses.push_back(until(10.0f, settled));
                  }
                  return presses;
              }(),
              {pause(0.8f, "settings-downloads-max"), move(Direction::down), tap(Action::confirm),
               until(10.0f, settled), pause(0.8f, "settings-downloads-archives")}})});
    list.push_back({"settings-pairing", Mode::normal, true,
        {run(0.5f, [](orbit::App &app) { app.open_settings(Section::pairing); }),
         until(15.0f, [] { return !now().pair_code.empty(); }),
         pause(1.2f, "settings-pairing-code")}});
    list.push_back({"settings-catalogue", Mode::normal, true,
        join({settings_at(Section::catalogue, [] { return now().have_catalogue_status; }),
              {pause(0.8f, "settings-catalogue-status"), tap(Action::confirm),
               pause(0.3f, "settings-catalogue-busy"),
               until(60.0f, [] { return settled() && !now().catalogue.busy; }),
               pause(1.0f, "settings-catalogue-done")}})});
    // The download service: check, then install what the check found.
    list.push_back({"settings-updates", Mode::normal, true,
        join({settings_at(Section::updates,
                          [] { return now().have_service_update && now().have_tv_app; }),
              {pause(1.0f, "settings-updates-status"), move(Direction::down),
               when([] { return now().service.available; }, tap(Action::confirm)),
               until(60.0f,
                     [] {
                         return settled() && now().service.checked_at > 0.0 &&
                                !now().service.busy;
                     }),
               pause(1.0f, "settings-updates-checked"),
               when(service_installable, move(Direction::right)),
               when(service_installable, tap(Action::confirm)),
               until(120.0f,
                     [] {
                         return settled() && !now().service.busy &&
                                now().service.phase != "downloading" &&
                                now().service.phase != "saving";
                     }),
               pause(1.0f, "settings-updates-saved")}})});
    // The TV app's card (installed none, orbit or manual: the harness's world) and a check.
    list.push_back({"settings-tv-app", Mode::normal, true,
        join({settings_at(Section::updates, [] { return now().have_tv_app; }),
              {pause(1.0f, "settings-tv-app"),
               when([] { return now().tv_app.available; }, tap(Action::confirm)),
               until(60.0f,
                     [] {
                         return settled() && !now().tv_app.busy &&
                                (!now().tv_app.available || now().tv_app.checked_at > 0.0);
                     }),
               pause(1.0f, "settings-tv-app-checked")}})});
    list.push_back({"settings-more", Mode::normal, true,
        {run(0.5f, [](orbit::App &app) { app.open_settings(Section::more); }),
         pause(1.0f, "settings-more-page")}});
    // Newer releases of both parts: the badge, guided installs and a restart.
    list.push_back(
        {"updates",
         Mode::updates,
         false,
         {pause(2.5f, "updates-badge"),
          run(0.5f, [](orbit::App &app)
              { app.open_settings(orbit::SettingsScreen::Section::updates, true); }),
          pause(1.0f, "settings-updates"), move(Direction::right), tap(Action::confirm),
          pause(1.0f, "settings-tv-installed"), move(Direction::down), move(Direction::right),
          tap(Action::confirm), pause(1.2f, "settings-service-saved"), tap(Action::confirm),
          pause(0.8f, "settings-restart-confirm"), move(Direction::left), tap(Action::confirm),
          pause(0.6f, "settings-restarting"), pause(40.0f, "settings-restarted")}});

    // ---- live only: each needs a world of its own (scenarios.json) ----

    // Nothing answers and nothing can start the backend.
    list.push_back({"offline", Mode::offline, true,
        {pause(0.9f, "offline-opening"),
         until(15.0f, [] { return now().link == orbit::Link::offline; }),
         pause(3.0f, "offline-screen")},
        true, true, false});
    // The backend is down and the fake loader up: the app sends the bundled
    // copy (XENO_LOADER_PORT, XENO_STARTER_*) and opens on Discover.
    list.push_back({"cold-start", Mode::offline, true,
        {quick(pause(0.1f, "cold-start-opening")),
         quick(until(20.0f, start_seen, "cold-start-starting")),
         until(60.0f, ready_now), pause(2.0f, "cold-start-discover")},
        true, true, false});
    // The saved copy exits at once: the first start (copy=saved) fails after
    // its wait; Try again sends the bundled copy, which answers.
    list.push_back({"crash-fallback", Mode::offline, true,
        {until(20.0f, start_seen),
         until(60.0f, [] { return now().starting == orbit::Starting::failed; }),
         pause(0.5f, "crash-fallback-failed"), tap(Action::confirm),
         until(60.0f, ready_now), pause(2.0f, "crash-fallback-recovered")},
        true, true, false});
    // Settings, Restart download service: the backend stops, the fake loader
    // starts the next copy, and the done toast shows.
    list.push_back({"restart", Mode::normal, true,
        join({settings_at(Section::updates,
                          [] { return now().have_service_update && now().service.available; }),
              {times(12, move(Direction::down, 0.15f)), times(6, move(Direction::right, 0.15f)),
               tap(Action::confirm), move(Direction::left), pause(0.8f, "restart-confirm"),
               tap(Action::confirm),
               quick(until(20.0f,
                           [] {
                               for (const std::string &state : g_watch->restart)
                                   if (state == "stopping" || state == "starting")
                                       return true;
                               return false;
                           },
                           "restart-stopping")),
               until(90.0f, [] { return g_watch->restart_ended; }),
               pause(0.8f, "restart-done")}}),
        true, true});
    // The backend is stopped mid-session (at the mark): the banner.
    list.push_back({"connection-lost", Mode::normal, true,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::discover); }),
         pause(1.5f, "connection-lost-before"), mark("stop-backend"),
         until(30.0f, [] { return now().link == orbit::Link::offline; }),
         pause(1.5f, "connection-lost-banner")},
        true, true});
    // The backend cannot save its queue (stateHealthy false): the banner.
    list.push_back({"state-unhealthy", Mode::normal, true,
        {run(1.0f, [](orbit::App &app) { app.show_tab(orbit::tab::discover); }),
         until(30.0f, [] { return now().have_system && !now().system.state_healthy; }),
         pause(1.5f, "state-unhealthy-banner")},
        true, true});
    return list;
}

const char *saves_load_name(orbit::SavesLoad load)
{
    switch (load)
    {
    case orbit::SavesLoad::loading:
        return "loading";
    case orbit::SavesLoad::ready:
        return "ready";
    case orbit::SavesLoad::unsupported:
        return "unsupported";
    case orbit::SavesLoad::failed:
        return "failed";
    default:
        return "idle";
    }
}

std::vector<std::string> split_names(const std::string &text)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        std::string name = text.substr(start, end - start);
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (!name.empty() && name != "all")
            names.push_back(name);
        start = end + 1;
    }
    return names;
}

std::string json_list(const std::vector<std::string> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i > 0 ? "," : "") + orbit::json::quote(items[i]);
    return out + "]";
}

// One stdout line per scenario: the store's state at its end, and what it showed
// and met on the way. A harness asserts on it.
void print_state(const Scenario &scenario, bool live, orbit::Store &store, Watch &watch,
                 double seconds)
{
    using orbit::json::quote;
    const orbit::Snapshot &state = store.state();
    std::vector<std::pair<std::string, int>> jobs; // status, count, in queue order
    for (const orbit::Job &job : state.jobs)
    {
        auto at = std::find_if(jobs.begin(), jobs.end(),
                               [&job](const auto &entry) { return entry.first == job.status; });
        if (at == jobs.end())
            jobs.emplace_back(job.status, 1);
        else
            ++at->second;
    }
    std::string job_counts = "{";
    for (std::size_t i = 0; i < jobs.size(); ++i)
        job_counts += (i > 0 ? "," : "") + quote(jobs[i].first) + ":" + std::to_string(jobs[i].second);
    job_counts += "}";
    std::string errors = "[";
    for (std::size_t i = 0; i < watch.errors.size(); ++i)
        errors += std::string(i > 0 ? "," : "") + "{\"from\":" + quote(watch.errors[i].first) +
                  ",\"error\":" + quote(watch.errors[i].second) + "}";
    errors += "]";
    std::vector<std::string> log;
    {
        std::lock_guard<std::mutex> lock(watch.log_mutex);
        log = watch.log;
    }
    const orbit::LibrarySnapshot &library = state.library;
    char duration[32];
    std::snprintf(duration, sizeof(duration), "%.1f", seconds);
    const std::string line =
        "TVSTATE {\"scenario\":" + quote(scenario.name) +
        ",\"backend\":" + quote(live ? "live" : "fixture") +
        ",\"link\":" + quote(link_name(state.link)) + ",\"linkError\":" + quote(state.link_error) +
        ",\"libraryError\":" + quote(state.library_error) +
        ",\"downloadsError\":" + quote(store.action().error) +
        ",\"catalogueCount\":" + std::to_string(state.games.size()) +
        ",\"toasts\":" + json_list(watch.toasts) +
        ",\"settingError\":" + quote(store.setting_result().error) +
        ",\"libraryActionError\":" + quote(store.library_action().error) +
        ",\"favouriteError\":" + quote(store.favourite_action().error) +
        ",\"browserError\":" + quote(state.browser_error.empty() ? store.browser_action().error
                                                                 : state.browser_error) +
        ",\"stateHealthy\":" + (state.system.state_healthy ? "true" : "false") +
        ",\"paired\":" + (state.paired ? "true" : "false") +
        ",\"version\":" + quote(state.system.version) +
        ",\"instanceId\":" + quote(state.system.instance_id) +
        ",\"setupNeeded\":" + (state.setup_needed() ? "true" : "false") +
        ",\"starting\":" + quote(starting_name(state.starting)) +
        ",\"startDetail\":" + quote(state.start_detail) +
        ",\"restart\":" + quote(restart_name(state.restart)) +
        ",\"restartDetail\":" + quote(state.restart_detail) + ",\"jobs\":" + job_counts +
        ",\"saves\":" + quote(saves_load_name(store.saves().state().load)) +
        ",\"savesCount\":" + std::to_string(store.saves().state().listing.saves.size()) +
        ",\"savesBuilding\":" + (store.saves().state().listing.building ? "true" : "false") +
        ",\"savesError\":" + quote(store.saves().state().load == orbit::SavesLoad::ready
                                         ? std::string()
                                         : store.saves().state().error) +
        ",\"saveInstall\":" + quote(watch.save_install) +
        ",\"libraryGames\":" + std::to_string(state.have_library ? library.games.size() : 0) +
        ",\"storageJob\":" +
        quote(library.has_storage_job && library.storage_job.id > 0 ? library.storage_job.state
                                                                    : std::string()) +
        ",\"captures\":" + json_list(watch.captures) + ",\"marks\":" + json_list(watch.marks) +
        ",\"timeouts\":" + json_list(watch.timeouts) + ",\"errorsSeen\":" + errors +
        ",\"startingSeen\":" + json_list(watch.starting) +
        ",\"restartSeen\":" + json_list(watch.restart) + ",\"opened\":" + json_list(watch.opened) +
        ",\"starterLog\":" + json_list(log) + ",\"seconds\":" + duration + "}";
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // --list: one JSON line per scenario (where it runs, its pictures and marks),
    // for a harness; nothing is rendered.
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        for (const Scenario &scenario : scenarios())
        {
            std::vector<std::string> captures, marks;
            for (const Step &step : scenario.steps)
            {
                if (step.capture != nullptr)
                    captures.emplace_back(step.capture);
                if (step.mark != nullptr)
                    marks.emplace_back(step.mark);
            }
            std::printf("{\"name\":%s,\"fixture\":%s,\"live\":%s,\"ownWorld\":%s,\"waitReady\":%s,"
                        "\"captures\":%s,\"marks\":%s}\n",
                        orbit::json::quote(scenario.name).c_str(),
                        scenario.live_only ? "false" : "true", scenario.live_ok ? "true" : "false",
                        scenario.own_world ? "true" : "false",
                        scenario.wait_ready ? "true" : "false", json_list(captures).c_str(),
                        json_list(marks).c_str());
        }
        return 0;
    }
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s <assets dir> <output dir> [scenario[,scenario...]|all] "
                     "[width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    // XENO_SCENARIOS (a list, run in its order) wins over the argument.
    std::string wanted = argc > 3 ? argv[3] : "all";
    if (const char *list = std::getenv("XENO_SCENARIOS"); list != nullptr && *list != '\0')
        wanted = list;
    const std::vector<std::string> names = split_names(wanted);
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;
    const char *backend = std::getenv("XENO_BACKEND");
    if (backend != nullptr && *backend == '\0')
        backend = nullptr;
    // XENO_LANG=de renders the screens in German, from the app's catalogues (app/tv/i18n).
    if (const char *language = std::getenv("XENO_LANG"))
    {
        if (!orbit::i18n::load(assets + "/../i18n", language))
            std::fprintf(stderr, "language %s unavailable; using English\n", language);
    }
    const char *today = std::getenv("XENO_TODAY");
    const char *preview_version = std::getenv("XENO_APP_VERSION");
    std::error_code made;
    std::filesystem::create_directories(output, made);

    // "all": every scenario this mode can run (live: those of the default world).
    std::vector<Scenario> chosen;
    {
        std::vector<Scenario> every = scenarios();
        const auto runs_here = [backend](const Scenario &scenario)
        { return backend != nullptr ? scenario.live_ok : !scenario.live_only; };
        if (names.empty())
        {
            for (Scenario &scenario : every)
                if (runs_here(scenario) && (backend == nullptr || !scenario.own_world))
                    chosen.push_back(std::move(scenario));
        }
        for (const std::string &name : names)
        {
            const auto at = std::find_if(every.begin(), every.end(), [&name](const Scenario &s)
                                         { return name == s.name; });
            if (at == every.end())
            {
                std::fprintf(stderr, "no scenario named '%s'\n", name.c_str());
                return 2;
            }
            if (!runs_here(*at))
            {
                std::fprintf(stderr, "scenario '%s' %s\n", name.c_str(),
                             backend != nullptr ? "runs on the built-in fixture only"
                                                : "needs a live backend (XENO_BACKEND)");
                return 2;
            }
            chosen.push_back(*at);
        }
    }

    // Live mode with XENO_LOADER_PORT: the console's own starter, sending the
    // copies the XENO_STARTER_* variables name (their console paths otherwise).
    std::uint16_t loader_port = 0;
    if (const char *port = std::getenv("XENO_LOADER_PORT"); port != nullptr && *port != '\0')
        loader_port = static_cast<std::uint16_t>(std::atoi(port));
    orbit::starter::Locations where;
    for (const auto &[variable, path] :
         {std::pair{"XENO_STARTER_SAVED_ELF", &where.saved_elf},
          std::pair{"XENO_STARTER_SAVED_VERSION", &where.saved_version},
          std::pair{"XENO_STARTER_BUNDLED_ELF", &where.bundled_elf},
          std::pair{"XENO_STARTER_BUNDLED_VERSION", &where.bundled_version}})
    {
        if (const char *value = std::getenv(variable); value != nullptr && *value != '\0')
            *path = value;
    }

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font light;
    hui::gfx::Font regular;
    hui::gfx::Font medium;
    hui::gfx::Font semibold;
    orbit::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, assets + "/fonts/inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold, &type.semibold))
        return 1;
    hui::ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;
    orbit::GlTextures textures(renderer.batch());
    const std::uint32_t mark = textures.create(orbit::brand::badge(128, assets));
    const std::uint32_t loader_mark = textures.create(orbit::brand::badge(256, assets));

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);

    bool ok = true;
    for (const Scenario &scenario : chosen)
    {
        std::unique_ptr<orbit::http::Transport> transport;
        std::unique_ptr<orbit::http::Transport> art_transport;
        std::unique_ptr<orbit::http::Transport> saves_transport;
        if (backend != nullptr)
        {
            std::string address = backend;
            const std::size_t colon = address.find(':');
            const auto port = static_cast<std::uint16_t>(
                colon == std::string::npos ? 9095 : std::atoi(address.c_str() + colon + 1));
            address = address.substr(0, colon);
            transport = std::make_unique<orbit::http::SocketTransport>(address, port);
            art_transport = std::make_unique<orbit::http::SocketTransport>(address, port, 30000);
            saves_transport = std::make_unique<orbit::http::SocketTransport>(address, port, 30000);
        }
        else
        {
            // The fixture answers as XENO STORE does (no provider capture).
            transport = std::make_unique<orbit::host::Fixture>(scenario.mode,
                                                                orbit::host::Dialect::xeno);
            art_transport = std::make_unique<orbit::host::Fixture>(scenario.mode,
                                                                    orbit::host::Dialect::xeno);
            saves_transport = std::make_unique<orbit::host::Fixture>(scenario.mode,
                                                                      orbit::host::Dialect::xeno);
        }
        const std::string name = scenario.name;
        Watch watch;
        std::unique_ptr<orbit::starter::Starter> starter;
        if (backend == nullptr && (name == "starting" || name == "no-loader" || name == "updates"))
            starter = std::make_unique<PreviewStarter>(name == "no-loader"
                                                           ? orbit::starter::Outcome::no_loader
                                                           : orbit::starter::Outcome::sent);
        else if (backend != nullptr && loader_port != 0)
            starter = loader_starter(where, loader_port,
                                     [&watch](const std::string &line)
                                     {
                                         std::fprintf(stderr, "[XENO-TV] %s\n", line.c_str());
                                         std::lock_guard<std::mutex> lock(watch.log_mutex);
                                         watch.log.push_back(line);
                                     });
        else if (backend != nullptr && name == "no-loader")
            starter = std::make_unique<PreviewStarter>(orbit::starter::Outcome::no_loader);
        orbit::Store store(*transport, starter.get(), saves_transport.get());
        g_store = &store;
        g_watch = &watch;
        g_job.clear();
        orbit::ArtCache art(*art_transport, textures);
        store.start();
        art.start();
        orbit::Context context(type, fonts, store, art);
        context.mark = mark;
        context.loader_mark = loader_mark;
        context.today = today != nullptr && *today != '\0' ? today : "2026-10-04";
        // Else this folder's sce_sys/param.json, as the console reads /app0/sce_sys.
        context.app_version = preview_version != nullptr ? std::string(preview_version)
                                                          : hui::read_content_version(
                                                                assets + "/../sce_sys/param.json");
        // Preview callbacks never open websites or submit a real download; they
        // only note what would have opened, for TVSTATE.
        context.open_browser = [&watch]
        {
            watch.opened.push_back("browser");
            return true;
        };
        context.open_download = [&watch](const orbit::BrowserSelection &selection)
        {
            watch.opened.push_back("download:" + selection.release_id);
            return true;
        };
        set_open_game_browser(context,
                              [&watch](const std::string &game_id)
                              {
                                  watch.opened.push_back("game:" + game_id);
                                  return true;
                              });
        orbit::App app(context);
        orbit::Frame frame;
        frame.glass_texture = renderer.glass_texture();

        constexpr float kDt = 1.0f / 60.0f;
        hui::ui::Feedback feedback;
        // Against a live backend a frame takes as long as on the console, so
        // simulated waits are real ones.
        const useconds_t frame_sleep = backend != nullptr ? 16000 : 1500;
        const auto tick = [&](const hui::InputFrame &input)
        {
            feedback.clear();
            app.update(input, kDt, feedback);
            observe(app, watch);
            // The workers run in real time: give them a moment every frame.
            usleep(frame_sleep);
        };
        const auto render = [&](const std::string &name, bool quick)
        {
            // Draw once so the screen asks for its art, then wait (in real
            // time) until it has arrived, so pictures are complete. A quick
            // picture is taken at once: start-up screens last a moment only.
            frame.reset();
            app.draw(frame);
            for (int i = 0; !quick && i < 600 && !art.idle(); ++i)
            {
                tick({});
                frame.reset();
                app.draw(frame);
            }
            // Art fades in over real time: let it finish.
            for (int i = 0; !quick && i < 30; ++i)
                tick({});
            if (!quick)
                usleep(300000);
            tick({});
            frame.reset();
            app.draw(frame);
            renderer.begin();
            renderer.backdrop(frame.backdrop);
            renderer.draw(frame.scene);
            if (frame.glass)
                renderer.glass();
            renderer.draw(frame.overlay);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(framebuffer, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            const std::string path = output + "/" + name + ".png";
            watch.captures.push_back(name);
            ok =
                stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
            std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n",
                         path.c_str(), renderer.last_instances(), renderer.last_draw_calls(),
                         glGetError());
        };

        using Clock = std::chrono::steady_clock;
        const Clock::time_point began = Clock::now();
        const auto seconds_since = [](Clock::time_point from)
        { return std::chrono::duration<double>(Clock::now() - from).count(); };
        // Frames run until the condition holds or the time (real seconds) is up.
        const auto wait_for = [&](const std::function<bool()> &condition, float timeout,
                                  const std::string &label)
        {
            const Clock::time_point from = Clock::now();
            while (!condition() && seconds_since(from) < timeout)
                tick({});
            if (!condition())
            {
                watch.timeouts.push_back(label);
                std::fprintf(stderr, "%s: still waiting after %.0f s (%s)\n", name.c_str(),
                             static_cast<double>(timeout), label.c_str());
            }
        };
        if (backend != nullptr && scenario.wait_ready)
            wait_for(ready_now, 30.0f, "ready");
        for (std::size_t index = 0; index < scenario.steps.size(); ++index)
        {
            const Step &step = scenario.steps[index];
            const int frames = static_cast<int>(step.wait / kDt);
            for (int i = 0; i < frames; ++i)
                tick({});
            if (step.until)
            {
                // Named after the picture it waits for, or the next one.
                std::string label = "end";
                for (std::size_t next = index; next < scenario.steps.size(); ++next)
                {
                    if (scenario.steps[next].capture != nullptr)
                    {
                        label = (next == index ? "" : "before ") +
                                std::string(scenario.steps[next].capture);
                        break;
                    }
                }
                wait_for(step.until, step.timeout, label);
            }
            if (step.when && !step.when())
                continue;
            if (step.mark != nullptr)
            {
                watch.marks.emplace_back(step.mark);
                std::printf("TVMARK {\"scenario\":%s,\"mark\":%s}\n",
                            orbit::json::quote(name).c_str(),
                            orbit::json::quote(step.mark).c_str());
                std::fflush(stdout);
            }
            if (step.act)
                step.act(app);
            if (step.capture != nullptr)
                render(step.capture, step.quick);
            for (int n = 0; n < step.times; ++n)
            {
                for (int i = 0; n > 0 && i < frames; ++i)
                    tick({});
                hui::InputFrame input;
                input.connected = true;
                input.pressed = step.press;
                input.nav = step.nav;
                tick(input);
            }
        }
        art.stop();
        store.stop();
        art.clear();
        print_state(scenario, backend != nullptr, store, watch, seconds_since(began));
        g_store = nullptr;
        g_watch = nullptr;
    }
    return ok ? 0 : 1;
}
