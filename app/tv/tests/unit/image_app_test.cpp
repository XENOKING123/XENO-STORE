// Orbit Store TV app - Image decoding, the mark, and the app driven by input.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.

#include "fixture.hpp"
#include "orbit/image.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include "../third_party/stb/stb_image_write.h"

#include <webp/encode.h>

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace orbit
{
namespace
{

void append(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

std::vector<std::uint8_t> solid(int width, int height, std::uint8_t r, std::uint8_t g,
                                std::uint8_t b)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    return rgba;
}

TEST(Image, DecodesPngAndFitsItToTheDrawingSize)
{
    const std::vector<std::uint8_t> rgba = solid(800, 400, 200, 40, 10);
    std::string png;
    stbi_write_png_to_func(append, &png, 800, 400, 4, rgba.data(), 800 * 4);
    EXPECT_EQ(image::sniff(png), image::Format::png);
    image::Pixels pixels;
    ASSERT_TRUE(image::decode(png, 400, 400, &pixels));
    EXPECT_EQ(pixels.width, 400);
    EXPECT_EQ(pixels.height, 200);
    EXPECT_NEAR(pixels.rgba[0], 200, 1);
    EXPECT_NEAR(pixels.rgba[1], 40, 1);
    // Small images are never enlarged.
    ASSERT_TRUE(image::decode(png, 4000, 4000, &pixels));
    EXPECT_EQ(pixels.width, 800);
}

TEST(Image, DecodesWebpAtTheRequestedSize)
{
    const std::vector<std::uint8_t> rgba = solid(512, 512, 30, 120, 240);
    std::uint8_t *encoded = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(rgba.data(), 512, 512, 512 * 4, &encoded);
    ASSERT_GT(size, 0u);
    const std::string webp(reinterpret_cast<const char *>(encoded), size);
    WebPFree(encoded);
    EXPECT_EQ(image::sniff(webp), image::Format::webp);
    image::Pixels pixels;
    ASSERT_TRUE(image::decode(webp, 400, 400, &pixels));
    EXPECT_EQ(pixels.width, 400);
    EXPECT_EQ(pixels.height, 400);
    EXPECT_NEAR(pixels.rgba[2], 240, 2);
}

TEST(Image, RefusesWhatIsNotAnImage)
{
    image::Pixels pixels;
    std::string error;
    EXPECT_FALSE(image::decode("<html>blocked</html>", 100, 100, &pixels, &error));
    EXPECT_EQ(error, "unsupported image format");
    EXPECT_FALSE(image::decode(std::string("RIFF\0\0\0\0WEBPjunk", 16), 100, 100, &pixels, &error));
    EXPECT_FALSE(
        image::decode(std::string("\x89PNG\r\n\x1a\nbroken", 14), 100, 100, &pixels, &error));
    EXPECT_TRUE(pixels.empty());
}

TEST(Image, ResizeAveragesAndAmbientIsASoftDarkenedWash)
{
    image::Pixels checker;
    checker.width = checker.height = 4;
    checker.rgba.resize(64);
    for (int i = 0; i < 16; ++i)
    {
        const std::uint8_t v = (i + i / 4) % 2 ? 255 : 0;
        checker.rgba[static_cast<std::size_t>(i) * 4] = v;
        checker.rgba[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    const image::Pixels one = image::resize(checker, 1, 1);
    EXPECT_NEAR(one.rgba[0], 128, 1);
    const image::Pixels wash = image::ambient(checker, 16);
    EXPECT_EQ(wash.width, 16);
    EXPECT_LT(wash.rgba[0], 128); // darkened
    EXPECT_EQ(wash.rgba[3], 255);
}

// ---- the whole app, driven by controller input against the fixture ----

struct AppHarness
{
    test::TestFonts fonts;
    host::Fixture backend;
    test::FakeSink sink;
    test::ScriptedTransport transport;
    std::string storage_override;
    std::string browser_override;
    Store store{transport};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit AppHarness(host::FixtureMode mode = host::FixtureMode::normal) : backend(mode)
    {
        transport.reply = [this](const http::Request &request)
        {
            if (request.path == "/api/v1/storage" && !storage_override.empty())
                return test::json_reply(200, storage_override);
            if (request.path == "/api/v1/browser" && request.method == "GET" && !browser_override.empty())
                return test::json_reply(200, browser_override);
            return backend.send(request);
        };
        context.today = "2026-10-04";
        context.app_version = "01.000.000";
        store.poll_once();
        frame();
    }
    void frame(Direction nav = Direction::none, std::uint32_t press = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = press;
        feedback.clear();
        app.update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app.draw(drawn);
        for (const auto &instance : drawn.scene.instances())
            ASSERT_TRUE(std::isfinite(instance.rect[0]) && std::isfinite(instance.rect[1]));
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    void settle(int frames = 30)
    {
        for (int i = 0; i < frames; ++i)
            frame();
    }
};

TEST(App, OpensAGameAndQueuesADownloadToTheDownloadsTab)
{
    AppHarness h;
    h.app.open_game("neon-courier");
    h.settle();
    h.press(Action::confirm); // Review options, focused on the enabled Download button.
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_EQ(h.transport.count("/api/v1/downloads"), 1); // initial GET only
    h.press(Action::confirm); // Explicitly start.
    ASSERT_TRUE(h.store.action().busy);
    h.store.run_next_action();
    h.settle(5);
    EXPECT_EQ(h.store.state().jobs.size(), 7u);
    // The new job is shown in Downloads with a confirmation.
    bool queued = false;
    for (const Job &job : h.store.state().jobs)
        queued = queued || (job.release_id == "neon-courier-ffpfsc" && job.status == "queued");
    EXPECT_TRUE(queued);
}

TEST(App, GameDetailsQueuesTheSelectedEditionAndDrive)
{
    AppHarness h;
    h.app.open_game("ember-hollow");
    h.settle();
    h.press(Action::confirm); // Download options
    h.frame(Direction::up); // Save to
    h.frame(Direction::left); // Delivery
    h.frame(Direction::left); // Source
    h.press(Action::confirm);
    h.frame(Direction::down); // version 1.05
    h.press(Action::confirm); // Return to Source setting
    h.frame(Direction::right);h.frame(Direction::right); // Save to
    h.press(Action::confirm);
    h.frame(Direction::down); // Internal storage
    h.press(Action::confirm);
    h.frame(Direction::down); // Disabled Download is skipped.
    h.press(Action::confirm); // Drive choices remain available.
    EXPECT_FALSE(h.store.action().busy);
    h.frame(Direction::up); // USB has enough room.
    h.press(Action::confirm);
    h.frame(Direction::down); // Download
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.action().busy);
    h.store.run_next_action();
    h.settle(5);
    bool matched = false;
    for (const Job &job : h.store.state().jobs)
        matched = matched || (job.release_id == "ember-hollow-ffpfsc-105" &&
                              job.storage_id == "usb0" && job.status == "queued");
    EXPECT_TRUE(matched);
}

TEST(App, NativeVikingStartsVerificationWithTheChosenDriveWithoutOpeningOrbitBrowser)
{
    AppHarness h;
    h.storage_override = R"([{"id":"usb0","label":"USB Extended Storage","path":"/mnt/usb0/homebrew","freeBytes":1400000000000,"totalBytes":2000000000000,"projectedFreeBytes":1400000000000,"external":true},{"id":"internal","label":"Internal storage","path":"/data/homebrew","freeBytes":500000000000,"totalBytes":1000000000000,"projectedFreeBytes":500000000000,"external":false}])";
    h.store.poll_once();
    h.settle();
    int browser_opens = 0;
    h.context.open_download = [&](const BrowserSelection &selection)
    {
        (void)selection;
        ++browser_opens;
        return true;
    };
    h.app.open_game("ironclad-skies");
    h.settle();
    h.press(Action::confirm); // Review does not open the browser.
    EXPECT_EQ(browser_opens,0);
    h.frame(Direction::up); // Save to
    h.press(Action::confirm);
    h.frame(Direction::down); // Internal storage, not the default USB
    h.press(Action::confirm);
    h.frame(Direction::down); // Open Vikingfile on PS5
    h.press(Action::confirm);
    EXPECT_EQ(browser_opens,0);
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_TRUE(h.store.browser_action().busy);
    h.store.run_next_action();
    h.settle();
    bool started = false;
    for (const auto &request : h.transport.seen)
        if (request.method == "POST" && request.path == "/api/v1/browser")
        {
            EXPECT_EQ(request.body,R"({"action":"start","releaseId":"ironclad-skies-viking","storageId":"internal"})");
            started = true;
        }
    EXPECT_TRUE(started);
    EXPECT_TRUE(h.store.state().browser.active);
    EXPECT_EQ(h.store.state().jobs.size(),6u); // A page opening is not a download.
    h.press(Action::back); // Close progress, keep original selection.
    const std::string original_storage = h.storage_override;
    h.storage_override = "[]"; // Reopening/cancelling still works if the drive disappears.
    h.store.poll_once();h.settle();
    h.press(Action::confirm); // Reopen current verification; no second POST.
    EXPECT_FALSE(h.store.run_next_action());
    h.frame(Direction::left);
    h.press(Action::confirm); // Cancel exactly this verification.
    EXPECT_TRUE(h.store.run_next_action());
    h.settle();
    EXPECT_EQ(h.store.state().browser.state,"cancelled");
    h.press(Action::back);
    h.storage_override = original_storage;
    h.store.poll_once();h.settle(); // Dismissed terminal result must not reappear.
    h.press(Action::confirm); // Original option and chosen drive can be retried.
    EXPECT_TRUE(h.store.browser_action().busy);
}

TEST(App, BrowserHandoffNeverSubstitutesADisconnectedDrive)
{
    AppHarness h;
    int opened = 0;
    h.context.open_download = [&](const BrowserSelection &)
    {
        ++opened;
        return true;
    };
    h.app.open_game("ironclad-skies"); // USB selected initially
    h.settle();
    h.storage_override = R"([{"id":"internal","label":"Internal storage","path":"/data/homebrew","freeBytes":500000000000,"totalBytes":1000000000000,"projectedFreeBytes":500000000000,"external":false}])";
    h.store.poll_once();
    h.settle();
    h.press(Action::confirm); // Disabled primary falls back to Source.
    EXPECT_EQ(opened,0);
    EXPECT_FALSE(h.store.action().busy);
    h.frame(Direction::right);h.frame(Direction::right); // Save to
    h.press(Action::confirm);
    h.press(Action::confirm); // Explicitly select remaining internal drive.
    h.frame(Direction::down);
    h.press(Action::confirm);
    EXPECT_EQ(opened,0);
    EXPECT_TRUE(h.store.browser_action().busy);
    EXPECT_FALSE(h.store.action().busy);
}

TEST(App, NativeVikingQueuedResultOpensTheExactValidatedJob)
{
    AppHarness h;
    h.app.open_game("ironclad-skies");h.settle();
    h.press(Action::confirm);h.press(Action::confirm);
    h.store.run_next_action();h.settle();
    h.browser_override=R"({"available":true,"active":false,"id":"verification-1","title":"Ironclad Skies","state":"queued","jobId":"j1","message":"Your verified file has been added to Downloads."})";
    h.store.poll_once();h.settle();
    h.frame(Direction::left);h.press(Action::confirm);
    EXPECT_EQ(h.app.current_tab(),tab::downloads);
    EXPECT_FALSE(h.store.action().busy);
}

TEST(App, NativeVikingLostSessionWaitsForExplicitRetry)
{
    AppHarness h;
    h.app.open_game("ironclad-skies");h.settle();
    h.press(Action::confirm);h.press(Action::confirm);
    ASSERT_TRUE(h.store.run_next_action());h.settle();
    // The service restarted: there is no session to resume or replay.
    h.browser_override=R"({"available":true,"active":false,"id":"","state":"idle","jobId":"","message":""})";
    h.store.poll_once();h.settle();
    EXPECT_FALSE(h.store.run_next_action());
    EXPECT_FALSE(h.store.state().browser.active);
    h.press(Action::back);
    h.store.poll_once();h.settle();
    EXPECT_FALSE(h.store.run_next_action());
    h.press(Action::confirm); // Same game/drive; only the user's click retries.
    EXPECT_TRUE(h.store.browser_action().busy);
}

TEST(App, NativeVikingDelayedStartDoesNotUndoClosingTheProgressPanel)
{
    AppHarness h;
    h.app.open_game("ironclad-skies");h.settle();
    h.press(Action::confirm);h.press(Action::confirm);
    h.press(Action::back); // Close before the POST/status response arrives.
    ASSERT_TRUE(h.store.run_next_action());h.settle();
    ASSERT_TRUE(h.store.state().browser.active);
    h.press(Action::confirm); // Explicitly reopen View verification.
    h.frame(Direction::left);h.press(Action::confirm);
    EXPECT_TRUE(h.store.run_next_action()); // Cancel, not an underlying game control.
    h.settle();
    EXPECT_EQ(h.store.state().browser.state,"cancelled");
}

// XENO STORE has no provider capture on the console: /browser is idle with
// available:false and a message. The page shows that message in place of the
// three steps, and its primary button opens the game in the browser version.
std::string idle_browser(const std::string &message)
{
    return R"({"available":false,"active":false,"id":"","state":"idle","title":"","jobId":"","message":)" +
           json::quote(message) + "}";
}

std::size_t drawn_glyphs(AppHarness &h)
{
    Frame frame;
    h.app.draw(frame);
    std::size_t glyphs = 0;
    for (const hui::gfx::DrawList *list : {&frame.scene, &frame.overlay})
        for (const auto &instance : list->instances())
            glyphs += instance.params[3] == static_cast<float>(hui::gfx::Shape::glyph) ? 1u : 0u;
    return glyphs;
}

TEST(App, BrowserOnlyOptionShowsTheBackendsMessageWithoutProviderCapture)
{
    const std::string message = "Vikingfile asks for a human check, so XENO STORE cannot fetch it by "
                                "itself. Download it with TorBox, or open this game in the browser "
                                "version.";
    const std::string extra = " Thanks for waiting.";
    std::size_t glyphs[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        AppHarness h;
        h.browser_override = idle_browser(i == 0 ? message : message + extra);
        h.store.poll_once();
        h.settle();
        ASSERT_TRUE(h.store.state().have_browser);
        ASSERT_FALSE(h.store.state().browser.available);
        h.app.open_game("ironclad-skies");
        h.settle();
        h.press(Action::confirm); // Download options
        h.settle();
        glyphs[i] = drawn_glyphs(h);
    }
    // Only the message differs, so the second page draws exactly its extra letters.
    std::size_t letters = 0;
    for (const char c : extra)
        letters += c == ' ' ? 0u : 1u;
    EXPECT_EQ(glyphs[1], glyphs[0] + letters);
}

TEST(App, BrowserOnlyOptionOpensTheGameInTheBrowserVersion)
{
    AppHarness h;
    h.browser_override = idle_browser("Vikingfile asks for a human check.");
    h.store.poll_once();
    h.settle();
    std::vector<std::string> opened;
    h.context.open_game_browser = [&](const std::string &game_id)
    {
        opened.push_back(game_id);
        return true;
    };
    h.app.open_game("ironclad-skies");
    h.settle();
    h.press(Action::confirm); // Download options, focused on Open browser version
    h.press(Action::confirm);
    ASSERT_EQ(opened.size(), 1u);
    EXPECT_EQ(opened[0], "ironclad-skies");
    EXPECT_EQ(browser_game_url(opened[0]), "http://127.0.0.1:9095/?tv=1#game/ironclad-skies");
    // Nothing is queued and no provider page is started.
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_FALSE(h.store.browser_action().busy);
    EXPECT_FALSE(h.store.run_next_action());
    for (const http::Request &request : h.transport.seen)
        EXPECT_FALSE(request.method == "POST" &&
                     (request.path == "/api/v1/browser" || request.path == "/api/v1/downloads"));

    // Where the browser cannot be opened, the button stays disabled.
    AppHarness plain;
    plain.browser_override = idle_browser("Vikingfile asks for a human check.");
    plain.store.poll_once();
    plain.settle();
    plain.app.open_game("ironclad-skies");
    plain.settle();
    plain.press(Action::confirm);
    plain.press(Action::confirm);
    EXPECT_FALSE(plain.store.browser_action().busy);
    EXPECT_FALSE(plain.store.run_next_action());
}

TEST(App, CircleLeavesTheGamePageAndTabsSwitchWithL1R1)
{
    AppHarness h;
    EXPECT_EQ(h.app.current_tab(), orbit::tab::discover);
    h.app.open_game("tidewater");
    h.settle();
    h.press(Action::back);
    h.settle();
    // From Discover, Browse comes next, then Saves; four presses reach Downloads.
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), orbit::tab::browse);
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), orbit::tab::saves);
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), orbit::tab::library);
    h.press(Action::page_next);
    h.settle();
    EXPECT_EQ(h.app.current_tab(), orbit::tab::downloads);
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), orbit::tab::downloads);
    EXPECT_FALSE(h.feedback.cues.empty());
}

