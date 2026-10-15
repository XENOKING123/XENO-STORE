// XENO STORE TV app - An older store, or another program, on the store's port.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// XENO STORE 1.2.x has no /api/v1 and answers 404 to the app's status poll.
// Its port is taken, so the app cannot start its own copy either. The app
// leaves the opening screen, names the store from GET /api/status, offers to
// replace it, and keeps polling so the screen clears after an update.

#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "orbit/starter.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace orbit
{
namespace
{

using starter::Outcome;

class CountingStarter final : public starter::Starter
{
  public:
    std::atomic<int> calls{0};
    starter::Result start() override
    {
        ++calls;
        starter::Result result;
        result.outcome = Outcome::sent;
        result.source = "bundled";
        result.version = "0.6.0";
        result.detail = "sent";
        return result;
    }
};

enum class Port : int
{
    free,    // nothing listens: connection refused
    legacy,  // XENO STORE 1.2.1
    current, // a store with this app's API (the fixture)
    other,   // the test's own reply to every request
};

struct Console
{
    host::Fixture fixture{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    std::atomic<Port> port{Port::legacy};
    int stop_status = 200;
    http::Response other;
    Console()
    {
        transport.reply = [this](const http::Request &request)
        {
            switch (port.load())
            {
            case Port::free:
                break;
            case Port::current:
                return fixture.send(request);
            case Port::other:
                return other;
            case Port::legacy:
                if (request.method == "GET" && request.path == "/api/status")
                    return test::json_reply(200, R"({"app":"XENO STORE","version":"1.2.1","port":9095})");
                if (request.method == "POST" && request.path == "/api/system/stop")
                    return stop_status == 200
                               ? test::json_reply(200, R"({"ok":true,"stopping":true})")
                               : test::json_reply(stop_status, R"({"ok":false,"error":"pin_required"})");
                return test::json_reply(404, R"({"ok":false,"error":"not_found","message":"Unknown API route"})");
            }
            http::Response refused;
            refused.error = "connection refused";
            return refused;
        };
    }
    int stops()
    {
        int n = 0;
        for (const http::Request &request : transport.seen)
            n += request.method == "POST" && request.path == "/api/system/stop" ? 1 : 0;
        return n;
    }
};

TEST(StoreOutdated, AnOlderStoreLeavesTheOpeningScreenAndClearsAfterAnUpdate)
{
    Console console;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::offline);
    EXPECT_TRUE(store.state().outdated);
    EXPECT_EQ(store.state().outdated_version, "1.2.1");
    EXPECT_EQ(store.state().starting, Starting::idle);
    EXPECT_EQ(starter.calls, 0); // its port is taken: a copy sent now would only exit
    store.poll_once();
    store.tick();
    EXPECT_TRUE(store.state().outdated);
    EXPECT_EQ(starter.calls, 0);

    // Updated in the browser version: the screen clears by itself.
    console.port = Port::current;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_FALSE(store.state().outdated);
    EXPECT_TRUE(store.state().outdated_version.empty());
    EXPECT_EQ(starter.calls, 0);
}

TEST(StoreOutdated, AnOlderStoreThatQuitsIsReplacedByThisAppsCopy)
{
    Console console;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    console.port = Port::free; // stopped from the browser version
    store.poll_once();
    store.tick();
    EXPECT_FALSE(store.state().outdated);
    EXPECT_EQ(starter.calls, 1); // the automatic start was still unused
    EXPECT_EQ(store.state().starting, Starting::waiting);
}

TEST(StoreOutdated, OtherWholeRepliesAreAnotherProgramButBusyRepliesAreNot)
{
    Console console;
    console.port = Port::other;
    CountingStarter starter;
    Store store(console.transport, &starter);

    console.other = test::json_reply(403, R"({"error":"forbidden"})");
    store.poll_once();
    store.tick();
    EXPECT_TRUE(store.state().outdated);
    EXPECT_TRUE(store.state().outdated_version.empty()); // not XENO STORE's /api/status
    EXPECT_EQ(store.state().link, Link::offline);

    Store garbled(console.transport, &starter);
    console.other = test::json_reply(200, "<html>a router page</html>");
    garbled.poll_once();
    garbled.tick();
    EXPECT_TRUE(garbled.state().outdated);
    EXPECT_TRUE(garbled.state().outdated_version.empty());

    Store busy(console.transport, &starter);
    console.other = test::json_reply(503, R"({"error":"XENO STORE is stopping."})");
    busy.poll_once();
    busy.tick();
    EXPECT_FALSE(busy.state().outdated);
    EXPECT_EQ(busy.state().link, Link::connecting);
    EXPECT_EQ(starter.calls, 0);
}

TEST(StoreOutdated, ReplacingAnOlderStoreStopsItThenStartsThisAppsCopy)
{
    Console console;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.tick();
    ASSERT_TRUE(store.state().outdated);
    store.replace_outdated();
    EXPECT_TRUE(store.setting_result().busy);
    store.run_next_action();
    store.tick();
    EXPECT_EQ(console.stops(), 1);
    EXPECT_FALSE(store.setting_result().busy);
    EXPECT_TRUE(store.setting_result().error.empty());
    EXPECT_EQ(store.state().restart, Restart::stopping);
    store.poll_once(); // it answers for a moment after agreeing to stop
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    EXPECT_EQ(starter.calls, 0);
    console.port = Port::free;
    store.poll_once(); // stopped
    store.tick();
    EXPECT_FALSE(store.state().outdated);
    EXPECT_EQ(store.state().restart, Restart::starting);
    store.poll_once(); // sends this app's copy
    EXPECT_EQ(starter.calls, 1);
    console.port = Port::current;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "0.6.0");
    EXPECT_EQ(starter.calls, 1);
}

TEST(StoreOutdated, AnOlderStoreWithAPinIsNotStopped)
{
    Console console;
    console.stop_status = 401;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.replace_outdated();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.setting_result().setting, Setting::service_replace);
    EXPECT_NE(store.setting_result().error.find("PIN"), std::string::npos);
    EXPECT_EQ(store.state().restart, Restart::idle);
    store.poll_once();
    store.tick();
    EXPECT_TRUE(store.state().outdated);
    EXPECT_EQ(starter.calls, 0);
}

