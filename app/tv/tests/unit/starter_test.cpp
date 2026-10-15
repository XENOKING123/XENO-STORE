// Orbit Store TV app - Starting Orbit through the ELF loader.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.

#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "orbit/starter.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace orbit
{
namespace
{

using starter::Outcome;

std::string elf_bytes(std::size_t size, char fill)
{
    std::string bytes(size, fill);
    const char header[] = "\177ELF\2\1\1";
    bytes.replace(0, 7, header, 7);
    bytes[16] = 2;
    bytes[17] = 0;
    bytes[18] = 62;
    bytes[19] = 0;
    return bytes;
}

void write(const std::string &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

struct TempDir
{
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/orbit-starter-XXXXXX";
        path = mkdtemp(name);
        mkdir((path + "/xeno").c_str(), 0700);
    }
    ~TempDir()
    {
        std::system(("rm -rf '" + path + "'").c_str());
    }
    starter::Locations locations() const
    {
        starter::Locations where;
        // As on the console: /data/xeno-store and the package's /app0/xeno.
        where.saved_elf = path + "/xeno-store.elf";
        where.saved_version = path + "/xeno-store.elf.version";
        where.bundled_elf = path + "/xeno/xeno-store.elf";
        where.bundled_version = path + "/xeno/version.txt";
        return where;
    }
};

// A one-connection stand-in for the ELF loader: keeps what it was sent.
class Loader
{
  public:
    Loader()
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length);
        port = ntohs(address.sin_port);
        listen(fd_, 1);
        thread_.start(
            [this]
            {
                const int client = accept(fd_, nullptr, nullptr);
                char buffer[65536];
                for (;;)
                {
                    const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        break;
                    received.append(buffer, static_cast<std::size_t>(n));
                }
                close(client);
            });
    }
    ~Loader()
    {
        thread_.join();
        close(fd_);
    }
    std::string take()
    {
        thread_.join();
        return received;
    }

    std::uint16_t port = 0;

  private:
    int fd_ = -1;
    std::string received;
    Thread thread_;
};

// A port on which nothing listens.
std::uint16_t closed_port()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
    close(fd);
    return ntohs(address.sin_port);
}

TEST(Starter, OrdersReleaseVersionsAsOrbitDoes)
{
    using starter::compare_versions;
    EXPECT_EQ(compare_versions("0.4.2", "0.4.2"), 0);
    EXPECT_EQ(compare_versions("v0.4.2", "0.4.2"), 0);
    EXPECT_EQ(compare_versions("0.4.10", "0.4.9"), 1);
    EXPECT_EQ(compare_versions("0.5.0", "0.4.99"), 1);
    EXPECT_EQ(compare_versions("0.5.0-beta.2", "0.5.0"), -1);
    EXPECT_EQ(compare_versions("0.5.0-beta.10", "0.5.0-beta.9"), 1);
    EXPECT_EQ(compare_versions("", "0.0.1"), -1);
    EXPECT_EQ(compare_versions("0.0.1", "1.2"), 1);
    EXPECT_EQ(compare_versions("nonsense", ""), 0);
    EXPECT_EQ(compare_versions("1.2.3-rc.1", "0.0.1"), -1);
}

TEST(Starter, PrefersTheSavedCopyUnlessThePackageIsNewer)
{
    TempDir dir;
    const starter::Locations where = dir.locations();
    EXPECT_TRUE(starter::choose(where).path.empty());

    write(where.bundled_elf, elf_bytes(4096, 'b'));
    write(where.bundled_version, "0.4.2\n");
    starter::Payload payload = starter::choose(where);
    EXPECT_EQ(payload.path, where.bundled_elf);
    EXPECT_STREQ(payload.source, "bundled");
    EXPECT_EQ(payload.version, "0.4.2");
    EXPECT_EQ(payload.bytes, 4096);

    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.saved_version, "0.4.2");
    EXPECT_EQ(starter::choose(where).path, where.saved_elf); // a tie keeps the saved copy

    write(where.saved_version, "0.5.0");
    EXPECT_EQ(starter::choose(where).path, where.saved_elf);

    write(where.saved_version, "0.4.1");
    EXPECT_EQ(starter::choose(where).path, where.bundled_elf);

    std::remove(where.saved_version.c_str()); // an unknown saved release counts as older
    EXPECT_EQ(starter::choose(where).path, where.bundled_elf);

    write(where.bundled_elf, "#!/bin/sh\n" + std::string(100, 'x')); // not an ELF
    EXPECT_EQ(starter::choose(where).path, where.saved_elf);

    write(where.saved_elf, elf_bytes(32, 's')); // too short to be a payload
    EXPECT_TRUE(starter::choose(where).path.empty());
}