TEST(App, AFavouriteFromAGamePageShowsUnderBrowseFavourites)
{
    AppHarness h;
    h.app.open_game("neon-courier");
    h.settle();
    h.frame(Direction::right); // Favourite beside the main button
    h.press(Action::confirm); // Add to favourites
    ASSERT_TRUE(h.store.favourite_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_TRUE(h.store.state().favourite("neon-courier"));
    // (Circle would return to the game's tile; start from the search field.)
    h.app.show_tab(orbit::tab::browse);
    h.settle();
    h.frame(Direction::down); // Download-size filter beneath Search
    h.frame(Direction::left); // Format
    h.frame(Direction::left); // Source
    h.frame(Direction::left); // Favourites, on the same toolbar
    h.press(Action::confirm);
    h.settle(3);
    EXPECT_TRUE(h.app.browse().filters().favourites);
    EXPECT_EQ(h.app.browse().filters().query, "");
}

TEST(App, BrowseRegionPickerReturnsToMatchingGrid)
{
    AppHarness h;
    BrowseScreen screen(h.context);
    screen.focus();
    const auto input = [&](Direction direction, bool confirm = false)
    {
        InputFrame frame;
        frame.connected = true;
        frame.nav = direction;
        if (confirm)
            frame.pressed = hui::action_bit(Action::confirm);
        return screen.update(frame, 1.0f / 60.0f, h.feedback, true);
    };
    input(Direction::down); // Region, directly below Search
    input(Direction::none, true);
    input(Direction::down); // EUR
    input(Direction::none, true);
    EXPECT_EQ(screen.filters().region, "EUR");
    input(Direction::down); // leave closed dropdown for matching grid
    EXPECT_EQ(input(Direction::none, true).id, "ember-hollow");
}

TEST(App, DiscoverFavouriteTogglesFromTheHeroWithoutOpeningAGame)
{
    AppHarness h;
    h.app.show_tab(orbit::tab::discover);
    h.settle();
    const bool before = h.store.state().favourite("tidewater");
    h.frame(Direction::right); // Heart beside View game
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.favourite_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().favourite("tidewater"), !before);
    h.press(Action::confirm); // The same control can remove it again.
    ASSERT_TRUE(h.store.favourite_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().favourite("tidewater"), before);
}

