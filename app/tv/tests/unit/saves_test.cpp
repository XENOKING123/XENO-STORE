// XENO STORE TV app - Saves: the Save Data Store's model, client, worker and tab.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The community saves come from GET /api/saves/store (outside /api/v1); an
// install is a console job the app follows (POST /api/saves/store/{id}/install,
// GET /api/saves/jobs/{id}, POST .../cancel). These tests read the store's
// replies with the app's own model, drive the saves worker step by step, and
// play the Saves tab and a game page's Community saves with the controller.

#include "fixture.hpp"
#include "orbit/api.hpp"
#include "orbit/json.hpp"
#include "orbit/save_store.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <string>

namespace orbit
{
namespace
{

const char *const kListing = R"({"ok":true,"backend":"supabase","backendName":"XenoKing",
 "building":false,"count":3,
 "sources":[{"kind":"github","rows":2,"ok":true},{"kind":"supabase","name":"XenoKing","rows":1,"ok":false}],
 "saves":[
  {"id":"gh_1","titleId":"PPSA04029","gameId":"cyber","title":"Cyberpunk 2077","name":"Chapter 4",
   "description":"All weapons","author":"nick","category":"chapter","gameVersion":"01.004.000",
   "region":"EUR","size":1234567,"createdAt":"2026-10-01T08:00:00Z","downloads":12,"source":"github",
   "installed":true,"installedVersion":"01.004.000","versionCompare":0,"compat":"match",
   "compatText":"Matches your 01.004.000"},
  {"titleId":"PPSA00001","name":"no id: left out"},
  {"id":"sb:5a1d","titleId":"PPSA04029","title":"Cyberpunk 2077","name":"Max start",
   "description":"New game+ with everything maxed","author":"ann","category":"","size":"big",
   "createdAt":"2026-10-09T10:00:00Z","source":"supabase","sourceName":"XenoKing","installed":false,
   "compat":"not_installed","otherRegion":true,"installedTitleId":"PPSA04030"},
  {"id":"gh_2","titleId":"PPSA05555","title":"Glass Tide","name":"Undated","description":"a glitched tide",
   "author":"zed","installed":true,"compat":"unknown"}
 ]})";

SaveListing listing(const char *text = kListing)
{
    json::Value value;
    EXPECT_TRUE(json::parse(text, &value));
    return parse_save_listing(value);
}

SaveRow row(const char *compat, int compare = 0, bool other_region = false)
{
    SaveRow save;
    save.id = "s";
    save.title = "Ember Hollow";
    save.title_id = "PPSA90003";
    save.game_version = "01.050.000";
    save.installed_version = "01.040.000";
    save.compat = compat;
    save.version_compare = compare;
    save.other_region = other_region;
    save.installed_title_id = "PPSA90103";
    return save;
}

// ---- the model ----

TEST(SavesModel, ReadsTheStoresListingTolerantly)
{
    const SaveListing saves = listing();
    EXPECT_EQ(saves.backend, "supabase");
    EXPECT_EQ(saves.backend_name, "XenoKing");
    EXPECT_FALSE(saves.building);
    ASSERT_EQ(saves.sources.size(), 2u);
    EXPECT_TRUE(saves.sources[0].ok);
    EXPECT_FALSE(saves.sources[1].ok);
    EXPECT_EQ(saves.sources[1].name, "XenoKing");
    // A row without an ID cannot be installed: it is left out.
    ASSERT_EQ(saves.saves.size(), 3u);
    const SaveRow &first = saves.saves[0];
    EXPECT_EQ(first.id, "gh_1");
    EXPECT_EQ(first.game_id, "cyber");
    EXPECT_EQ(first.author, "nick");
    EXPECT_EQ(first.game_version, "01.004.000");
    EXPECT_DOUBLE_EQ(first.size, 1234567.0);
    EXPECT_EQ(first.downloads, 12);
    EXPECT_TRUE(first.installed);
    EXPECT_EQ(first.compat, "match");
    const SaveRow &site = saves.saves[1];
    EXPECT_EQ(site.id, "sb:5a1d");
    EXPECT_EQ(site.source_name, "XenoKing");
    EXPECT_DOUBLE_EQ(site.size, 0.0); // a mistyped size reads as unknown
    EXPECT_TRUE(site.other_region);
    EXPECT_EQ(site.installed_title_id, "PPSA04030");
}