TEST(Starter, StartsXenoStoresSavedOrBundledCopyThroughTheLoader)
{
    const starter::Locations where;
    EXPECT_EQ(where.saved_elf, "/data/xeno-store/xeno-store.elf");
    EXPECT_EQ(where.saved_version, "/data/xeno-store/xeno-store.elf.version");
    EXPECT_EQ(where.bundled_elf, "/app0/xeno/xeno-store.elf");
    EXPECT_EQ(where.bundled_version, "/app0/xeno/version.txt");
    EXPECT_EQ(where.loader_port, 9021);
}

TEST(Starter, AvoidingTheSavedCopyChoosesTheBundledOne)
{
    TempDir dir;
    const starter::Locations where = dir.locations();
    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.saved_version, "1.2.2"); // as update_apply writes it: no newline
    write(where.bundled_elf, elf_bytes(4096, 'b'));
    write(where.bundled_version, "1.2.1\n");
    EXPECT_EQ(starter::choose(where).path, where.saved_elf);
    const starter::Payload avoided = starter::choose(where, true);
    EXPECT_EQ(avoided.path, where.bundled_elf);
    EXPECT_EQ(avoided.version, "1.2.1");
    EXPECT_TRUE(avoided.saved_avoided);

    // Only the bundled copy is readable: it is chosen either way.
    std::remove(where.saved_elf.c_str());
    EXPECT_EQ(starter::choose(where).path, where.bundled_elf);
    EXPECT_EQ(starter::choose(where, true).path, where.bundled_elf);
    EXPECT_FALSE(starter::choose(where, true).saved_avoided);

    // Without a readable bundled copy, the saved one is still sent.
    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.bundled_elf, "not an ELF");
    EXPECT_EQ(starter::choose(where, true).path, where.saved_elf);
}

TEST(Starter, SendsToTheLoaderPortOfItsLocations)
{
    TempDir dir;
    starter::Locations where = dir.locations();
    const std::string bytes = elf_bytes(300000, 'x');
    write(where.bundled_elf, bytes);
    write(where.bundled_version, "1.2.1\n");
    Loader loader;
    where.loader_port = loader.port;
    std::vector<std::string> lines;
    starter::LoaderStarter starter(where, [&](const std::string &line) { lines.push_back(line); });
    const starter::Result result = starter.start();
    EXPECT_EQ(result.outcome, Outcome::sent) << result.detail;
    EXPECT_STREQ(result.source, "bundled");
    EXPECT_EQ(result.version, "1.2.1");
    EXPECT_EQ(loader.take(), bytes); // every byte, in order
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("copy=bundled version=1.2.1 bytes=300000"), std::string::npos);
}

TEST(Starter, LoaderStarterSendsTheBundledCopyWhileTheSavedOneIsAvoided)
{
    TempDir dir;
    starter::Locations where = dir.locations();
    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.saved_version, "1.2.2");
    const std::string bundled = elf_bytes(8192, 'b');
    write(where.bundled_elf, bundled);
    write(where.bundled_version, "1.2.1\n");
    Loader loader;
    where.loader_port = loader.port;
    std::vector<std::string> lines;
    starter::LoaderStarter starter(where, [&](const std::string &line) { lines.push_back(line); });
    EXPECT_FALSE(starter.avoid_saved());
    starter.set_avoid_saved(true);
    const starter::Result result = starter.start();
    EXPECT_EQ(result.outcome, Outcome::sent);
    EXPECT_STREQ(result.source, "bundled");
    EXPECT_EQ(loader.take(), bundled);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("start: sent copy=bundled (saved copy did not answer) version=1.2.1"),
              std::string::npos)
        << lines[0];
}

