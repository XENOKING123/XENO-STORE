// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include <cstdio>
#include <gtest/gtest.h>

namespace orbit
{
namespace
{
struct DiscoverHarness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    DiscoverScreen screen{context};
    Feedback feedback;
    int count;
    int revision = 1;
    bool dated;
    bool additions = false;
    int pinned = -1;
    std::vector<int> sizes;
    explicit DiscoverHarness(int size = 55, bool dates = true) : count(size), dated(dates)
    {
        transport.reply = [this](const http::Request &request)
        {
            if (request.path == "/api/v1/catalog")
            {
                std::string catalogue = "[";
                for (int i = 0; i < count; ++i)
                {
                    char id[24];
                    std::snprintf(id, sizeof(id), "game-%02d", i);
                    if (i > 0)
                        catalogue += ',';
                    catalogue += "{\"id\":\"" + std::string(id) + "-r\",\"gameId\":\"" + id +
                                 "\",\"title\":\"" + id + "\",\"sourceId\":\"archive\","
                                 "\"provider\":\"Archive.org\",\"format\":\"FFPFSC\",\"sizeBytes\":" +
                                 std::to_string(i < static_cast<int>(sizes.size()) ? sizes[static_cast<std::size_t>(i)] : 1000) +
                                 ",\"newOnOrbitRank\":" + (i == pinned ? "1" : "null") +
                                 ",\"addedAt\":\"" + (additions ? "2026-10-05T12:00:00Z" : std::string()) + "\",\"releaseDate\":\"" + (dated ? "2026-09-12" : std::string()) + "\"}";
                }
                return test::json_reply(200, catalogue + ']');
            }
            http::Response response = backend.send(request);
            if (request.path == "/api/v1/system")
            {
                const std::string old = "\"catalogueRevision\":7";
                const auto at = response.body.find(old);
                if (at != std::string::npos)
                    response.body.replace(at, old.size(), "\"catalogueRevision\":" + std::to_string(revision));
            }
            return response;
        };
        context.today = "2026-10-06";
        refresh();
    }
    void refresh()
    {
        ++revision;
        store.poll_once();
        store.tick();
        screen.focus();
        input();
    }
    Intent input(Direction nav = Direction::none, bool confirm = false)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        if (confirm)
            input.pressed = hui::action_bit(Action::confirm);
        Intent result = screen.update(input, 1.0f / 60.0f, feedback, true);
        Frame frame;
        screen.draw(frame, true);
        return result;
    }
    Intent press(Action action)
    {
        InputFrame input;
        input.connected = true;
        input.pressed = hui::action_bit(action);
        Intent result = screen.update(input, 1.0f / 60.0f, feedback, true);
        Frame frame;
        screen.draw(frame, true);
        return result;
    }
    void all_games()
    {
        input(Direction::down);
        if (dated)
            input(Direction::down);
    }
};

TEST(Discover, NewOnOrbitIsASeparateNavigableRailBeforeTheGrid)
{
    DiscoverHarness h;
    h.additions = true;
    h.refresh();
    h.input(Direction::down); // New on Orbit
    h.input(Direction::down); // latest
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.screen.return_to("game-01");
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.input(Direction::down); // all-games grid
    h.input(Direction::down); // next grid row
    EXPECT_EQ(h.input(Direction::none, true).id, "game-06");
    h.input(Direction::up);
    h.input(Direction::up); // remembers selection in Latest releases
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
}

TEST(Discover, NewOnOrbitLeadsLatestAndKeepsItsOwnSelection)
{
    DiscoverHarness h;
    h.additions = true;
    h.pinned = 40;
    h.refresh();
    h.input(Direction::down); // first rail: catalogue pick
    EXPECT_EQ(h.input(Direction::none, true).id, "game-40");
    h.input(Direction::down); // second rail: latest releases
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-40");
}

TEST(Discover, GridMovesVerticallyAndReturnsToTheSelectedGame)
{
    DiscoverHarness h;
    h.all_games();
    h.input(Direction::right);
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-07");
    h.screen.return_to("game-07");
    EXPECT_EQ(h.input(Direction::none, true).id, "game-07");
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    for (int i = 0; i < 10; ++i)
        h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-05"); // no wrap to next row
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00"); // latest rail
}