TEST(SavesModel, ReadsProfilesAndJobs)
{
    json::Value users;
    ASSERT_TRUE(json::parse(R"({"ok":true,"users":[{"id":268435456,"name":"XENO","foreground":true},
        {"id":0,"name":"none"},{"id":-4,"name":"bad"},{"id":99999999999,"name":"too big"},
        {"id":268435457,"name":"Guest"}]})",
                            &users));
    const std::vector<SaveProfile> profiles = parse_save_profiles(users);
    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(profiles[0].id, 268435456u);
    EXPECT_TRUE(profiles[0].foreground);
    EXPECT_EQ(profiles[1].name, "Guest");
    EXPECT_FALSE(profiles[1].foreground);

    json::Value job;
    ASSERT_TRUE(json::parse(R"({"id":"s_ab","type":"save-install","state":"done","message":"Save installed",
        "error":"","errorCode":"","done":5,"total":5,"percent":150,
        "result":{"tier":"garlic","note":"Re-signed for your profile.","preBackup":"/data/b.zip"}})",
                            &job));
    const SaveJob parsed = parse_save_job(job);
    EXPECT_EQ(parsed.id, "s_ab");
    EXPECT_TRUE(parsed.finished());
    EXPECT_EQ(parsed.percent, 100);
    EXPECT_EQ(parsed.note, "Re-signed for your profile.");
    EXPECT_EQ(parsed.pre_backup, "/data/b.zip");
    SaveJob running;
    running.state = "running";
    EXPECT_FALSE(running.finished());
}

TEST(SavesModel, CompatibilityFollowsTheBrowserVersion)
{
    EXPECT_EQ(save_compat(row("match")), SaveCompat::match);
    EXPECT_EQ(save_compat(row("match", -1)), SaveCompat::older_save);
    EXPECT_EQ(save_compat(row("update_game", 1)), SaveCompat::update_game);
    EXPECT_EQ(save_compat(row("not_installed")), SaveCompat::not_installed);
    EXPECT_EQ(save_compat(row("not_installed", 0, true)), SaveCompat::other_region);
    EXPECT_EQ(save_compat(row("unknown")), SaveCompat::unknown);
    EXPECT_EQ(save_compat(row("")), SaveCompat::unknown);
    for (int i = 0; i <= static_cast<int>(SaveCompat::unknown); ++i)
        EXPECT_STRNE(save_compat_label(static_cast<SaveCompat>(i)), "");
    // The sentences name the versions and the other region.
    const std::string update = save_compat_sentence(row("update_game", 1));
    EXPECT_NE(update.find("01.050.000"), std::string::npos) << update;
    EXPECT_NE(update.find("01.040.000"), std::string::npos) << update;
    EXPECT_NE(save_compat_sentence(row("not_installed", 0, true)).find("PPSA90103"), std::string::npos);
    EXPECT_NE(save_compat_sentence(row("match")).find("01.050.000"), std::string::npos);
}

TEST(SavesModel, CategoriesAreReadOrInferredAsTheStoreDoes)
{
    SaveRow save;
    save.category = "Modded";
    EXPECT_EQ(save_category(save), SaveCategory::modded);
    save.category.clear();
    const std::pair<const char *, SaveCategory> inferred[] = {
        {"100% complete", SaveCategory::platinum}, {"Platinum run", SaveCategory::platinum},
        {"NG+ start", SaveCategory::starter},      {"everything maxed", SaveCategory::starter},
        {"before the boss", SaveCategory::checkpoint}, {"glitched coins", SaveCategory::modded},
        {"Chapter 3", SaveCategory::chapter},      {"a quiet evening", SaveCategory::other},
    };
    for (const auto &[text, category] : inferred)
    {
        save.description = text;
        EXPECT_EQ(save_category(save), category) << text;
    }
    // The chips list each category the saves hold once, in a fixed order.
    const std::vector<SaveCategory> present = save_categories(listing().saves);
    ASSERT_EQ(present.size(), 3u);
    EXPECT_EQ(present[0], SaveCategory::starter);
    EXPECT_EQ(present[1], SaveCategory::modded);
    EXPECT_EQ(present[2], SaveCategory::chapter);
}