// A favourite saved here says so in a toast, added or removed; one saved on a
// paired phone arrives with a poll and shows nothing.
TEST(App, AFavouriteSavedHereShowsAToastAndOneFromElsewhereDoesNot)
{
    AppHarness h;
    h.app.show_tab(orbit::tab::discover);
    h.settle();
    EXPECT_EQ(h.app.toast_left(), 0.0f); // nothing at start-up
    const bool before = h.store.state().favourite("tidewater");
    h.frame(Direction::right); // Heart beside View game
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.favourite_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.app.toast(), before ? "Removed from favourites" : "Added to favourites");
    EXPECT_GT(h.app.toast_left(), 0.0f);
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.favourite_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.app.toast(), before ? "Added to favourites" : "Removed from favourites");
    h.settle(200); // the toast has gone
    ASSERT_EQ(h.app.toast_left(), 0.0f);
    http::Request phone;
    phone.method = "POST";
    phone.path = "/api/v1/favorites";
    phone.bearer = h.store.state().token;
    phone.body = "{\"gameId\":\"neon-courier\",\"favorite\":" +
                 std::string(h.store.state().favourite("neon-courier") ? "false" : "true") + "}";
    ASSERT_EQ(h.backend.send(phone).status, 200);
    const auto earlier = h.store.state().favourites;
    h.store.poll_once();
    h.settle(3);
    EXPECT_NE(h.store.state().favourites, earlier);
    EXPECT_EQ(h.app.toast_left(), 0.0f);
}