std::string slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "(none)";
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// XENO STORE: the bundled copy that stands in for a saved update that never
// answered finds an expired hand-off marker, so it sets that update aside and
// runs itself instead of handing the same file to the loader again.
TEST(Starter, TheBundledStandInLeavesAnExpiredHandOffMarker)
{
    TempDir dir;
    starter::Locations where = dir.locations();
    const std::string marker = starter::handoff_marker(where);
    EXPECT_EQ(marker, dir.path + "/handoff.pending");
    EXPECT_EQ(starter::handoff_marker(starter::Locations{}), "/data/xeno-store/handoff.pending");
    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.saved_version, "1.2.2");
    write(where.bundled_elf, elf_bytes(8192, 'b'));
    write(where.bundled_version, "1.2.1\n");

    {
        Loader loader;
        where.loader_port = loader.port;
        starter::LoaderStarter starter(where);
        EXPECT_EQ(starter.next_version(), "1.2.2");
        EXPECT_EQ(starter.start().outcome, Outcome::sent);
        EXPECT_EQ(slurp(marker), "(none)"); // the saved copy itself: the backend handles it
    }
    {
        starter::LoaderStarter starter(where, closed_port());
        starter.set_avoid_saved(true);
        EXPECT_EQ(starter.next_version(), "1.2.1");
        EXPECT_EQ(starter.start().outcome, Outcome::no_loader);
        EXPECT_EQ(slurp(marker), "(none)"); // nothing started: nothing set aside later
    }
    {
        Loader loader;
        where.loader_port = loader.port;
        starter::LoaderStarter starter(where);
        starter.set_avoid_saved(true);
        const starter::Result result = starter.start();
        EXPECT_EQ(result.outcome, Outcome::sent);
        EXPECT_STREQ(result.source, "bundled");
        EXPECT_EQ(slurp(marker), "0 tv-app\n");
        EXPECT_EQ(slurp(marker + ".tv-app"), "(none)");
    }
}

TEST(Starter, StreamsThePayloadToTheLoader)
{
    TempDir dir;
    const std::string path = dir.path + "/orbit.elf";
    const std::string bytes = elf_bytes(300000, 'o');
    write(path, bytes);
    Loader loader;
    std::string error;
    EXPECT_EQ(starter::send(path, loader.port, 2000, &error), Outcome::sent) << error;
    EXPECT_EQ(loader.take(), bytes);
}

TEST(Starter, ReportsAMissingLoaderAndAMissingFile)
{
    TempDir dir;
    const std::string path = dir.path + "/orbit.elf";
    write(path, elf_bytes(4096, 'o'));
    std::string error;
    EXPECT_EQ(starter::send(path, closed_port(), 2000, &error), Outcome::no_loader);
    EXPECT_NE(error.find("No ELF loader"), std::string::npos);
    EXPECT_EQ(starter::send(dir.path + "/missing.elf", closed_port(), 2000, &error),
              Outcome::no_payload);
}

TEST(Starter, LoaderStarterSendsTheChosenCopyAndLogsIt)
{
    TempDir dir;
    const starter::Locations where = dir.locations();
    std::vector<std::string> lines;
    {
        starter::LoaderStarter nothing(where, closed_port(),
                                       [&](const std::string &line) { lines.push_back(line); });
        const starter::Result result = nothing.start();
        EXPECT_EQ(result.outcome, Outcome::no_payload);
        ASSERT_EQ(lines.size(), 1u);
        EXPECT_NE(lines[0].find("no payload"), std::string::npos);
    }
    const std::string bytes = elf_bytes(8192, 'b');
    write(where.bundled_elf, bytes);
    write(where.bundled_version, "0.4.2");
    Loader loader;
    starter::LoaderStarter starter(where, loader.port,
                                   [&](const std::string &line) { lines.push_back(line); });
    const starter::Result result = starter.start();
    EXPECT_EQ(result.outcome, Outcome::sent);
    EXPECT_NE(result.detail.find("0.4.2"), std::string::npos);
    EXPECT_EQ(result.version, "0.4.2");
    EXPECT_EQ(loader.take(), bytes);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find("copy=bundled version=0.4.2 bytes=8192"), std::string::npos);
}