TEST(SavesModel, FiltersSearchesAndListsNewestFirst)
{
    const std::vector<SaveRow> saves = listing().saves;
    SaveFilters all;
    // Newest first; the undated save last.
    EXPECT_EQ(filter_saves(saves, all), (std::vector<int>{1, 0, 2}));
    SaveFilters search;
    search.query = "  ANN ";
    EXPECT_EQ(filter_saves(saves, search), (std::vector<int>{1}));
    search.query = "ppsa05555";
    EXPECT_EQ(filter_saves(saves, search), (std::vector<int>{2}));
    SaveFilters mine;
    mine.mine = true;
    EXPECT_EQ(filter_saves(saves, mine), (std::vector<int>{0, 2}));
    SaveFilters category;
    category.category = static_cast<int>(SaveCategory::starter);
    EXPECT_EQ(filter_saves(saves, category), (std::vector<int>{1}));
    // One game: its gameId, or one of its options' title IDs.
    Game cyber;
    cyber.id = "cyber";
    SaveFilters game;
    game.game = &cyber;
    EXPECT_EQ(filter_saves(saves, game), (std::vector<int>{0}));
    Release release;
    release.title_id = "ppsa04029";
    cyber.releases.push_back(release);
    EXPECT_EQ(filter_saves(saves, game), (std::vector<int>{1, 0}));
    EXPECT_EQ(count_saves(saves, cyber), 2);
}

TEST(SavesModel, AnEndedInstallSaysWhatHappened)
{
    SaveJob job;
    job.state = "done";
    EXPECT_EQ(save_install_outcome(job, "Ember Hollow"), "Installed. Start the game to use it.");
    job.state = "cancelled";
    job.error_code = "cancelled";
    EXPECT_EQ(save_install_outcome(job, "Ember Hollow"), "Install cancelled.");
    job.state = "failed";
    job.error_code = "title_running";
    job.error = "PPSA90003 is running: close the game";
    const std::string running = save_install_outcome(job, "Ember Hollow");
    EXPECT_NE(running.find("Ember Hollow is running"), std::string::npos) << running;
    EXPECT_NE(running.find("Close the game"), std::string::npos) << running;
    // Garlic Save Manager and the rest: the store's own sentence.
    job.error_code = "garlic_unavailable";
    job.error = "This save needs Garlic Save Manager to be re-signed for your account: it is not running";
    EXPECT_EQ(save_install_outcome(job, "Ember Hollow"), job.error);
    job.error_code = "download_failed";
    job.error.clear();
    EXPECT_EQ(save_install_outcome(job, "Ember Hollow"), "The save could not be installed.");
}

TEST(SavesModel, SizesUnderAMegabyteAreKilobytes)
{
    // As the browser version's Saves: a 3 KB save never reads "0.0 MB".
    EXPECT_EQ(format_save_size(3278.0), "3.3 KB");
    EXPECT_EQ(format_save_size(34420.0), "34 KB");
    EXPECT_EQ(format_save_size(999400.0), "999 KB");
    EXPECT_EQ(format_save_size(1653856.0), "1.7 MB");
    EXPECT_EQ(format_save_size(67108864.0), format_bytes(67108864.0));
}

// ---- the client ----

TEST(SavesApi, EncodesTheSaveIdAndAlwaysBacksUpFirst)
{
    EXPECT_EQ(api::save_path_segment("sb:5a1d-00_x.y~"), "sb%3A5a1d-00_x.y~");
    EXPECT_EQ(api::save_path_segment("a/b c"), "a%2Fb%20c");
    EXPECT_EQ(api::save_path_segment(""), "");
    EXPECT_EQ(api::save_path_segment(std::string(96, 'a')), "");
    EXPECT_EQ(api::save_path_segment("."), "");

    test::ScriptedTransport transport;
    transport.reply = [](const http::Request &request)
    {
        if (request.path == "/api/saves/users")
            return test::json_reply(200, R"({"ok":true,"users":[{"id":7,"name":"P","foreground":true}]})");
        return test::json_reply(200, R"({"ok":true,"job":{"id":"s_1","type":"save-install","state":"queued"}})");
    };
    api::Client client(transport);
    client.set_token("tok");
    const api::Result<SaveJob> started = client.install_save("sb:5a1d", 268435457u);
    ASSERT_TRUE(started.ok()) << started.error;
    EXPECT_EQ(started.value.id, "s_1");
    const http::Request &install = transport.seen.back();
    EXPECT_EQ(install.method, "POST");
    EXPECT_EQ(install.path, "/api/saves/store/sb%3A5a1d/install");
    EXPECT_EQ(install.bearer, "tok");
    EXPECT_EQ(install.body, R"({"user":268435457,"backupFirst":true,"wait":false})");
    EXPECT_TRUE(client.save_job("s_1").ok());
    EXPECT_EQ(transport.seen.back().path, "/api/saves/jobs/s_1");
    EXPECT_EQ(transport.seen.back().method, "GET");
    EXPECT_TRUE(client.cancel_save_job("s_1").ok());
    EXPECT_EQ(transport.seen.back().path, "/api/saves/jobs/s_1/cancel");
    EXPECT_EQ(transport.seen.back().method, "POST");
    // An ID the path cannot carry is refused before anything is sent.
    const std::size_t sent = transport.seen.size();
    EXPECT_FALSE(client.save_job("../x").ok());
    EXPECT_FALSE(client.install_save("", 0).ok());
    EXPECT_EQ(transport.seen.size(), sent);
    const api::Result<std::vector<SaveProfile>> users = client.save_users();
    ASSERT_TRUE(users.ok());
    EXPECT_EQ(users.value.size(), 1u);
}