TEST(App, BrowsePlatformPickerOpensOnlyTheMatchingEdition)
{
    AppHarness h;
    BrowseScreen screen(h.context);
    screen.focus();
    const auto input = [&](Direction direction, bool confirm = false)
    {
        InputFrame frame;
        frame.connected = true;
        frame.nav = direction;
        if (confirm) frame.pressed = hui::action_bit(Action::confirm);
        return screen.update(frame, 1.0f / 60.0f, h.feedback, true);
    };
    input(Direction::down); // Region, below Search
    input(Direction::right); input(Direction::right); input(Direction::right); // Platform
    input(Direction::none, true);
    input(Direction::down); input(Direction::down); // PS4
    input(Direction::none, true);
    EXPECT_EQ(screen.filters().platform, "ps4");
    input(Direction::down); // Closed picker returns to the matching grid.
    EXPECT_EQ(input(Direction::none, true).id, "copper-circuit-ps4");
    // The same title exists as a separate PS5 edition; the filter must not open it.
    input(Direction::up); input(Direction::up); // Collection, then Search.
    input(Direction::down);
    input(Direction::right); input(Direction::right); input(Direction::right);
    input(Direction::none, true);
    input(Direction::up); input(Direction::none, true); // PS5
    EXPECT_EQ(screen.filters().platform, "ps5");
    input(Direction::down);
    EXPECT_NE(input(Direction::none, true).id, "copper-circuit-ps4");
}

