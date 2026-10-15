#include "orbit/api.hpp"
#include "support.hpp"
#include <gtest/gtest.h>

namespace orbit {
using test::json_reply;
TEST(Zero, NativeRoutesCatalogueDownloadsAndArtWithoutSendingConsoleTokenToComputer) {
    test::ScriptedTransport local,pc;
    bool enabled=true;
    local.reply=[&](const http::Request &r) {
        if(r.path=="/api/v1/system") return json_reply(200,R"({"name":"Orbit Store","paired":true,"catalogueRevision":1})");
        if(r.path=="/api/v1/zero/companion") {
            if(r.method=="POST")enabled=false;
            return json_reply(200,std::string("{\"address\":\"192.168.1.50\",\"port\":34179,\"token\":\"")+std::string(64,'a')+"\",\"enabled\":"+(enabled?"true}":"false}"));
        }
        if(r.path=="/api/v1/downloads")return json_reply(200,R"([{"id":"local","status":"paused"}])");
        return json_reply(200,"[]");
    };
    pc.reply=[](const http::Request &r) {
        EXPECT_EQ(r.bearer,std::string(64,'a'));
        if(r.path=="/api/native/status")return json_reply(200,R"({"revision":18,"preference":"computer"})");
        if(r.path=="/api/native/catalogue")return json_reply(200,R"([{"id":"r","gameId":"game","title":"Game","directAvailable":true}])");
        if(r.path=="/api/native/download")return json_reply(200,R"({"id":"zero-test"})");
        if(r.path=="/api/native/downloads")return json_reply(200,R"([{"id":"zero-test","delivery":"zero","status":"downloading","computerReceived":80,"received":40,"downloadSpeed":30,"speed":20}])");
        if(r.path=="/api/native/art/game/hero") {auto response=json_reply(200,"pixels");response.content_type="image/png";return response;}
        return json_reply(200,"[]");
    };
    api::Client client(local,&pc);client.set_token(std::string(64,'b'));
    const auto system=client.system();ASSERT_TRUE(system.ok());EXPECT_TRUE(system.value.zero_enabled);EXPECT_EQ(system.value.catalogue_revision,18);
    EXPECT_EQ(client.catalog().value.size(),1u);
    EXPECT_EQ(client.create_download("r","internal").value,"zero-test");
    auto jobs=client.downloads();ASSERT_EQ(jobs.value.size(),2u);EXPECT_EQ(jobs.value[1].computer_received,80);EXPECT_EQ(jobs.value[1].download_speed,30);
    EXPECT_EQ(client.art("game",api::ArtKind::hero).value.bytes,"pixels");
    EXPECT_TRUE(client.job_action("zero-test","cancel",false).ok());
    auto action=pc.seen[pc.seen.size()-2];EXPECT_EQ(action.path,"/api/native/action");EXPECT_NE(action.body.find("pause"),std::string::npos);
    EXPECT_TRUE(client.job_action("zero-test","archive-password",false,"custom password").ok());
    action=pc.seen[pc.seen.size()-2];EXPECT_EQ(action.path,"/api/native/action");
    EXPECT_NE(action.body.find("\"action\":\"archive-password\""),std::string::npos);
    EXPECT_NE(action.body.find("\"password\":\"custom password\""),std::string::npos);
    EXPECT_TRUE(client.set_zero(false).ok());
    EXPECT_FALSE(client.system().value.zero_enabled);
    EXPECT_TRUE(client.catalog().ok());EXPECT_EQ(local.seen.back().path,"/api/v1/catalog");
}
TEST(Zero, MissingComputerCannotSilentlyCreateAConsoleDownload) {
    test::ScriptedTransport local,pc;
    local.reply=[](const http::Request &r) {
        if(r.path=="/api/v1/zero/companion")return json_reply(200,std::string("{\"address\":\"192.168.1.50\",\"port\":34179,\"enabled\":true,\"token\":\"")+std::string(64,'a')+"\"}");
        return json_reply(200,"{}");
    };
    pc.reply=[](const http::Request&){http::Response r;r.error="connection refused";return r;};
    api::Client client(local,&pc);client.set_token(std::string(64,'b'));
    EXPECT_FALSE(client.system().value.zero_error.empty());
    EXPECT_FALSE(client.create_download("r","internal").ok());
    EXPECT_EQ(local.count("/api/v1/downloads"),0);
}
TEST(Zero, RegistrationCannotRedirectNativeRequestsToPublicAddresses) {
    test::ScriptedTransport local,pc;
    local.reply=[](const http::Request&r) {
        if(r.path=="/api/v1/zero/companion")return json_reply(200,std::string("{\"address\":\"8.8.8.8\",\"port\":34179,\"enabled\":true,\"token\":\"")+std::string(64,'a')+"\"}");
        return json_reply(200,"{}");
    };
    api::Client client(local,&pc);client.system();EXPECT_TRUE(pc.seen.empty());
}
}
