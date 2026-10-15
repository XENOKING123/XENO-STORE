// XENO STORE TV app - Settings: the simultaneous downloads stepper.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// XENO STORE runs 1 to 4 downloads at once. The stepper in Settings, Storage
// stops at 4, the API client refuses anything outside 1..4, and a larger
// limit reported by an older backend steps down into that range.

#include "fixture.hpp"
#include "orbit/api.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

// The app against the fixture, with the console's download limit and its
// drives (none, so the stepper is the second row) under the test's control.
struct SettingsHarness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::FakeSink sink;
    test::ScriptedTransport transport;
    int limit;
    std::vector<std::string> posted; // bodies of POST /api/v1/settings/downloads
    Store store{transport};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit SettingsHarness(int start) : limit(start)
    {
        transport.reply = [this](const http::Request &request)
        {
            if (request.path == "/api/v1/storage")
                return test::json_reply(200, "[]");
            if (request.path == "/api/v1/settings/downloads" && request.method == "POST")
            {
                posted.push_back(request.body);
                json::Value value;
                if (json::parse(request.body, &value))
                    limit = static_cast<int>(value["concurrentDownloads"].as_int());
                return test::json_reply(200, "{\"concurrentDownloads\":" + std::to_string(limit) + "}");
            }
            http::Response response = backend.send(request);
            if (request.path == "/api/v1/system" && request.method == "GET")
            {
                // Replace any limit the fixture reports with the test's.
                const std::string key = "\"concurrentDownloads\":";
                const auto at = response.body.find(key);
                if (at != std::string::npos)
                {
                    auto end = response.body.find_first_of(",}", at);
                    if (response.body[end] == ',')
                        ++end;
                    response.body.erase(at, end - at);
                }
                response.body.insert(1, key + std::to_string(limit) + ",");
            }
            return response;
        };
        context.today = "2026-10-04";
        context.app_version = "01.002.001";
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
    void settle(int frames = 20)
    {
        for (int i = 0; i < frames; ++i)
            frame();
    }
    // Storage: "Choose automatically", then Decrease and Increase on one row.
    void focus_stepper()
    {
        app.open_settings(SettingsScreen::Section::storage, true);
        settle();
        frame(Direction::down);  // the stepper's row
        frame(Direction::right); // Increase (it stays there if already on it)
    }
};

TEST(SettingsStepper, IncreasesUpToFourAndNoFurther)
{
    SettingsHarness h(3);
    ASSERT_EQ(h.store.state().system.concurrent_downloads, 3);
    h.focus_stepper();
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.run_next_action());
    h.settle(3);
    ASSERT_EQ(h.posted, std::vector<std::string>({R"({"concurrentDownloads":4})"}));
    EXPECT_EQ(h.store.state().system.concurrent_downloads, 4);

    // At 4, Increase is refused: nothing is sent.
    h.store.poll_once();
    h.settle(3);
    h.press(Action::confirm);
    EXPECT_FALSE(h.store.setting_result().busy);
    EXPECT_FALSE(h.store.run_next_action());
    EXPECT_EQ(h.posted.size(), 1u);
    EXPECT_EQ(h.store.state().system.concurrent_downloads, 4);
}

TEST(SettingsStepper, ALargerLimitFromAnOlderBackendStepsDownIntoRange)
{
    SettingsHarness h(8);
    ASSERT_EQ(h.store.state().system.concurrent_downloads, 8);
    h.focus_stepper();
    h.frame(Direction::left); // Decrease
    h.press(Action::confirm);
    ASSERT_TRUE(h.store.run_next_action());
    h.settle(3);
    EXPECT_EQ(h.posted, std::vector<std::string>({R"({"concurrentDownloads":4})"}));
    EXPECT_EQ(h.store.state().system.concurrent_downloads, 4);
}

TEST(SettingsStepper, TheClientAcceptsOnlyOneToFour)
{
    test::ScriptedTransport transport;
    int reply = 4;
    transport.reply = [&](const http::Request &)
    { return test::json_reply(200, "{\"concurrentDownloads\":" + std::to_string(reply) + "}"); };
    api::Client client(transport);
    for (const int refused : {0, 5, 8})
    {
        const api::Result<int> result = client.set_download_limit(refused);
        EXPECT_EQ(result.error, "Choose a whole number from 1 to 4.");
    }
    EXPECT_TRUE(transport.seen.empty()); // refused before sending
    const api::Result<int> four = client.set_download_limit(4);
    ASSERT_TRUE(four.ok()) << four.error;
    EXPECT_EQ(four.value, 4);
    reply = 5; // a reply outside the range is not taken as saved
    EXPECT_EQ(client.set_download_limit(2).error, "Could not save the setting.");
}

} // namespace
} // namespace orbit