TEST(App, SquareTogglesTheHighlightedBrowseGameAndIgnoresRepeatedPendingPresses)
{
    AppHarness h;
    BrowseScreen screen(h.context);
    screen.focus();
    screen.return_to("neon-courier");
    InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(Action::west);
    EXPECT_EQ(screen.update(input, 1.0f / 60, h.feedback, true).kind, Intent::Kind::none);
    ASSERT_TRUE(h.store.favourite_action().busy);
    screen.update(input, 1.0f / 60, h.feedback, true);
    ASSERT_TRUE(h.store.run_next_action());
    EXPECT_FALSE(h.store.run_next_action());
    h.store.tick();
    EXPECT_TRUE(h.store.state().favourite("neon-courier"));
    screen.update(input, 1.0f / 60, h.feedback, true);
    ASSERT_TRUE(h.store.run_next_action());
    h.store.tick();
    EXPECT_FALSE(h.store.state().favourite("neon-courier"));
    screen.update(input, 1.0f / 60, h.feedback, false);
    EXPECT_FALSE(h.store.run_next_action());
}

TEST(App, SquareOnDiscoverTogglesTheHeroAndTheSelectedTileWithoutOpeningDetails)
{
    AppHarness h;
    DiscoverScreen screen(h.context);
    screen.focus();
    InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(Action::west);
    const auto before = h.store.state().favourites;
    EXPECT_EQ(screen.update(input, 1.0f / 60, h.feedback, true).kind, Intent::Kind::none);
    ASSERT_TRUE(h.store.run_next_action());
    h.store.tick();
    EXPECT_NE(h.store.state().favourites, before);
    screen.return_to("neon-courier");
    const bool tile_before = h.store.state().favourite("neon-courier");
    EXPECT_EQ(screen.update(input, 1.0f / 60, h.feedback, true).kind, Intent::Kind::none);
    ASSERT_TRUE(h.store.run_next_action());
    h.store.tick();
    EXPECT_EQ(h.store.state().favourite("neon-courier"), !tile_before);
}