// ---- the store's rules for starting Orbit ----

class CountingStarter final : public starter::Starter
{
  public:
    Outcome outcome = Outcome::sent;
    std::string version;
    std::atomic<int> calls{0};
    starter::Result start() override
    {
        ++calls;
        starter::Result result;
        result.outcome = outcome;
        result.detail = "detail";
        result.version = version;
        return result;
    }
};

// Reports which copy it sent: the saved one unless told to avoid it.
class CopyStarter final : public starter::Starter
{
  public:
    std::vector<bool> avoided; // avoid_saved() at each call
    starter::Result start() override
    {
        avoided.push_back(avoid_saved());
        starter::Result result;
        result.outcome = Outcome::sent;
        result.source = avoid_saved() ? "bundled" : "saved";
        result.version = avoid_saved() ? "1.2.1" : "1.2.2";
        result.detail = "detail";
        return result;
    }
};

// Orbit refusing connections until `online` is set, then the fixture backend.
// Stop Orbit is accepted here; the test decides when Orbit goes away.
struct Console
{
    host::Fixture fixture{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    std::atomic<bool> online{false};
    std::string failure = "connection refused";
    std::string version = "0.6.0", instance, saved;
    bool stopping = false;
    Console()
    {
        transport.reply = [this](const http::Request &request)
        {
            if (online && request.method == "POST" && request.path == "/api/v1/updates")
                return test::json_reply(202, "{\"phase\":\"stopping\",\"instanceId\":\"" + instance +
                    "\",\"runningVersion\":\"" + version + "\",\"savedVersion\":\"" + saved + "\"}");
            if (online && request.path == "/api/v1/system")
            {
                // Preserve the fixture's session and catalogue fields.
                auto response = fixture.send(request);
                const auto at = response.body.find('{');
                response.body.insert(at + 1, "\"instanceId\":\"" + instance +
                    "\",\"stopping\":" + (stopping ? "true," : "false,"));
                const auto old = response.body.find("0.6.0");
                if (old != std::string::npos) response.body.replace(old, 5, version);
                return response;
            }
            if (online)
                return fixture.send(request);
            http::Response response;
            response.error = failure;
            return response;
        };
    }
};

TEST(StoreStart, StartsOrbitOnceWhenNothingAnswersThenClearsWhenItDoes)
{
    Console console;
    CountingStarter starter;
    Store store(console.transport, &starter);
    EXPECT_TRUE(store.can_start());
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().starting, Starting::waiting);
    EXPECT_EQ(store.state().start_detail, "detail");
    store.poll_once(); // still starting: no second payload
    EXPECT_EQ(starter.calls, 1);
    console.online = true;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_EQ(store.state().starting, Starting::idle);
    EXPECT_TRUE(store.state().start_detail.empty());
    // A later loss of Orbit is not answered with another payload by itself.
    console.online = false;
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().link, Link::offline);
}

TEST(StoreStart, ASavedCopyThatNeverAnswersIsAvoidedUntilACopyAnswers)
{
    Console console;
    CopyStarter starter;
    Store store(console.transport, &starter);
    store.set_answer_waits(0.0, 0.0);
    store.poll_once(); // sends the saved copy
    store.poll_once(); // it never answered
    store.tick();
    EXPECT_EQ(store.state().starting, Starting::failed);
    EXPECT_TRUE(starter.avoid_saved());
    store.start_orbit(); // Try again: the bundled copy
    store.poll_once();
    ASSERT_EQ(starter.avoided.size(), 2u);
    EXPECT_FALSE(starter.avoided[0]);
    EXPECT_TRUE(starter.avoided[1]);
    console.online = true;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_FALSE(starter.avoid_saved()); // cleared after success
}

