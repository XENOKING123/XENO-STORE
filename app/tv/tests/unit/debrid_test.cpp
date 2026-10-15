// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include <gtest/gtest.h>

namespace orbit
{
namespace
{
constexpr const char *connected =
    R"({"provider":"torbox","available":true,"connected":true,"busy":false,"hosts":[{"sourceId":"archive","available":true,"maxFileSize":107374182400}]})";

TEST(Archive, ApiSendsPasswordInBodyOnlyForPasswordRetry)
{
    test::ScriptedTransport transport;
    transport.reply = [](const http::Request &) { return test::json_reply(200, "{}"); };
    api::Client client(transport);
    ASSERT_TRUE(client.job_action("job1", "archive-password", false, "custom café value").ok());
    EXPECT_EQ(transport.seen.back().path, "/api/v1/downloads/job1/archive-password");
    EXPECT_EQ(transport.seen.back().body, R"({"password":"custom café value"})");
    ASSERT_TRUE(client.job_action("job1", "pause", false, "unused").ok());
    EXPECT_EQ(transport.seen.back().body.find("unused"), std::string::npos);
}

TEST(Debrid, AvailabilityRequiresTheSelectedHostAndItsFileLimit)
{
    json::Value value;
    ASSERT_TRUE(json::parse(connected, &value));
    Debrid status = parse_debrid(value);
    Release release;
    release.source_id = "archive";
    release.size_bytes = 107374182400.0;
    EXPECT_TRUE(status.supports(release));
    release.size_bytes += 1;
    EXPECT_FALSE(status.supports(release));
    release.size_bytes = 1000;
    release.source_id = "vikingfile";
    EXPECT_FALSE(status.supports(release));
    release.source_id = "archive";
    status.busy = true;
    EXPECT_FALSE(status.supports(release));
    status.busy = false;
    status.connected = false;
    EXPECT_FALSE(status.supports(release));
}

TEST(Debrid, ApiSubmitsDeliveryOnlyAfterAnExplicitChoice)
{
    test::ScriptedTransport transport;
    transport.reply = [](const http::Request &)
    { return test::json_reply(201, R"({"id":"job1"})"); };
    api::Client client(transport);
    ASSERT_TRUE(client.create_download("release", "usb0").ok());
    EXPECT_EQ(transport.seen.back().body, R"({"releaseId":"release","storageId":"usb0"})");
    ASSERT_TRUE(client.create_download("release", "usb0", true).ok());
    EXPECT_EQ(transport.seen.back().body,
              R"({"releaseId":"release","storageId":"usb0","delivery":"torbox"})");
}

TEST(Debrid, UsesHigherWebDownloadLimitsReportedByTorBox)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"provider":"torbox","available":true,"connected":true,"hosts":[{"sourceId":"archive","available":true,"maxFileSize":214748364800}]})", &value));
    const auto status = parse_debrid(value);
    Release release;
    release.source_id = "archive";
    release.size_bytes = 150.0 * 1024 * 1024 * 1024;
    EXPECT_TRUE(status.supports(release));
    release.size_bytes = 214748364801.0;
    EXPECT_FALSE(status.supports(release));
}

TEST(Debrid, DataNodesUsesItsOwnAvailabilityAndLimits)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"provider":"torbox","available":true,"connected":true,"hosts":[{"sourceId":"archive","available":true,"maxFileSize":0},{"sourceId":"datanodes","available":true,"maxFileSize":107374182400}]})", &value));
    auto status = parse_debrid(value);
    Release release;
    release.source_id = "datanodes";
    release.browser_available = true;
    release.direct_available = false;
    release.size_bytes = 107374182400.0;
    EXPECT_TRUE(status.supports(release));
    release.size_bytes += 1;
    EXPECT_FALSE(status.supports(release));
    release.size_bytes = 1000;
    status.hosts.back().available = false;
    EXPECT_FALSE(status.supports(release));
    release.source_id = "akirabox";
    EXPECT_FALSE(status.supports(release));
}