TEST(App, BrowseSquareKeepsSearchClearAndDropdownBehaviour)
{
    AppHarness h;
    BrowseScreen screen(h.context);
    screen.focus();
    screen.set_query("neon");
    InputFrame input;
    input.connected = true;
    input.pressed = hui::action_bit(Action::west);
    screen.update(input, 1.0f / 60, h.feedback, true);
    EXPECT_TRUE(screen.filters().query.empty());
    EXPECT_FALSE(h.store.run_next_action());
    input.pressed = 0;
    input.nav = Direction::down;
    screen.update(input, 1.0f / 60, h.feedback, true);
    input.nav = Direction::none;
    input.pressed = hui::action_bit(Action::confirm);
    screen.update(input, 1.0f / 60, h.feedback, true);
    input.pressed = hui::action_bit(Action::west);
    screen.update(input, 1.0f / 60, h.feedback, true);
    EXPECT_FALSE(h.store.run_next_action());
}

TEST(App, LibraryMountsAGameFromItsDetails)
{
    AppHarness h;
    h.app.show_tab(orbit::tab::library);
    h.settle();
    ASSERT_TRUE(h.store.state().library.ready());
    h.app.library().show_game("PPSA90004", std::string(64, '4')); // Glass Orchard
    h.settle();
    ASSERT_TRUE(h.app.library().details_open());
    h.press(Action::confirm); // Mount game
    ASSERT_TRUE(h.store.library_action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().library.action.action, "mount");
    EXPECT_EQ(h.store.state().library.action.title_id, "PPSA90004");
    h.press(Action::back);
    h.settle();
    EXPECT_FALSE(h.app.library().details_open());
}

TEST(App, ViewInLibraryOpensTheGamesLibraryDetails)
{
    AppHarness h;
    h.app.open_game("quiet-harbour"); // an exact copy is in the Library
    h.settle();
    h.press(Action::confirm); // Review
    h.press(Action::confirm); // View in Library
    h.settle();
    EXPECT_EQ(h.app.current_tab(), orbit::tab::library);
    EXPECT_TRUE(h.app.library().details_open());
}