TEST(StoreOutdated, AnOldCopyComingBackDuringARestartEndsIt)
{
    Console console;
    console.port = Port::current;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.port = Port::free;
    store.poll_once(); // stopped
    store.poll_once(); // sends a copy
    EXPECT_EQ(starter.calls, 1);
    console.port = Port::legacy; // a payload manager started 1.2.1 first
    store.poll_once();
    store.tick();
    EXPECT_TRUE(store.state().outdated);
    EXPECT_EQ(store.state().restart, Restart::idle);
    EXPECT_EQ(store.state().starting, Starting::idle);
    store.poll_once();
    EXPECT_EQ(starter.calls, 1); // nothing more is sent while the port is taken
}

TEST(StoreOutdated, TheScreenOffersToReplaceTheStoreAndCrossDoesIt)
{
    Console console;
    CountingStarter starter;
    test::TestFonts fonts;
    test::FakeSink sink;
    Store store(console.transport, &starter);
    ArtCache art(console.fixture, sink);
    Context context(fonts.type, fonts.fonts, store, art);
    App app(context);
    Feedback feedback;
    store.poll_once();
    const auto frame = [&](std::uint32_t press)
    {
        InputFrame input;
        input.connected = true;
        input.pressed = press;
        feedback.clear();
        app.update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app.draw(drawn);
        EXPECT_FALSE(drawn.scene.instances().empty());
    };
    frame(0);
    ASSERT_TRUE(store.state().outdated);
    frame(hui::action_bit(Action::confirm)); // the only button: Replace it now
    EXPECT_FALSE(feedback.cues.empty());
    EXPECT_TRUE(store.run_next_action());
    EXPECT_EQ(console.stops(), 1);
    EXPECT_EQ(starter.calls, 0);
    frame(0);
}

} // namespace
} // namespace orbit