TEST(Discover, AllGamesScrollsThroughTheWholeCatalogueWithoutPages)
{
    DiscoverHarness h(250);
    for (int i = 0; i < h.count; ++i)
        h.sizes.push_back(1000 + h.count - i); // largest first keeps the fixture's order
    h.refresh();
    h.all_games();
    for (int i = 0; i < 41; ++i)
        h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-246"); // past the old 96-game page
    for (int i = 0; i < 3; ++i)
        h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-249");
    h.input(Direction::down); // the last row ends the grid
    EXPECT_EQ(h.input(Direction::none, true).id, "game-249");
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-243");
}

TEST(Discover, L2AndR2JumpSixteenRowsThroughTheGrid)
{
    DiscoverHarness h(250);
    for (int i = 0; i < h.count; ++i)
        h.sizes.push_back(1000 + h.count - i);
    h.refresh();
    const auto jump_hint = [&]
    {
        for (const hui::ui::Hint &hint : h.screen.hints())
            if (hint.button == hui::ui::Button::l2)
                return true;
        return false;
    };
    EXPECT_FALSE(jump_hint()); // the hero has nothing to jump through
    h.press(Action::jump_next);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00"); // still the hero's game
    h.all_games();
    EXPECT_TRUE(jump_hint());
    h.input(Direction::right);
    h.press(Action::jump_next);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-97"); // same column, 16 rows down
    h.press(Action::jump_next);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-193");
    h.press(Action::jump_next); // fewer than 16 rows remain: the last row
    EXPECT_EQ(h.input(Direction::none, true).id, "game-247");
    h.press(Action::jump_next);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-247");
    h.press(Action::jump_prev);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-151");
    h.press(Action::jump_prev);
    h.press(Action::jump_prev); // stops on the first row
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.input(Direction::up); // and the first row still leads up to Latest releases
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
}

TEST(Discover, BrowseL2AndR2MatchSixteenStepsDown)
{
    DiscoverHarness h(250);
    const auto step = [&](BrowseScreen &screen, Direction nav, std::uint32_t pressed = 0)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        input.pressed = pressed;
        return screen.update(input, 1.0f / 60.0f, h.feedback, true);
    };
    BrowseScreen jumped(h.context), walked(h.context);
    for (BrowseScreen *screen : {&jumped, &walked})
    {
        screen->focus();
        step(*screen, Direction::down); // filters
        step(*screen, Direction::down); // the first result
    }
    const std::string first = step(jumped, Direction::none, hui::action_bit(Action::confirm)).id;
    step(jumped, Direction::none, hui::action_bit(Action::jump_next));
    for (int i = 0; i < 16; ++i)
        step(walked, Direction::down);
    const std::string landed = step(jumped, Direction::none, hui::action_bit(Action::confirm)).id;
    EXPECT_FALSE(landed.empty());
    EXPECT_NE(landed, first);
    EXPECT_EQ(landed, step(walked, Direction::none, hui::action_bit(Action::confirm)).id);
    step(jumped, Direction::none, hui::action_bit(Action::jump_prev));
    EXPECT_EQ(step(jumped, Direction::none, hui::action_bit(Action::confirm)).id, first);
}

TEST(Discover, CatalogueShrinkAndUndatedGamesKeepNavigationInBounds)
{
    DiscoverHarness h;
    h.all_games();
    for (int i = 0; i < 8; ++i)
        h.input(Direction::down);
    h.input(Direction::none, true);
    h.count = 1;
    h.dated = false;
    h.refresh();
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.count = 0;
    h.refresh();
    EXPECT_NE(h.input(Direction::none, true).kind, Intent::Kind::open_game);
    EXPECT_EQ(h.input(Direction::up).kind, Intent::Kind::top_bar);
}

TEST(Discover, AllGamesUsesLargestDownloadFirstWithoutChangingTheLatestRail)
{
    DiscoverHarness h(3);
    h.sizes = {3000, 9000, 1000};
    h.refresh();
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-02");
}
} // namespace
} // namespace orbit