TEST(StoreStart, ABundledCopyThatNeverAnswersDoesNotSetTheSavedOneAside)
{
    Console console;
    CopyStarter starter;
    starter.set_avoid_saved(true); // sends the bundled copy
    Store store(console.transport, &starter);
    store.set_answer_waits(0.0, 0.0);
    store.poll_once();
    starter.set_avoid_saved(false);
    store.poll_once(); // the bundled copy never answered
    store.tick();
    EXPECT_EQ(store.state().starting, Starting::failed);
    EXPECT_FALSE(starter.avoid_saved());
}

TEST(StoreStart, ReportsAMissingLoaderAndRetriesOnlyWhenAsked)
{
    Console console;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
    Store store(console.transport, &starter);
    store.poll_once();
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().starting, Starting::no_loader);
    store.start_orbit();
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 2);
    starter.outcome = Outcome::no_payload;
    store.start_orbit();
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().starting, Starting::no_payload);
}

TEST(StoreStart, LeavesASlowOrbitAloneAndWorksWithoutAStarter)
{
    Console console;
    console.failure = "connect failed"; // something answers, slowly: not a stopped Orbit
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 0);
    EXPECT_EQ(store.state().starting, Starting::idle);

    Console plain;
    Store waiting(plain.transport);
    EXPECT_FALSE(waiting.can_start());
    waiting.start_orbit();
    waiting.poll_once();
    waiting.tick();
    EXPECT_EQ(waiting.state().starting, Starting::idle);
    EXPECT_EQ(waiting.state().link, Link::offline);
}

TEST(StoreStart, CrossOnTheOfflineScreenStartsOrbitAgain)
{
    Console console;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
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
    };
    frame(0);
    EXPECT_EQ(store.state().starting, Starting::no_loader);
    frame(hui::action_bit(Action::confirm));
    EXPECT_FALSE(feedback.cues.empty());
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
}

// ---- restarting the download service from Settings ----

TEST(StoreRestart, StopsOrbitThenStartsTheSavedCopyAndReportsItsVersion)
{
    Console console;
    console.online = true;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.tick();
    ASSERT_EQ(store.state().link, Link::online);
    store.restart_service();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    int stops = 0;
    for (const http::Request &request : console.transport.seen)
        stops += request.body == R"({"action":"stop","confirmed":true})" ? 1 : 0;
    EXPECT_EQ(stops, 1);
    // Orbit answers for a moment, then its port closes.
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    console.online = false;
    store.poll_once(); // stopped
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::starting);
    EXPECT_EQ(starter.calls, 0);
    store.poll_once(); // sends the saved copy
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().restart, Restart::starting);
    store.poll_once(); // waiting for it to answer: not sent again yet
    EXPECT_EQ(starter.calls, 1);
    console.online = true;
    const int sessions = console.transport.count("/api/v1/session");
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "0.6.0");
    EXPECT_EQ(console.transport.count("/api/v1/session"), sessions + 1); // a new pairing code
    store.dismiss_restart();
    EXPECT_EQ(store.state().restart, Restart::idle);
    // Having connected consumes automatic startup. Do not undo a later Stop.
    console.online = false;
    store.poll_once();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().restart, Restart::idle);
    store.poll_once();
    EXPECT_EQ(starter.calls, 1);
}

TEST(StoreRestart, WithoutALoaderItAsksForAPayloadManagerAndWaits)
{
    Console console;
    console.online = true;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::manual);
    store.poll_once();
    EXPECT_EQ(starter.calls, 1); // no retry on its own
    store.start_orbit();         // Try again
    store.poll_once();
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
    console.online = true; // started from a payload manager
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);

    Console plain;
    plain.online = true;
    Store waiting(plain.transport); // no starter at all
    waiting.set_restart_grace(0.0);
    waiting.poll_once();
    waiting.restart_service();
    waiting.run_next_action();
    plain.online = false;
    waiting.poll_once();
    waiting.poll_once();
    waiting.tick();
    EXPECT_EQ(waiting.state().restart, Restart::manual);
}