struct DebridHarness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    GamePage page{context};
    Feedback feedback;
    bool linked = true;
    explicit DebridHarness(bool datanodes = false)
    {
        transport.reply = [this, datanodes](const http::Request &request)
        {
            if (request.path == "/api/v1/debrid")
                return test::json_reply(
                    200, linked ? datanodes
                                    ? R"({"provider":"torbox","available":true,"connected":true,"busy":false,"hosts":[{"sourceId":"datanodes","available":true,"maxFileSize":107374182400}]})"
                                    : connected
                                : R"({"provider":"torbox","available":true,"connected":false})");
            auto response = backend.send(request);
            if (datanodes && request.path == "/api/v1/catalog")
            {
                const std::string original = "\"sourceId\":\"archive\"";
                const std::string replacement = "\"sourceId\":\"datanodes\",\"browserAvailable\":true,\"directAvailable\":false";
                for (size_t at = 0; (at = response.body.find(original, at)) != std::string::npos; at += replacement.size())
                    response.body.replace(at, original.size(), replacement);
            }
            return response;
        };
        context.today = "2026-10-05";
        store.poll_once();
        store.tick();
        page.open("neon-courier");
    }
    void input(Direction nav = Direction::none, bool confirm = false)
    {
        InputFrame frame;
        frame.connected = true;
        frame.nav = nav;
        if (confirm)
            frame.pressed = hui::action_bit(Action::confirm);
        page.update(frame, 1.0f / 60.0f, feedback, true);
        Frame drawn;
        page.draw(drawn, true);
    }
    void choose_torbox()
    {
        input(Direction::none,true); // Review
        EXPECT_FALSE(store.action().busy);
        input(Direction::up); // Save to
        input(Direction::left); // Delivery
        input(Direction::none,true);
        ASSERT_TRUE(page.modal());
        input(Direction::down); // TorBox
        input(Direction::none,true); // Returns to Delivery setting
        EXPECT_TRUE(page.modal());
        input(Direction::down); // Primary

    }
};

TEST(Debrid, ControllerChoosesTorboxAndQueuesOnTheSelectedDrive)
{
    DebridHarness h;
    h.choose_torbox();
    h.input(Direction::none, true);
    ASSERT_TRUE(h.store.action().busy);
    ASSERT_TRUE(h.store.run_next_action());
    bool submitted = false;
    for (const auto &request : h.transport.seen)
    {
        if (request.method == "POST" && request.path == "/api/v1/downloads")
        {
            json::Value value;
            ASSERT_TRUE(json::parse(request.body, &value));
            EXPECT_EQ(value.text("delivery"), "torbox");
            EXPECT_EQ(value.text("releaseId"), "neon-courier-ffpfsc");
            EXPECT_FALSE(value.text("storageId").empty());
            submitted = true;
        }
    }
    EXPECT_TRUE(submitted);
}

TEST(Debrid, DataNodesTorBoxQueuesWithoutOpeningTheProviderBrowser)
{
    DebridHarness h(true);
    h.context.open_download = [](const BrowserSelection &) {
        ADD_FAILURE() << "TorBox delivery must not open provider verification";
        return false;
    };
    h.choose_torbox();
    h.input(Direction::none, true);
    ASSERT_TRUE(h.store.action().busy);
    ASSERT_TRUE(h.store.run_next_action());
    int submissions = 0;
    for (const auto &request : h.transport.seen)
    {
        if (request.method != "POST") continue;
        EXPECT_NE(request.path, "/api/v1/browser");
        if (request.path == "/api/v1/downloads")
        {
            ++submissions;
            json::Value value;
            ASSERT_TRUE(json::parse(request.body, &value));
            EXPECT_EQ(value.text("delivery"), "torbox");
            EXPECT_EQ(value.text("releaseId"), "neon-courier-ffpfsc");
            EXPECT_EQ(value.text("storageId"), "usb0");
        }
    }
    EXPECT_EQ(submissions, 1);
}

TEST(Debrid, SetupOpensTheBrowserWithTheSelectionWithoutQueuing)
{
    DebridHarness h;
    h.linked = false;
    h.store.poll_once();
    h.store.tick();
    int opened = 0;
    h.context.open_download = [&](const BrowserSelection &selection) {
        EXPECT_EQ(selection.game_id, "neon-courier");
        EXPECT_EQ(selection.release_id, "neon-courier-ffpfsc");
        EXPECT_EQ(selection.storage_id, "usb0");
        ++opened;
        return true;
    };
    h.choose_torbox();
    EXPECT_EQ(opened, 1);
    EXPECT_TRUE(h.page.modal());
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_FALSE(h.store.run_next_action());
    for (const auto &request : h.transport.seen)
        EXPECT_FALSE(request.method == "POST" && request.path == "/api/v1/downloads");
}

TEST(Debrid, DisconnectedAccountDoesNotSilentlySwitchTheDownloadToDirect)
{
    DebridHarness h;
    h.choose_torbox();
    h.linked = false;
    h.store.poll_once();
    h.store.tick();
    h.input(Direction::none, true);
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_FALSE(h.store.run_next_action());
}
} // namespace
} // namespace orbit