TEST(App, DownloadsCancelDialogKeepsThePartial)
{
    AppHarness h;
    h.app.show_tab(orbit::tab::downloads);
    h.settle();
    h.frame(Direction::right); // Cancel on the first (downloading) row
    h.press(Action::confirm);  // opens the dialog
    h.settle();
    h.press(Action::confirm); // Keep partial file
    ASSERT_TRUE(h.store.action().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().jobs[0].status, "cancelled");
}

TEST(App, CancelledDownloadDeletionRequiresConfirmationAndRemovesTheEntry)
{
    AppHarness h;
    const std::string id = h.store.state().jobs.back().id;
    ASSERT_EQ(h.store.state().jobs.back().status, "cancelled");
    h.app.show_tab(orbit::tab::downloads);
    h.app.downloads().show_job(id);
    h.settle();
    h.frame(Direction::right); // Delete download
    h.press(Action::confirm);
    EXPECT_TRUE(h.app.downloads().modal());
    EXPECT_FALSE(h.store.action().busy);
    h.frame(Direction::left); // Go back is initially focused; select Delete.
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.action().busy);
    h.store.run_next_action(); h.settle(3);
    EXPECT_EQ(h.transport.count("/api/v1/downloads/" + id + "/delete"), 1);
    EXPECT_EQ(h.store.state().jobs.size(), 5u);
    EXPECT_FALSE(h.app.downloads().modal());
}

TEST(App, CancelledDeletionFailureStaysVisibleAndHistoryOnlyNeedsAnotherConfirmation)
{
    AppHarness h;
    const std::string id = h.store.state().jobs.back().id;
    h.transport.reply = [&](const http::Request &request)
    {
        if (request.path == "/api/v1/downloads/" + id + "/delete")
            return test::json_reply(409, R"({"error":"Reconnect the original drive before deleting the partial file."})");
        return h.backend.send(request);
    };
    h.app.show_tab(orbit::tab::downloads);
    h.app.downloads().show_job(id);
    h.settle();
    h.frame(Direction::right); h.press(Action::confirm);
    h.frame(Direction::left); h.press(Action::confirm);
    h.store.run_next_action(); h.settle(3);
    EXPECT_TRUE(h.app.downloads().modal());
    EXPECT_FALSE(h.store.action().error.empty());
    EXPECT_EQ(h.store.state().jobs.size(), 6u);
    h.frame(Direction::left); // Remove from history
    h.press(Action::confirm); // Opens a second confirmation; no request yet.
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_EQ(h.transport.count("/api/v1/downloads/" + id + "/forget"), 0);
    h.frame(Direction::right); h.press(Action::confirm);
    ASSERT_TRUE(h.store.action().busy);
    h.store.run_next_action(); h.settle(3);
    EXPECT_EQ(h.transport.count("/api/v1/downloads/" + id + "/forget"), 1);
    EXPECT_EQ(h.store.state().jobs.size(), 5u);
    EXPECT_FALSE(h.app.downloads().modal());
}

// ---- App settings ----

TEST(App, FirstRunChoosesSourcesInTheAppAndOpensDiscover)
{
    AppHarness h(host::FixtureMode::setup);
    h.settle();
    h.press(Action::confirm); // Choose sources
    h.settle();
    h.frame(Direction::right); // Archive.org (after Vikingfile)
    h.press(Action::confirm);
    h.frame(Direction::down); // the download notice
    h.press(Action::confirm);
    h.frame(Direction::down); // Save sources
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.setting_result().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_FALSE(h.store.state().setup_needed());
    EXPECT_EQ(h.store.state().sources.enabled, (std::vector<std::string>{"archive"}));
    // Saving finished setting up: back to Discover.
    h.store.poll_once();
    h.settle();
    EXPECT_EQ(h.app.current_tab(), orbit::tab::discover);
    h.press(Action::back); // nothing left open to close
    h.settle();
    EXPECT_EQ(h.store.state().games.size(), 13u);
}

TEST(App, SavingSourcesNeedsTheNoticeAccepted)
{
    AppHarness h(host::FixtureMode::setup);
    h.settle();
    h.press(Action::confirm);
    h.settle();
    h.press(Action::confirm); // Vikingfile
    h.frame(Direction::down); // the notice, left unaccepted
    h.frame(Direction::down); // Save sources
    h.press(Action::confirm); // refused
    EXPECT_FALSE(h.store.setting_result().busy);
}