TEST(SavesApi, RefusalsCarryTheStoresCodeAndSentence)
{
    test::ScriptedTransport transport;
    transport.reply = [](const http::Request &request)
    {
        if (request.path == "/api/saves/store")
            return test::json_reply(404, R"({"ok":false,"error":"not_found","message":"Unknown API route","status":404})");
        if (request.path == "/api/saves/store?refresh=1")
            return test::json_reply(200, R"({"ok":true})");
        http::Response gone;
        gone.error = "connection refused";
        return gone;
    };
    api::Client client(transport);
    const api::Result<SaveListing> older = client.save_store();
    EXPECT_EQ(older.status, 404);
    EXPECT_EQ(older.code, "not_found");
    EXPECT_EQ(older.error, "Unknown API route");
    const api::Result<SaveListing> odd = client.save_store(true);
    EXPECT_FALSE(odd.ok());
    EXPECT_NE(odd.error.find("unreadable"), std::string::npos) << odd.error;
    const api::Result<SaveJob> lost = client.save_job("s_1");
    EXPECT_TRUE(lost.unreachable());
    EXPECT_EQ(lost.error, "connection refused");
}

// ---- the worker ----

struct SavesWorker
{
    test::ScriptedTransport transport;
    SaveStore saves{transport};
    std::function<http::Response(const http::Request &)> reply;

    SavesWorker()
    {
        transport.reply = [this](const http::Request &request) { return reply(request); };
        saves.set_delays(0.0, 0.0, 0.0);
    }
    // Every step that is due, then the frame that applies them.
    void step(int steps = 1)
    {
        for (int i = 0; i < steps; ++i)
            saves.work_once();
        saves.tick();
    }
};

http::Response list_reply(bool building, const char *saves = "")
{
    return test::json_reply(200, std::string(R"({"ok":true,"backend":"github","backendName":"","building":)") +
                                     (building ? "true" : "false") + R"(,"saves":[)" + saves + "]}");
}

TEST(SaveStore, WaitsForTheSessionThenReadsTheListOnce)
{
    SavesWorker w;
    w.reply = [](const http::Request &request)
    {
        if (request.path == "/api/saves/users")
            return test::json_reply(200, R"({"ok":true,"users":[{"id":7,"name":"P","foreground":true}]})");
        return list_reply(false, R"({"id":"a","titleId":"PPSA00001"})");
    };
    w.saves.want();
    EXPECT_EQ(w.saves.state().load, SavesLoad::loading);
    EXPECT_FALSE(w.saves.work_once()); // no console session yet
    EXPECT_TRUE(w.transport.seen.empty());
    w.saves.set_token("tok");
    w.step();
    EXPECT_EQ(w.saves.state().load, SavesLoad::ready);
    ASSERT_EQ(w.saves.state().listing.saves.size(), 1u);
    EXPECT_TRUE(w.saves.state().have_profiles);
    EXPECT_EQ(w.transport.count("/api/saves/store"), 1);
    EXPECT_EQ(w.transport.count("/api/saves/users"), 1);
    EXPECT_EQ(w.transport.seen.front().bearer, "tok");
    // Fresh: nothing more until it is old or asked for again.
    EXPECT_FALSE(w.saves.work_once());
    w.saves.reload(true);
    w.step();
    EXPECT_EQ(w.transport.count("/api/saves/store?refresh=1"), 1);
    EXPECT_EQ(w.transport.count("/api/saves/users"), 1);
}