TEST(StoreRestart, ARefusedStopChangesNothing)
{
    Console console;
    console.online = true;
    console.transport.reply = [&console](const http::Request &request)
    {
        if (request.method == "POST" && request.path == "/api/v1/updates")
            return test::json_reply(409, R"({"error":"An Orbit update is in progress."})");
        return console.fixture.send(request);
    };
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::idle);
    EXPECT_EQ(store.setting_result().error, "An Orbit update is in progress.");
    EXPECT_EQ(store.setting_result().setting, Setting::service_restart);
}

TEST(StoreStart, ConnectingToAnExistingServiceDoesNotUndoALaterStop)
{
    Console console;
    console.online = true;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    console.stopping = true;
    store.poll_once();
    console.online = false;
    store.poll_once();
    store.poll_once();
    EXPECT_EQ(starter.calls, 0);
    store.start_orbit();
    store.poll_once();
    EXPECT_EQ(starter.calls, 1); // explicit retry remains available
}

TEST(StoreRestart, WaitsForShutdownThenVerifiesVersionAndNewInstance)
{
    Console console;
    console.online = true;
    console.instance = "previous-session";
    console.saved = "1.0.6";
    CountingStarter starter;
    starter.version = "1.0.6";
    Store store(console.transport, &starter);
    store.set_restart_grace(0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.stopping = true;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    EXPECT_EQ(starter.calls, 0);
    console.online = false;
    store.poll_once();
    store.poll_once();
    EXPECT_EQ(starter.calls, 1);
    console.online = true;
    store.poll_once(); // old server is still draining: never send another ELF
    EXPECT_EQ(starter.calls, 1);
    console.stopping = false;
    console.instance = "new-session";
    console.version = "1.0.6";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.0.6");
}

TEST(StoreRestart, RejectsAReturningOldVersionEvenWhenItIsANewProcess)
{
    Console console;
    console.online = true;
    console.instance = "previous-session";
    console.saved = "1.0.6";
    CountingStarter starter;
    starter.version = "1.0.6";
    Store store(console.transport, &starter);
    store.set_restart_grace(0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    console.online = true;
    console.instance = "another-old-copy";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_NE(store.state().restart_detail.find("0.6.0"), std::string::npos);
    EXPECT_NE(store.state().restart_detail.find("1.0.6"), std::string::npos);
}

TEST(StoreRestart, DoesNotMistakeATemporaryConnectionLossForANewProcess)
{
    Console console;
    console.online = true;
    console.instance = "same-session";
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.set_restart_grace(0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    console.online = true;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_NE(store.state().restart_detail.find("previous XENO STORE session"), std::string::npos);
}

TEST(StoreRestart, OlderFallbackDoesNotLowerTheExpectedInstalledVersion)
{
    Console console;
    console.online = true;
    console.instance = "previous-session";
    console.saved = "1.0.6";
    CountingStarter starter;
    starter.version = "0.6.0"; // unreadable saved update, readable older bundle
    Store store(console.transport, &starter);
    store.set_restart_grace(0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    console.online = true;
    console.instance = "older-fallback";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::failed);
    EXPECT_NE(store.state().restart_detail.find("1.0.6"), std::string::npos);
}

TEST(StoreRestart, NewerBundleCanSupersedeAnOlderSavedCopy)
{
    Console console;
    console.online = true;
    console.instance = "previous-session";
    console.saved = "0.6.0";
    CountingStarter starter;
    starter.version = "1.0.6";
    Store store(console.transport, &starter);
    store.set_restart_grace(0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    console.online = true;
    console.instance = "new-bundle";
    console.version = "1.0.6";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "1.0.6");
}

TEST(StoreRestart, RecognisesAFastReplacementEvenBetweenPolls)
{
    Console console;
    console.online = true;
    console.instance = "previous-session";
    console.saved = "1.0.6";
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.instance = "new-session";
    console.version = "1.0.6";
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(starter.calls, 0); // replacement already running, leave it alone
}

} // namespace
} // namespace orbit