TEST(App, SettingsChoosesTheDefaultDrive)
{
    AppHarness h;
    h.app.open_settings(SettingsScreen::Section::storage, true);
    h.settle();
    h.frame(Direction::down); // USB Extended Storage
    h.frame(Direction::down); // Internal storage
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.setting_result().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().system.preferred_storage, "internal");
    // Circle leaves the section, then closes App settings.
    h.press(Action::back);
    h.press(Action::back);
    h.settle();
    EXPECT_EQ(h.app.current_tab(), orbit::tab::discover);
}

TEST(App, UpdatesInstallTheTvAppAndRestartTheServiceAfterConfirming)
{
    AppHarness h(host::FixtureMode::updates);
    h.store.poll_once();
    h.app.open_settings(SettingsScreen::Section::updates, true);
    h.settle();
    h.frame(Direction::right); // Update TV app
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.setting_result().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().tv_app.installed_version, "1.1.0");
    h.frame(Direction::down);  // the download service's buttons
    h.frame(Direction::right); // Install update
    h.press(Action::confirm);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_TRUE(h.store.state().service.restart_required);
    // Restart needs a second, explicit choice; Circle backs out of it.
    h.frame(Direction::right); // Restart download service
    h.press(Action::confirm);
    EXPECT_FALSE(h.store.setting_result().busy);
    h.press(Action::back);
    h.settle();
    EXPECT_EQ(h.store.state().restart, Restart::idle);
    h.press(Action::confirm); // Restart download service again
    h.frame(Direction::left); // Restart now
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.setting_result().busy);
    h.store.run_next_action();
    h.settle(3);
    EXPECT_EQ(h.store.state().restart, Restart::stopping);
}

TEST(App, SurvivesRandomInputEverywhere)
{
    AppHarness h;
    std::mt19937 random(7);
    const Direction directions[] = {Direction::none, Direction::up, Direction::down,
                                    Direction::left, Direction::right};
    const Action actions[] = {Action::confirm,   Action::back,      Action::north,
                              Action::west,      Action::page_prev, Action::page_next,
                              Action::jump_prev, Action::jump_next};
    for (int i = 0; i < 3000; ++i)
    {
        const Direction nav = directions[random() % 5];
        const std::uint32_t press = random() % 4 == 0 ? hui::action_bit(actions[random() % 8]) : 0;
        h.frame(nav, press);
        if (h.store.action().busy)
            h.store.run_next_action();
        if (i % 500 == 0)
            h.store.poll_once();
    }
    SUCCEED();
}

} // namespace
} // namespace orbit

TEST(App, BrowserHandoffCannotChangeTheLocalOriginOrSmuggleACommand)
{
    // TorBox setup opens XENO STORE's Settings; nothing selected reaches the address.
    const std::string settings = "http://127.0.0.1:9095/?tv=1#settings";
    EXPECT_EQ(orbit::browser_handoff_url({"game-a", "ppsa12345-viking", "usb1"}), settings);
    EXPECT_EQ(orbit::browser_handoff_url({"game-a", "r&start=true", "usb1"}), settings);
    EXPECT_EQ(orbit::browser_handoff_url({"https://example.com", "r", "usb1"}), settings);
    EXPECT_EQ(orbit::browser_handoff_url({"game-a", "r", ""}), settings);
    EXPECT_EQ(orbit::browser_handoff_url({std::string(64, 'a'), "r", "usb1"}), settings);
    // A game's page: the ID is percent-encoded, whatever its length.
    EXPECT_EQ(orbit::browser_game_url("ironclad-skies"),
              "http://127.0.0.1:9095/?tv=1#game/ironclad-skies");
    EXPECT_EQ(orbit::browser_game_url("a b/c?d#e&f.g_h~i"),
              "http://127.0.0.1:9095/?tv=1#game/a%20b%2Fc%3Fd%23e%26f.g_h~i");
    EXPECT_EQ(orbit::browser_game_url("caf\xC3\xA9"), "http://127.0.0.1:9095/?tv=1#game/caf%C3%A9");
    const std::string long_id(70, 'x');
    EXPECT_EQ(orbit::browser_game_url(long_id), "http://127.0.0.1:9095/?tv=1#game/" + long_id);
    EXPECT_TRUE(orbit::browser_game_url("").empty());
}