TEST(SaveStore, AsksAgainWhileTheStoreBuildsItsIndex)
{
    SavesWorker w;
    int lists = 0;
    w.reply = [&lists](const http::Request &request)
    {
        if (request.path == "/api/saves/users")
            return test::json_reply(200, R"({"ok":true,"users":[]})");
        return list_reply(++lists < 3);
    };
    w.saves.set_token("tok");
    w.saves.want();
    w.step();
    EXPECT_TRUE(w.saves.state().building());
    EXPECT_TRUE(w.saves.state().listing.saves.empty());
    w.saves.want();
    w.step();
    EXPECT_TRUE(w.saves.state().building());
    w.saves.want();
    w.step();
    EXPECT_FALSE(w.saves.state().building());
    EXPECT_EQ(lists, 3);
    w.saves.want();
    EXPECT_FALSE(w.saves.work_once()); // built: no more asking
    // Nobody shows the saves: a building store is not asked.
    SavesWorker idle;
    idle.reply = [](const http::Request &) { return list_reply(true); };
    idle.saves.set_token("tok");
    EXPECT_FALSE(idle.saves.work_once());
}

TEST(SaveStore, AnOlderStoreIsUnsupportedAndAFailureIsRetried)
{
    SavesWorker older;
    older.reply = [](const http::Request &)
    { return test::json_reply(404, R"({"ok":false,"error":"not_found","message":"Unknown API route"})"); };
    older.saves.set_token("tok");
    older.saves.want();
    older.step();
    EXPECT_EQ(older.saves.state().load, SavesLoad::unsupported);
    older.saves.want();
    EXPECT_FALSE(older.saves.work_once()); // it is not asked again

    SavesWorker w;
    bool up = false;
    w.reply = [&up](const http::Request &request)
    {
        if (!up)
        {
            http::Response timeout;
            timeout.error = "timed out";
            return timeout;
        }
        if (request.path == "/api/saves/users")
            return test::json_reply(200, R"({"ok":true,"users":[]})");
        return list_reply(false, R"({"id":"a","titleId":"PPSA00001"})");
    };
    w.saves.set_token("tok");
    w.saves.want();
    w.step();
    EXPECT_EQ(w.saves.state().load, SavesLoad::failed);
    EXPECT_EQ(w.saves.state().error, "timed out");
    up = true;
    w.saves.want();
    w.step();
    EXPECT_EQ(w.saves.state().load, SavesLoad::ready);
    EXPECT_TRUE(w.saves.state().error.empty());
}

TEST(SaveStore, InstallsForTheProfileAndFollowsTheJobToItsEnd)
{
    SavesWorker w;
    int reads = 0;
    w.reply = [&reads](const http::Request &request)
    {
        if (request.method == "POST")
            return test::json_reply(200, R"({"ok":true,"job":{"id":"s_9","state":"queued"}})");
        const bool done = ++reads >= 2;
        return test::json_reply(200, std::string(R"({"ok":true,"job":{"id":"s_9","state":")") +
                                         (done ? "done" : "running") +
                                         R"(","message":"Downloading","done":5,"total":10}})");
    };
    w.saves.set_token("tok");
    SaveRow save = row("match");
    save.id = "sb:1";
    w.saves.install_save(save, 268435457u);
    EXPECT_TRUE(w.saves.install().active);
    EXPECT_EQ(w.saves.install().game, "Ember Hollow");
    w.step();
    ASSERT_EQ(w.transport.seen.size(), 1u);
    EXPECT_EQ(w.transport.seen[0].path, "/api/saves/store/sb%3A1/install");
    EXPECT_NE(w.transport.seen[0].body.find("\"user\":268435457"), std::string::npos);
    EXPECT_NE(w.transport.seen[0].body.find("\"backupFirst\":true"), std::string::npos);
    EXPECT_TRUE(w.saves.install().have_job);
    w.step();
    EXPECT_TRUE(w.saves.install().active);
    EXPECT_EQ(w.saves.install().job.state, "running");
    EXPECT_DOUBLE_EQ(w.saves.install().job.done, 5.0);
    w.step();
    EXPECT_FALSE(w.saves.install().active);
    EXPECT_TRUE(w.saves.install().ended());
    EXPECT_EQ(w.saves.install().job.state, "done");
    EXPECT_EQ(w.transport.count("/api/saves/jobs/s_9"), 2);
    EXPECT_FALSE(w.saves.work_once()); // nothing to follow any more
    w.saves.clear_install();
    EXPECT_FALSE(w.saves.install().ended());
}

