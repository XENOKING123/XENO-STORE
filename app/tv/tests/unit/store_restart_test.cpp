// XENO STORE TV app - What a restart of the download service expects back.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings restarts XENO STORE: it stops (pausing its downloads), then the app
// sends a copy to the ELF loader and checks the version that comes back: the
// saved update the stop reply promised, or else the version that was running.
// A copy older than that (a payload sender had started a newer release) is
// reported, not accepted. A saved update that never answers is set aside, so
// "Try again" sends the bundled copy.

#include "fixture.hpp"
#include "orbit/starter.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

namespace orbit
{
namespace
{

using starter::Outcome;

// Sends one copy each time it is asked: the saved copy unless it is avoided.
class Starter final : public starter::Starter
{
  public:
    std::string saved_version = "1.2.2";
    std::string bundled_version = "1.2.1";
    bool saved_readable = true;
    std::vector<std::string> sent; // the source of each copy sent
    starter::Result start() override
    {
        const bool saved = saved_readable && !avoid_saved();
        starter::Result result;
        result.outcome = Outcome::sent;
        result.source = saved ? "saved" : "bundled";
        result.version = saved ? saved_version : bundled_version;
        result.detail = "sent";
        sent.push_back(result.source);
        return result;
    }
};

// The backend: refusing connections until `online`, then the fixture with this
// instance and version. Its stop reply carries running and saved versions.
struct Console
{
    host::Fixture fixture{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    std::atomic<bool> online{true};
    std::string version, instance = "first", saved;
    Console(std::string running, std::string saved_update)
        : version(std::move(running)), saved(std::move(saved_update))
    {
        transport.reply = [this](const http::Request &request)
        {
            if (!online)
            {
                http::Response response;
                response.error = "connection refused";
                return response;
            }
            if (request.method == "POST" && request.path == "/api/v1/updates")
                return test::json_reply(202, "{\"phase\":\"stopping\",\"instanceId\":\"" + instance +
                                                 "\",\"runningVersion\":\"" + version +
                                                 "\",\"savedVersion\":\"" + saved + "\"}");
            auto response = fixture.send(request);
            if (request.path == "/api/v1/system")
            {
                const auto at = response.body.find('{');
                response.body.insert(at + 1, "\"instanceId\":\"" + instance + "\",");
                const auto old = response.body.find("0.6.0");
                if (old != std::string::npos)
                    response.body.replace(old, 5, version);
            }
            return response;
        };
    }
};

// Stops the service, lets its port close and sends a copy: the store is then
// waiting for an answer.
void restart_until_sent(Console &console, Store &store)
{
    store.set_restart_grace(0.0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once(); // stopped
    store.poll_once(); // sends a copy
}

TEST(StoreRestartExpectation, AnOlderCopyThanTheOneRunningIsReportedNotAccepted)
{
    // 1.2.3 was started by a payload sender; nothing is saved, the package has 1.2.1.
    Console console("1.2.3", "");
    Starter starter;
    starter.saved_readable = false;
    Store store(console.transport, &starter);
    restart_until_sent(console, store);
    ASSERT_EQ(starter.sent, std::vector<std::string>({"bundled"}));
    console.online = true;
    console.instance = "second";
    console.version = "1.2.1";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::older);
    EXPECT_NE(store.state().restart_detail.find("1.2.3"), std::string::npos);
    EXPECT_NE(store.state().restart_detail.find("1.2.1"), std::string::npos);
    // No saved copy exists, so none is named.
    EXPECT_EQ(store.state().restart_detail.find("/data/xeno-store"), std::string::npos);
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_EQ(starter.sent.size(), 1u);
}

TEST(StoreRestartExpectation, WithoutASavedUpdateTheSameReleaseRestartsAsDone)
{
    Console console("1.2.1", "");
    Starter starter;
    starter.saved_readable = false;
    Store store(console.transport, &starter);
    restart_until_sent(console, store);
    console.online = true;
    console.instance = "second";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.2.1");
}

TEST(StoreRestartExpectation, AMismatchWithoutASavedUpdateNamesNoSavedFile)
{
    // The bundled 1.2.1 was sent for a 1.2.1 restart, but another copy answers.
    Console console("1.2.1", "");
    Starter starter;
    starter.saved_readable = false;
    Store store(console.transport, &starter);
    restart_until_sent(console, store);
    console.online = true;
    console.instance = "second";
    console.version = "1.3.0";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_NE(store.state().restart_detail.find("1.3.0"), std::string::npos);
    EXPECT_EQ(store.state().restart_detail.find("/data/xeno-store"), std::string::npos);
}

TEST(StoreRestartExpectation, APromisedSavedUpdateKeepsTheStrictCheck)
{
    // The stop reply promised 1.2.2, but only the older bundled copy could be sent.
    Console console("1.2.1", "1.2.2");
    Starter starter;
    starter.saved_readable = false;
    Store store(console.transport, &starter);
    restart_until_sent(console, store);
    console.online = true;
    console.instance = "second";
    console.version = "1.2.1";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_NE(store.state().restart_detail.find("1.2.2"), std::string::npos);
    EXPECT_NE(store.state().restart_detail.find("/data/xeno-store/xeno-store.elf"), std::string::npos);
}

TEST(StoreRestartExpectation, ThePromisedSavedUpdateRestartsAsExpected)
{
    Console console("1.2.1", "1.2.2");
    Starter starter;
    Store store(console.transport, &starter);
    restart_until_sent(console, store);
    ASSERT_EQ(starter.sent, std::vector<std::string>({"saved"}));
    console.online = true;
    console.instance = "second";
    console.version = "1.2.2";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.2.2");
}

TEST(StoreRestartExpectation, ASavedUpdateThatNeverAnswersIsReplacedByTheBundledCopy)
{
    Console console("1.2.1", "1.2.2");
    Starter starter;
    Store store(console.transport, &starter);
    store.set_answer_waits(0.0, 0.0);
    restart_until_sent(console, store);
    store.poll_once(); // no answer: sent once more
    store.poll_once(); // still none
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_EQ(starter.sent, std::vector<std::string>({"saved", "saved"}));
    EXPECT_TRUE(starter.avoid_saved());
    store.start_orbit(); // Try again
    store.poll_once();
    EXPECT_EQ(starter.sent, std::vector<std::string>({"saved", "saved", "bundled"}));
    // The bundled 1.2.1 starts and sets the crashing update aside.
    console.online = true;
    console.instance = "second";
    console.version = "1.2.1";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.2.1");
    EXPECT_FALSE(starter.avoid_saved());
}

TEST(StoreRestartExpectation, ASlowSavedUpdateAnsweringAfterTheFallbackIsASuccess)
{
    Console console("1.2.1", "1.2.2");
    Starter starter;
    Store store(console.transport, &starter);
    store.set_answer_waits(0.0, 0.0);
    restart_until_sent(console, store);
    store.poll_once();
    store.poll_once();
    store.start_orbit(); // Try again: the bundled 1.2.1 stands in
    store.poll_once();
    ASSERT_EQ(starter.sent.back(), "bundled");
    // The saved 1.2.2 was only slow, and answers after all.
    console.online = true;
    console.instance = "second";
    console.version = "1.2.2";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.2.2");
}

} // namespace
} // namespace orbit
