// Native provider sessions use the same authenticated service workflow.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "orbit/api.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

namespace orbit
{
using test::json_reply;
using test::ScriptedTransport;

TEST(ProviderBrowser, StartAndCancelCarryOnlySelectedIdsAndTheSessionToken)
{
    ScriptedTransport transport;
    transport.reply=[](const http::Request &) {
        return json_reply(202,R"({"available":true,"active":true,"id":"s1","state":"waiting","title":"Test game","jobId":"","message":"Waiting"})");
    };
    api::Client client(transport);client.set_token("local-token");
    EXPECT_TRUE(client.start_browser("r1","m2").ok());
    EXPECT_EQ(transport.seen.back().body,R"({"action":"start","releaseId":"r1","storageId":"m2"})");
    EXPECT_EQ(transport.seen.back().bearer,"local-token");
    EXPECT_TRUE(client.cancel_browser("s1").ok());
    EXPECT_EQ(transport.seen.back().body,R"({"action":"cancel","id":"s1"})");
}

TEST(ProviderBrowser, RejectsMalformedStatusAndPreservesRefusalReason)
{
    ScriptedTransport transport;api::Client client(transport);
    transport.reply=[](const http::Request &) { return json_reply(200,R"({"available":true})"); };
    EXPECT_FALSE(client.browser_session().ok());
    transport.reply=[](const http::Request &) { return json_reply(200,R"({"state":"waiting","active":true})"); };
    EXPECT_FALSE(client.browser_session().ok());
    transport.reply=[](const http::Request &) { return json_reply(409,R"({"error":"Finish or cancel the current verification first."})"); };
    EXPECT_EQ(client.start_browser("r","usb0").error,"Finish or cancel the current verification first.");
}

TEST(ProviderBrowser, AcceptsAkiraReadyLinkWaitAndAutomaticQueueCompletion)
{
    ScriptedTransport transport;
    api::Client client(transport);
    transport.reply=[](const http::Request &) {
        return json_reply(202,R"({"available":true,"active":true,"id":"akira1","state":"waiting-link","title":"Test game","message":"Complete verification on AkiraBox, then wait for Orbit to check the download link."})");
    };
    const auto waiting = client.start_browser("akira-release", "internal");
    ASSERT_TRUE(waiting.ok());
    EXPECT_EQ(waiting.value.state, "waiting-link");
    EXPECT_TRUE(waiting.value.active);
    EXPECT_EQ(waiting.value.message, "Complete verification on AkiraBox, then wait for Orbit to check the download link.");
    transport.reply=[](const http::Request &) {
        return json_reply(200,R"({"available":true,"active":false,"id":"akira1","state":"queued","title":"Test game","jobId":"download1","message":"Your verified file has been added to Downloads."})");
    };
    const auto queued = client.browser_session();
    ASSERT_TRUE(queued.ok());
    EXPECT_FALSE(queued.value.active);
    EXPECT_EQ(queued.value.job_id, "download1");
    EXPECT_EQ(transport.seen.size(), 2u);
    EXPECT_EQ(transport.seen.back().method, "GET");
}

TEST(ProviderBrowser, LostStartReplyIsReconciledWithoutReplayingOrQueueing)
{
    host::Fixture fixture(host::FixtureMode::normal);
    ScriptedTransport transport;
    int posts=0;
    transport.reply=[&](const http::Request &request) {
        auto response=fixture.send(request);
        if(request.path=="/api/v1/browser" && request.method=="POST") {
            ++posts;response={};response.error="connection interrupted";
        }
        return response;
    };
    Store store(transport);store.poll_once();store.tick();
    store.start_browser("ironclad-skies-viking","usb0");
    store.start_browser("ironclad-skies-viking","usb0"); // repeated X
    ASSERT_TRUE(store.run_next_action());store.tick();
    EXPECT_FALSE(store.run_next_action());
    EXPECT_TRUE(store.state().browser.active);
    EXPECT_EQ(store.browser_action().error,"connection interrupted");
    store.poll_once();store.tick();
    EXPECT_EQ(posts,1);
    EXPECT_EQ(store.state().jobs.size(),6u);
    EXPECT_FALSE(store.action().busy);
}

TEST(ProviderBrowser, OlderBackendAndReadFailureDisableNewStartsWithoutFakeCompletion)
{
    host::Fixture fixture(host::FixtureMode::normal);
    ScriptedTransport transport;
    transport.reply=[&](const http::Request &request) {
        return request.path=="/api/v1/browser" ? json_reply(404,R"({"error":"Not found"})") : fixture.send(request);
    };
    Store store(transport);store.poll_once();store.tick();
    EXPECT_FALSE(store.state().have_browser);
    EXPECT_FALSE(store.state().browser.available);
    EXPECT_EQ(store.state().link,Link::online);
    EXPECT_EQ(store.state().browser_error,"Update Orbit to open provider pages.");
}
} // namespace orbit