TEST(SaveStore, CancelsARunningInstall)
{
    SavesWorker w;
    bool cancelled = false;
    w.reply = [&cancelled](const http::Request &request)
    {
        if (request.path == "/api/saves/jobs/s_3/cancel")
            cancelled = true;
        return test::json_reply(200, std::string(R"({"ok":true,"job":{"id":"s_3","state":")") +
                                         (cancelled ? "cancelled" : "running") +
                                         R"(","errorCode":")" + (cancelled ? "cancelled" : "") + R"("}})");
    };
    w.saves.set_token("tok");
    w.saves.install_save(row("match"), 0);
    w.step();
    EXPECT_TRUE(w.saves.install().active);
    w.saves.cancel_install();
    EXPECT_TRUE(w.saves.install().cancelling);
    w.step();
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(w.saves.install().active);
    EXPECT_EQ(w.saves.install().job.state, "cancelled");
    EXPECT_EQ(save_install_outcome(w.saves.install().job, w.saves.install().game), "Install cancelled.");
}

TEST(SaveStore, ARefusedInstallEndsWithTheStoresSentence)
{
    SavesWorker w;
    w.reply = [](const http::Request &)
    { return test::json_reply(404, R"({"ok":false,"error":"not_found","message":"unknown save id"})"); };
    w.saves.set_token("tok");
    w.saves.install_save(row("match"), 0);
    w.step();
    EXPECT_FALSE(w.saves.install().active);
    EXPECT_TRUE(w.saves.install().ended());
    EXPECT_FALSE(w.saves.install().have_job);
    EXPECT_EQ(w.saves.install().error, "unknown save id");
}

// ---- the tab, against the preview's fixture ----

struct SavesApp
{
    test::TestFonts fonts;
    host::Fixture backend;
    test::FakeSink sink;
    test::ScriptedTransport transport;
    std::function<http::Response(const http::Request &)> intercept;
    Store store{transport};
    ArtCache art{backend, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    App app{context};
    Feedback feedback;

    explicit SavesApp(host::FixtureMode mode = host::FixtureMode::normal)
        : backend(mode, host::Dialect::xeno)
    {
        transport.reply = [this](const http::Request &request)
        {
            if (intercept)
            {
                http::Response response = intercept(request);
                if (response.status != 0)
                    return response;
            }
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
        for (const auto &instance : drawn.overlay.instances())
            ASSERT_TRUE(std::isfinite(instance.rect[0]) && std::isfinite(instance.rect[1]));
    }
    void press(Action action)
    {
        frame(Direction::none, hui::action_bit(action));
    }
    // The saves worker's due steps, then a few frames to show them.
    void work()
    {
        for (int i = 0; i < 4 && store.saves().work_once(); ++i)
        {
        }
        for (int i = 0; i < 10; ++i)
            frame();
    }
    int posts(const std::string &path)
    {
        int n = 0;
        for (const http::Request &request : transport.seen)
            n += request.method == "POST" && request.path == path ? 1 : 0;
        return n;
    }
};

TEST(SavesTab, ComesAfterBrowseAndListsTheCommunitySaves)
{
    SavesApp h;
    h.app.show_tab(tab::browse);
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), tab::saves);
    EXPECT_EQ(h.store.saves().state().load, SavesLoad::loading);
    h.work();
    ASSERT_EQ(h.store.saves().state().load, SavesLoad::ready);
    EXPECT_EQ(h.store.saves().state().listing.backend_name, "XenoKing");
    EXPECT_EQ(h.app.saves().results().size(), 7u);
    // Down to the chips, then For my games: the saves of installed games.
    h.frame(Direction::down);
    h.frame(Direction::right);
    h.press(Action::confirm);
    EXPECT_EQ(h.app.saves().results().size(), 5u);
    // A search narrows it further; Done goes to the saves.
    h.app.saves().set_query("ember");
    EXPECT_EQ(h.app.saves().results().size(), 2u);
    // Down to the first card and Cross: the details, a modal that keeps L1/R1 out.
    h.frame(Direction::down);
    h.press(Action::confirm);
    EXPECT_TRUE(h.app.saves().details_open());
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), tab::saves);
    h.press(Action::back);
    EXPECT_FALSE(h.app.saves().details_open());
    h.press(Action::page_next);
    EXPECT_EQ(h.app.current_tab(), tab::library);
}

TEST(SavesTab, AGamePageOpensItsCommunitySaves)
{
    SavesApp h;
    h.app.open_game("ember-hollow");
    h.frame();
    h.work(); // the game page asked for the saves
    // The primary, Favourite, About, then Community saves (2).
    for (int i = 0; i < 3; ++i)
        h.frame(Direction::right);
    h.press(Action::confirm);
    h.frame();
    EXPECT_EQ(h.app.current_tab(), tab::saves);
    EXPECT_EQ(h.app.saves().results().size(), 2u);
    // A game without saves shows no such button: Right stays on About.
    SavesApp quiet;
    quiet.app.open_game("ironclad-skies");
    quiet.frame();
    quiet.work();
    for (int i = 0; i < 3; ++i)
        quiet.frame(Direction::right);
    quiet.press(Action::confirm);
    quiet.frame();
    EXPECT_NE(quiet.app.current_tab(), tab::saves);
}

TEST(SavesTab, InstallsForTheChosenProfileAndBacksUpFirst)
{
    SavesApp h;
    h.app.show_tab(tab::saves);
    h.frame();
    h.work();
    h.app.saves().open_save("xk-ember-forge");
    h.frame();
    ASSERT_TRUE(h.app.saves().details_open());
    // Two profiles: Left reaches the picker; Guest, then Install.
    h.frame(Direction::left);
    h.press(Action::confirm);
    h.frame(Direction::down);
    h.press(Action::confirm);
    h.frame(Direction::right);
    h.press(Action::confirm);
    EXPECT_TRUE(h.store.saves().install().active);
    h.work();
    ASSERT_EQ(h.posts("/api/saves/store/xk-ember-forge/install"), 1);
    for (const http::Request &request : h.transport.seen)
    {
        if (request.method != "POST" || request.path != "/api/saves/store/xk-ember-forge/install")
            continue;
        EXPECT_NE(request.body.find("\"user\":268435457"), std::string::npos) << request.body;
        EXPECT_NE(request.body.find("\"backupFirst\":true"), std::string::npos) << request.body;
    }
    // The screen stays live while the job runs: Circle closes the details.
    EXPECT_TRUE(h.store.saves().install().have_job);
    h.press(Action::back);
    EXPECT_FALSE(h.app.saves().details_open());
    EXPECT_TRUE(h.store.saves().install().active);
}

TEST(SavesTab, ARunningGameEndsTheInstallWithCloseTheGameFirst)
{
    SavesApp h;
    h.intercept = [](const http::Request &request)
    {
        if (request.method == "POST" && request.path.find("/install") != std::string::npos)
            return test::json_reply(200, R"({"ok":true,"job":{"id":"s_7","state":"failed","errorCode":"title_running",
                "error":"PPSA90004 is running: close the game (not just suspend it) before its save is replaced"}})");
        return http::Response{};
    };
    h.app.show_tab(tab::saves);
    h.frame();
    h.work();
    h.app.saves().open_save("xk-orchard-bloom");
    h.frame();
    h.press(Action::confirm); // Install has the focus
    h.work();
    const SaveInstall &install = h.store.saves().install();
    ASSERT_TRUE(install.ended());
    EXPECT_EQ(install.job.error_code, "title_running");
    const std::string outcome = save_install_outcome(install.job, install.game);
    EXPECT_NE(outcome.find("Glass Orchard is running"), std::string::npos) << outcome;
    // Closing the details forgets the outcome; the next look starts afresh.
    h.press(Action::back);
    EXPECT_FALSE(h.store.saves().install().ended());
}

TEST(SavesTab, AnOlderStoreAndABuildingStoreSaySo)
{
    SavesApp older(host::FixtureMode::saves_older);
    older.app.show_tab(tab::saves);
    older.frame();
    older.work();
    EXPECT_EQ(older.store.saves().state().load, SavesLoad::unsupported);
    EXPECT_TRUE(older.app.saves().results().empty());

    SavesApp building(host::FixtureMode::saves_empty);
    building.app.show_tab(tab::saves);
    building.frame();
    building.work();
    EXPECT_TRUE(building.store.saves().state().building());
    // The tabs still move while the store builds.
    building.press(Action::page_prev);
    EXPECT_EQ(building.app.current_tab(), tab::browse);
}

} // namespace
} // namespace orbit
