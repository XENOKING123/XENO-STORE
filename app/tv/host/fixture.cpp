// Orbit Store TV app - A stand-in Orbit backend for PC previews.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.

#include "fixture.hpp"

#include "orbit/json.hpp"

#include "../third_party/stb/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace orbit::host
{

namespace
{

constexpr const char *kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kChecksum =
    "5d41402abc4b2a76b9719d911017c592e2b5f6e1d0c4c1d36c0b1f3a6f0e9a21";

double unix_now()
{
    return static_cast<double>(std::time(nullptr));
}

double steady_now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string number(double value)
{
    char text[48];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}

struct Option
{
    const char *suffix;
    const char *provider;
    const char *format;
    const char *version;
    double gigabytes;
};

struct FixtureGame
{
    const char *id;
    const char *title;
    const char *genre;
    const char *tagline;
    const char *description;
    const char *publisher;
    const char *date; // empty: undated
    bool wide;
    const char *title_id;
    std::vector<Option> options;
};

// Invented titles and studios: previews never show real products.
const std::vector<FixtureGame> &games()
{
    static const std::vector<FixtureGame> list = {
        {"tidewater",
         "Tidewater",
         "Adventure",
         "Chart a drowned coast, one tide at a time.",
         "Sail a flooded archipelago in a boat you rebuild from what the sea gives back. Every "
         "tide "
         "uncovers a new path and hides an old one.",
         "Tin Sparrow",
         "2026-09-12",
         true,
         "PPSA90001",
         {{"ffpfsc", "Archive.org", "FFPFSC", "1.02", 48.2},
          {"exfat", "Archive.org", "exFAT", "", 46.9}}},
        {"neon-courier",
         "Neon Courier",
         "Racing",
         "Deliver anything, anywhere, before sunrise.",
         "A night-time delivery racer through a city that rearranges its streets every hour.",
         "Night Shift Studio",
         "2026-08-30",
         false,
         "PPSA90002",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 22.4}}},
        {"ember-hollow",
         "Ember Hollow",
         "Role-playing",
         "A village that remembers every fire.",
         "Rebuild a mountain village after the long winter, and learn why the hearths keep the old "
         "stories alive.",
         "Lantern & Oak",
         "2026-07-18",
         true,
         "PPSA90003",
         {{"ffpfsc", "Archive.org", "FFPFSC", "1.00", 61.7},
          {"ffpfsc-105", "Archive.org", "FFPFSC", "1.05", 63.0},
          {"exfat", "Archive.org", "exFAT", "1.05", 62.4},
          {"exfat-eu", "Archive.org", "exFAT", "1.05", 62.4}}},
        {"glass-orchard",
         "Glass Orchard",
         "Puzzle",
         "Grow light into shapes.",
         "A calm puzzle garden where beams of light grow into branches you can bend and split.",
         "Small Hours",
         "2026-06-02",
         false,
         "PPSA90004",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 3.4}}},
        {"starfall-rally",
         "Starfall Rally",
         "Racing",
         "Rally across a planet that is falling apart.",
         "Point-to-point rallies on a moon that sheds its surface lap by lap.",
         "Vector Nine",
         "2026-05-21",
         true,
         "PPSA90005",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 38.9}}},
        {"quiet-harbour",
         "Quiet Harbour",
         "Simulation",
         "Run a fishing town through four seasons.",
         "Keep a small harbour town fed, busy and kind through storms and festivals.",
         "Gull Works",
         "2026-04-11",
         false,
         "PPSA90006",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 9.8}}},
        {"ironclad-skies",
         "Ironclad Skies",
         "Action",
         "Fight for the last cloud city.",
         "Pilot a patched-up airship against an armada above a sea of cloud.",
         "Copperline",
         "2026-03-03",
         true,
         "PPSA90007",
         {{"viking", "Vikingfile", "exFAT", "", 70.1},
          {"ffpfsc", "Archive.org", "FFPFSC", "", 72.5}}},
        {"paper-lanterns",
         "Paper Lanterns",
         "Family",
         "Light the way home.",
         "Fold, float and guide paper lanterns through a festival night.",
         "Fold Studio",
         "2025-12-12",
         false,
         "PPSA90008",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 5.2}}},
        {"frostline",
         "Frostline",
         "Survival",
         "Hold the line against the long cold.",
         "Lead an expedition across an ice shelf as the weather turns.",
         "North Room",
         "2025-11-01",
         true,
         "PPSA90009",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 31.0}}},
        {"echo-valley",
         "Echo Valley",
         "Horror",
         "Something answers when you call.",
         "Explore a valley where every sound comes back changed.",
         "Hollow Tone",
         "",
         false,
         "PPSA90010",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 27.4}}},
        {"moonlit-atlas",
         "Moonlit Atlas",
         "Adventure",
         "Map the dark side of a friendly moon.",
         "Draw the map as you walk it, and share it with the travellers who come after you.",
         "Atlas Works",
         "2025-08-08",
         false,
         "PPSA90011",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 18.6}}},
        {"copper-circuit",
         "Copper Circuit",
         "Strategy",
         "Build the city's first power grid.",
         "Route copper, steam and clockwork through a growing city without blackouts.",
         "Gearbox Garden",
         "2025-06-20",
         true,
         "PPSA90012",
         {{"ffpfsc", "Archive.org", "FFPFSC", "", 12.1}}},
        {"copper-circuit-ps4",
         "Copper Circuit",
         "Strategy",
         "Build the city's first power grid.",
         "The PS4 edition of the power-grid puzzle game.",
         "Gearbox Garden",
         "2018-06-20",
         false,
         "CUSA90012",
         {{"pkg", "Vikingfile", "PKG", "1.00", 10.1}}},
    };
    return list;
}

std::string release_id(const FixtureGame &game, const Option &option)
{
    return std::string(game.id) + "-" + option.suffix;
}

std::string filename(const FixtureGame &game, const Option &option)
{
    return std::string(game.title_id) + "-" + option.suffix +
           (std::strcmp(option.format, "exFAT") == 0 ? ".exfat" :
            std::strcmp(option.format, "FPKG") == 0 || std::strcmp(option.format, "PKG") == 0 ? ".pkg" : ".ffpfsc");
}

std::uint32_t hash(const std::string &text)
{
    std::uint32_t h = 2166136261u;
    for (const char c : text)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h;
}

void hsv(float h, float s, float v, std::uint8_t *out)
{
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    const int sector = static_cast<int>(h * 6.0f) % 6;
    switch (sector)
    {
    case 0:
        r = c, g = x;
        break;
    case 1:
        r = x, g = c;
        break;
    case 2:
        g = c, b = x;
        break;
    case 3:
        g = x, b = c;
        break;
    case 4:
        r = x, b = c;
        break;
    default:
        r = c, b = x;
        break;
    }
    const float m = v - c;
    out[0] = static_cast<std::uint8_t>((r + m) * 255.0f);
    out[1] = static_cast<std::uint8_t>((g + m) * 255.0f);
    out[2] = static_cast<std::uint8_t>((b + m) * 255.0f);
}

void append(void *context, void *data, int size)
{
    static_cast<std::string *>(context)->append(static_cast<const char *>(data),
                                                static_cast<std::size_t>(size));
}

std::string error_body(const char *message)
{
    return std::string("{\"error\":") + json::quote(message) + "}";
}

// XENO STORE has no provider capture: Vikingfile pages need the browser version.
constexpr const char *kVikingfile =
    "Vikingfile asks for a human check, so XENO STORE cannot fetch it by itself. Download it "
    "with TorBox, or open this game in the browser version.";

} // namespace

Fixture::Fixture(FixtureMode mode, Dialect dialect)
    : mode_(mode), dialect_(dialect), running_(dialect == Dialect::xeno ? "1.2.1" : "0.6.0"),
      saved_(running_), tv_installed_(newer_tv_app(false))
{
    const auto add = [&](const char *game_id, int option, const char *storage, const char *status,
                         double fraction, double speed, const char *error)
    {
        for (const FixtureGame &game : games())
        {
            if (std::strcmp(game.id, game_id) != 0)
                continue;
            const Option &o = game.options[static_cast<std::size_t>(option)];
            Job job;
            job.id = "job" + std::to_string(next_job_++);
            job.release = release_id(game, o);
            job.game = game.id;
            job.title_id = game.title_id;
            job.title = game.title;
            job.storage = storage;
            job.filename = filename(game, o);
            job.format = o.format;
            job.status = status;
            job.error = error;
            job.total = o.gigabytes * 1e9;
            job.received = job.total * fraction;
            job.speed = speed;
            jobs_.push_back(job);
        }
    };
    add("tidewater", 0, "usb0", "downloading", 0.382, 24.6e6, "");
    add("starfall-rally", 0, "usb0", "queued", 0.0, 0.0, "");
    add("glass-orchard", 0, "internal", "paused", 0.35, 0.0, "");
    add("frostline", 0, "usb0", "error", 0.33, 0.0,
        "The provider stopped responding. Retry later.");
    add("quiet-harbour", 0, "usb0", "complete", 1.0, 0.0, "");
    add("paper-lanterns", 0, "usb0", "cancelled", 0.4, 0.0, "");
    if (mode_ != FixtureMode::setup)
    {
        enabled_ = {"archive"};
        acknowledged_ = true;
    }
    catalogue_checked_ = unix_now() - 25.0 * 60.0;
}

std::string Fixture::jobs_json() const
{
    std::string out = "[";
    for (std::size_t i = 0; i < jobs_.size(); ++i)
    {
        const Job &job = jobs_[i];
        char numbers[256];
        std::snprintf(
            numbers, sizeof(numbers),
            ",\"received\":%.0f,\"total\":%.0f,\"speed\":%.0f,\"retryAt\":%.0f,\"order\":%zu",
            job.received, job.total, job.speed, job.retry_at, i);
        if (i > 0)
            out += ",";
        out +=
            "{\"id\":" + json::quote(job.id) + ",\"releaseId\":" + json::quote(job.release) +
            ",\"gameId\":" + json::quote(job.game) + ",\"titleId\":" + json::quote(job.title_id) +
            ",\"title\":" + json::quote(job.title) + ",\"storageId\":" + json::quote(job.storage) +
            ",\"filename\":" + json::quote(job.filename) +
            ",\"format\":" + json::quote(job.format) +
            ",\"delivery\":" + json::quote(job.delivery) +
            ",\"path\":" + json::quote(job.status == "complete" ?
                "/mnt/usb0/homebrew/" + job.filename : "") + ",\"status\":" + json::quote(job.status) +
            ",\"error\":" + json::quote(job.error) +
            ",\"verification\":" + json::quote(job.status == "complete" ? "sha256" : "") + numbers +
            "}";
    }
    return out + "]";
}

std::string Fixture::art(const std::string &game, bool hero) const
{
    const int width = hero ? 960 : 512;
    const int height = hero ? 540 : 512;
    const std::uint32_t h = hash(game);
    const float hue_a = static_cast<float>(h % 360) / 360.0f;
    const float hue_b = std::fmod(hue_a + 0.12f + static_cast<float>((h >> 9) % 20) / 100.0f, 1.0f);
    std::uint8_t a[3];
    std::uint8_t b[3];
    hsv(hue_a, 0.65f, 0.55f, a);
    hsv(hue_b, 0.55f, 0.95f, b);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    const float cx = width * (0.35f + static_cast<float>((h >> 4) % 30) / 100.0f);
    const float cy = height * 0.42f;
    const float radius = std::min(width, height) * 0.24f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float t =
                (static_cast<float>(x) / width * 0.4f + static_cast<float>(y) / height * 0.6f);
            std::uint8_t *p = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c)
                p[c] = static_cast<std::uint8_t>(a[c] + (b[c] - a[c]) * t);
            // A soft sun and a horizon band: enough shape to judge cropping.
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d < radius)
            {
                const float k = std::clamp((radius - d) / 3.0f, 0.0f, 1.0f) * 0.85f;
                for (int c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(p[c] + (255 - p[c]) * k);
            }
            if (y > height * 0.68f)
            {
                const float wave =
                    std::sin(static_cast<float>(x) * 0.02f + static_cast<float>(h % 7)) * height *
                    0.02f;
                if (y > height * 0.7f + wave)
                    for (int c = 0; c < 3; ++c)
                        p[c] = static_cast<std::uint8_t>(p[c] * 0.45f);
            }
            p[3] = 255;
        }
    }
    std::string png;
    stbi_write_png_to_func(append, &png, width, height, 4, pixels.data(), width * 4);
    return png;
}

std::string Fixture::favourites_json() const
{
    std::string out = "[";
    for (std::size_t i = 0; i < favourites_.size(); ++i)
        out += (i > 0 ? "," : "") + json::quote(favourites_[i]);
    return out + "]";
}

std::string Fixture::library_json() const
{
    // ShadowMount's inventory: catalogue games in each state, and one
    // homebrew title without catalogue art.
    struct Entry
    {
        const char *game; // a fixture game, or nullptr
        const char *title_id;
        const char *title;
        const char *location;
        bool installed;
        bool mounted;
        bool on_drive;
    };
    static const Entry kEntries[] = {
        {"quiet-harbour", nullptr, nullptr, "USB storage 1", true, true, true},
        {"tidewater", nullptr, nullptr, "USB storage 1", true, false, false},
        {"glass-orchard", nullptr, nullptr, "Internal storage", false, false, true},
        {nullptr, "PPSA99050", "Homebrew UI Lab", "Internal storage", true, false, true},
    };
    std::string entries;
    for (const Entry &entry : kEntries)
    {
        std::string title_id = entry.title_id != nullptr ? entry.title_id : "";
        std::string title = entry.title != nullptr ? entry.title : "";
        std::string file = "PPSA99050.ffpkg";
        std::string format = "FFPKG";
        double bytes = 48.0e6;
        for (const FixtureGame &game : games())
        {
            if (entry.game != nullptr && std::strcmp(game.id, entry.game) == 0)
            {
                title_id = game.title_id;
                title = game.title;
                file = filename(game, game.options.back());
                format = game.options.back().format;
                bytes = game.options.back().gigabytes * 1e9;
            }
        }
        // XENO STORE mounts nothing and reports each source's size as it reads it.
        const bool xeno = dialect_ == Dialect::xeno;
        const std::string size =
            !xeno ? std::string("\"sizeBytes\":null,\"sizeStatus\":\"unknown\"")
            : entry.on_drive ? "\"sizeBytes\":" + number(bytes) + ",\"sizeStatus\":\"ready\""
                             : std::string("\"sizeBytes\":null,\"sizeStatus\":\"unavailable\"");
        const std::string root = std::strcmp(entry.location, "Internal storage") == 0
                                     ? "/data/homebrew"
                                     : "/mnt/usb0/homebrew";
        if (!entries.empty())
            entries += ",";
        entries += "{\"titleId\":" + json::quote(title_id) + ",\"title\":" + json::quote(title) +
                   ",\"platform\":\"ps5\",\"format\":" + json::quote(format) +
                   ",\"path\":" + json::quote(root + "/" + file) + ",\"runtimePath\":\"\"" +
                   ",\"sourceKey\":" +
                   json::quote(std::string(64, title_id.empty() ? 'a' : title_id.back())) +
                   ",\"location\":" + json::quote(entry.location) +
                   ",\"installed\":" + (entry.installed ? "true" : "false") +
                   ",\"mounted\":" + (entry.mounted && !xeno ? "true" : "false") +
                   ",\"onDrive\":" + (entry.on_drive ? "true" : "false") +
                   ",\"managed\":true,\"canManageSource\":true,\"installedSizeBytes\":null," +
                   size + "}";
    }
    const char *provider =
        dialect_ == Dialect::xeno
            ? "\"provider\":\"XENO STORE\",\"providerVersion\":\"1.2.1\","
            : "\"provider\":\"ShadowMount\",\"providerVersion\":\"1.7\",";
    // XENO STORE lists, rescans, copies and moves; it does not mount or measure.
    const char *capabilities =
        dialect_ == Dialect::xeno
            ? "\"rescan\",\"copy_game_source\",\"move_game_source\",\"storage_job_status\","
              "\"storage_job_cancel\",\"storage_space\""
            : "\"list_games\",\"rescan\",\"mount_game\",\"unmount_game\",\"copy_game_source\","
              "\"move_game_source\",\"storage_job_status\",\"storage_job_cancel\"";
    return std::string("{\"status\":\"ready\",\"message\":\"\",") + provider +
           "\"busy\":false,\"stale\":false,\"checkedAt\":1791100000,"
           "\"updatedAt\":1791100000,\"refreshAfter\":30,\"games\":[" +
           entries + "],\"capabilities\":[" + capabilities + "],\"action\":" +
           (library_action_.empty() ? std::string("null") : library_action_) +
           ",\"storageJob\":null,\"storageError\":\"\",\"storageBusy\":false}";
}

std::string Fixture::sources_json() const
{
    std::string enabled = "[";
    for (std::size_t i = 0; i < enabled_.size(); ++i)
        enabled += (i > 0 ? "," : "") + json::quote(enabled_[i]);
    enabled += "]";
    return "{\"enabled\":" + enabled + ",\"acknowledged\":" + (acknowledged_ ? "true" : "false") +
           ",\"noticeVersion\":1,\"options\":[{\"id\":\"vikingfile\",\"label\":\"Vikingfile\","
           "\"releaseCount\":2,\"downloadReady\":true},{\"id\":\"archive\",\"label\":"
           "\"Archive.org\",\"releaseCount\":15,\"downloadReady\":true}]}";
}

std::string Fixture::catalogue_json() const
{
    return "{\"available\":true,\"busy\":false,\"cached\":true,\"revision\":7,"
           "\"gameCount\":12,\"checkedAt\":" +
           number(catalogue_checked_) + ",\"updatedAt\":" + number(catalogue_checked_) +
           ",\"checkAfter\":" + number(catalogue_after_) + ",\"nextCheck\":0,\"error\":\"\"}";
}

std::string Fixture::newer_service(bool newer) const
{
    if (dialect_ == Dialect::xeno)
        return newer ? "1.2.2" : "1.2.1";
    return newer ? "0.6.1" : "0.6.0";
}

std::string Fixture::newer_tv_app(bool newer) const
{
    if (dialect_ == Dialect::xeno)
        return newer ? "1.2.2" : "1.2.1";
    return newer ? "1.1.0" : "1.0.0";
}

std::string Fixture::service_json() const
{
    const bool newer = mode_ == FixtureMode::updates;
    const std::string latest = service_checked_ > 0.0 ? newer_service(newer) : "";
    const bool available = !latest.empty() && latest != running_ && saved_ != latest;
    return "{\"available\":true,\"runningVersion\":" + json::quote(running_) +
           ",\"savedVersion\":" + json::quote(saved_) +
           ",\"latestVersion\":" + json::quote(latest) +
           ",\"checksum\":" + json::quote(latest.empty() ? "" : kChecksum) +
           ",\"updateAvailable\":" +
           (available || (!latest.empty() && latest != running_) ? "true" : "false") +
           ",\"automaticCheckAfter\":0,\"phase\":" + json::quote(service_phase_) +
           ",\"error\":\"\",\"busy\":false,\"restartRequired\":" +
           (saved_ != running_ ? "true" : "false") +
           ",\"received\":0,\"retryAt\":0,\"checkedAt\":" + number(service_checked_) +
           ",\"checkAfter\":0}";
}

std::string Fixture::tv_app_json() const
{
    const std::string latest =
        tv_checked_ > 0.0 ? newer_tv_app(mode_ == FixtureMode::updates) : "";
    return "{\"available\":true,\"installed\":\"orbit\",\"installedVersion\":" +
           json::quote(tv_installed_) + ",\"latestVersion\":" + json::quote(latest) +
           ",\"checksum\":" + json::quote(latest.empty() ? "" : kChecksum) +
           ",\"size\":" + (latest.empty() ? std::string("0") : std::string("71303168")) +
           ",\"updateAvailable\":" +
           (!latest.empty() && latest != tv_installed_ ? "true" : "false") +
           ",\"phase\":" + json::quote(tv_phase_) +
           ",\"error\":\"\",\"busy\":false,\"received\":0,\"retryAt\":0,\"checkedAt\":" +
           number(tv_checked_) + ",\"checkAfter\":0,\"automaticCheckAfter\":0}";
}

http::Response Fixture::send(const http::Request &request)
{
    http::Response response;
    if (mode_ == FixtureMode::offline)
    {
        response.error = "connection refused";
        return response;
    }
    Lock lock(mutex_);
    if (stopped_until_ > 0.0)
    {
        // Stopped from Settings: gone for a moment, then back as the saved version.
        if (steady_now() < stopped_until_)
        {
            response.error = "connection refused";
            return response;
        }
        stopped_until_ = 0.0;
        running_ = saved_;
        service_phase_ = "idle";
        service_checked_ = 0.0;
    }
    response.status = 200;
    response.content_type = "application/json";
    const std::string &path = request.path;
    const bool get = request.method == "GET";
    const bool authorised = request.bearer == kToken;
    if (get && path == "/api/v1/system")
    {
        response.body =
            "{\"name\":\"XENO STORE\",\"version\":" + json::quote(running_) +
            ",\"platform\":\"fixture\",\"paired\":" + (authorised ? "true" : "false") +
            ",\"localSessionAvailable\":true,\"stateHealthy\":true,\"catalogueRevision\":7,"
            "\"preferredStorage\":" +
            json::quote(preferred_) + ",\"deleteArchives\":" +
            (delete_archives_ ? "true" : "false") +
            ",\"concurrentDownloads\":" + std::to_string(concurrent_) + ",\"httpPort\":9095}";
        return response;
    }
    if (get && path == "/api/v1/sources")
    {
        response.body = sources_json();
        return response;
    }
    if (get && path == "/api/v1/catalog")
    {
        std::string out = "[";
        bool first = true;
        if (acknowledged_ && !enabled_.empty())
        {
            for (const FixtureGame &game : games())
            {
                for (const Option &option : game.options)
                {
                    char size[48];
                    std::snprintf(size, sizeof(size), "%.0f", option.gigabytes * 1e9);
                    if (!first)
                        out += ",";
                    first = false;
                    out += "{\"id\":" + json::quote(release_id(game, option)) +
                           ",\"gameId\":" + json::quote(game.id) +
                           ",\"titleId\":" + json::quote(game.title_id) +
                           ",\"title\":" + json::quote(game.title) +
                           ",\"genre\":" + json::quote(game.genre) +
                           ",\"tagline\":" + json::quote(game.tagline) +
                           ",\"description\":" + json::quote(game.description) +
                           ",\"sizeBytes\":" + size +
                           ",\"provider\":" + json::quote(option.provider) +
                           (std::strcmp(option.provider, "Vikingfile") == 0
                                ? ",\"sourceId\":\"vikingfile\",\"browserAvailable\":true,"
                                  "\"directAvailable\":false"
                                : ",\"sourceId\":\"archive\"") +
                           ",\"region\":" + json::quote(std::strstr(option.suffix, "eu") ? "EUR" : "USA") +
                           ",\"addedAt\":\"2026-10-05T12:00:00Z\"" +
                           ",\"format\":" + json::quote(option.format) + ",\"version\":" +
                           (option.version[0] ? json::quote(option.version) : "null") +
                           ",\"filename\":" + json::quote(filename(game, option)) +
                           ",\"artworkLayout\":" + json::quote(game.wide ? "wide" : "ambient") +
                           ",\"publisher\":" + json::quote(game.publisher) +
                           ",\"releaseDate\":" + (game.date[0] ? json::quote(game.date) : "null") +
                           ",\"consoleVerified\":false}";
                }
            }
        }
        response.body = out + "]";
        return response;
    }
    if (get && path == "/api/v1/session")
    {
        response.body = std::string("{\"token\":\"") + kToken +
                        "\",\"pairCode\":\"482915\",\"address\":\"192.168.1.20\"}";
        return response;
    }
    if (!authorised)
    {
        response.status = 401;
        response.body = error_body("Pair this device with your PS5 to continue.");
        return response;
    }
    if (get && path == "/api/v1/storage")
    {
        // XENO STORE's drive ids and 1-based labels; projected free space keeps
        // back the 240 MiB the backend reserves.
        response.body =
            "[{\"id\":\"usb0\",\"label\":\"USB storage 1\",\"path\":\"/mnt/usb0/homebrew\","
            "\"freeBytes\":1420000000000,\"totalBytes\":2000000000000,\"pendingBytes\":96700000000,"
            "\"projectedFreeBytes\":1323048341760,\"external\":true},"
            "{\"id\":\"internal\",\"label\":\"Internal storage\",\"path\":\"/data/homebrew\","
            "\"freeBytes\":41200000000,\"totalBytes\":825000000000,\"pendingBytes\":2200000000,"
            "\"projectedFreeBytes\":38748341760,\"external\":false}]";
        return response;
    }
    if (get && path == "/api/v1/downloads")
    {
        response.body = jobs_json();
        return response;
    }
    if (get && path == "/api/v1/debrid")
    {
        response.body =
            mode_ == FixtureMode::debrid
                ? R"({"provider":"torbox","available":true,"connected":true,"busy":false,"hosts":[{"sourceId":"archive","available":true,"maxFileSize":107374182400}]})"
                : R"({"provider":"torbox","available":true,"connected":false,"busy":false,"hosts":[]})";
        return response;
    }
    json::Value input;
    if (!get)
        json::parse(request.body, &input);
    if (path == "/api/v1/browser" && dialect_ == Dialect::orbit)
    {
        if (!get && input.text("action") == "start")
        {
            if (browser_state_ == "waiting")
            {
                response.status = 409;
                response.body = error_body("Finish or cancel the current verification first.");
                return response;
            }
            browser_title_.clear();
            for (const auto &game : games())
                for (const auto &option : game.options)
                    if (release_id(game, option) == input.text("releaseId") &&
                        std::string(option.provider) == "Vikingfile") browser_title_ = game.title;
            if (browser_title_.empty() || (input.text("storageId") != "usb0" && input.text("storageId") != "internal"))
            {
                response.status = 400;
                response.body = error_body("Unknown release or storage.");
                return response;
            }
            browser_id_ = "verification-" + std::to_string(++browser_count_);
            browser_state_ = "waiting";
            response.status = 202;
        }
        else if (!get && input.text("action") == "cancel")
        {
            if (input.text("id") != browser_id_ || browser_state_ != "waiting")
            {
                response.status = 409;
                response.body = error_body("This verification session is no longer active.");
                return response;
            }
            browser_state_ = "cancelled";
            response.status = 202;
        }
        response.body = "{\"available\":true,\"active\":" + std::string(browser_state_ == "waiting" ? "true" : "false") +
            ",\"id\":" + json::quote(browser_id_) + ",\"title\":" + json::quote(browser_title_) +
            ",\"state\":" + json::quote(browser_state_) + ",\"jobId\":\"\",\"message\":" +
            json::quote(browser_state_ == "waiting" ? "On the Vikingfile page, complete any verification and select Download. Then return to XENO STORE and open Downloads. XENO STORE checks the file before adding it to your queue." :
                        browser_state_ == "cancelled" ? "Verification cancelled. No download was added." : "") + "}";
        return response;
    }
    if (path == "/api/v1/browser")
    {
        // As XENO STORE answers: there is no provider capture, so the session
        // is always idle and a start is refused with the reason.
        if (!get)
        {
            const std::string action = input.text("action");
            response.status = action == "start" || action == "cancel" ? 409 : 400;
            response.body = error_body(action == "start"    ? kVikingfile
                                       : action == "cancel" ? "This verification session is no longer active."
                                                            : "Unknown browser action.");
            return response;
        }
        response.body = std::string("{\"available\":false,\"active\":false,\"id\":\"\",\"state\":"
                                    "\"idle\",\"title\":\"\",\"jobId\":\"\",\"message\":") +
                        json::quote(kVikingfile) + "}";
        return response;
    }
    if (!get && path == "/api/v1/settings/downloads")
    {
        // XENO STORE runs 1 to 4 downloads at once.
        const json::Value &limit = input["concurrentDownloads"];
        const double value = limit.as_number(0.0);
        if (!limit.is_number() || value != std::floor(value) || value < 1.0 || value > 4.0)
        {
            response.status = 400;
            response.body = error_body("Choose a whole number from 1 to 4.");
            return response;
        }
        concurrent_ = static_cast<int>(value);
        response.body = "{\"concurrentDownloads\":" + std::to_string(concurrent_) + "}";
        return response;
    }
    if (!get && path == "/api/v1/settings/extraction")
    {
        if (!input["deleteArchives"].is_bool())
        {
            response.status = 400;
            response.body = error_body("Expected deleteArchives.");
            return response;
        }
        delete_archives_ = input.flag("deleteArchives");
        response.body = std::string("{\"deleteArchives\":") + (delete_archives_ ? "true" : "false") + "}";
        return response;
    }
    if (!get && path == "/api/v1/sources")
    {
        enabled_.clear();
        for (const json::Value &id : input["enabled"].items())
            enabled_.push_back(id.as_string());
        acknowledged_ = input.flag("acknowledged");
        response.body = sources_json();
        return response;
    }
    if (!get && path == "/api/v1/storage/preferred")
    {
        preferred_ = input.text("storageId");
        response.body = "{\"preferredStorage\":" + json::quote(preferred_) + "}";
        return response;
    }
    if (path == "/api/v1/catalog/updates")
    {
        if (!get)
        {
            catalogue_checked_ = unix_now();
            catalogue_after_ = unix_now() + 60.0;
            response.status = 202;
        }
        response.body = catalogue_json();
        return response;
    }
    if (path == "/api/v1/updates")
    {
        const std::string action = input.text("action");
        if (action == "check")
        {
            service_checked_ = unix_now();
            service_phase_ = "checked";
        }
        else if (action == "install")
        {
            saved_ = mode_ == FixtureMode::updates ? newer_service(true) : running_;
            service_phase_ = "saved";
        }
        else if (action == "stop")
        {
            service_phase_ = "stopping";
            response.status = 202;
            response.body = service_json();
            stopped_until_ = steady_now() + 1.5;
            return response;
        }
        if (!get)
            response.status = 202;
        response.body = service_json();
        return response;
    }
    if (path == "/api/v1/tv-app")
    {
        const std::string action = input.text("action");
        if (action == "check")
        {
            tv_checked_ = unix_now();
            tv_phase_ = "checked";
        }
        else if (action == "install")
        {
            tv_installed_ = mode_ == FixtureMode::updates ? newer_tv_app(true) : tv_installed_;
            tv_phase_ = "installed";
        }
        if (!get)
            response.status = 202;
        response.body = tv_app_json();
        return response;
    }
    if (path == "/api/v1/favorites")
    {
        json::Value body;
        if (!get && json::parse(request.body, &body))
        {
            const std::string id = body.text("gameId");
            const auto at = std::find(favourites_.begin(), favourites_.end(), id);
            if (body.flag("favorite") && at == favourites_.end())
                favourites_.push_back(id);
            else if (!body.flag("favorite") && at != favourites_.end())
                favourites_.erase(at);
        }
        response.body = favourites_json();
        return response;
    }
    if (path == "/api/v1/library")
    {
        response.body = library_json();
        return response;
    }
    if (path == "/api/v1/library/storage")
    {
        // As XENO STORE answers: a drive is its mount root, a destination the
        // root's homebrew folder; ids are sha256("drive\n" + root) and
        // sha256("dest\n" + root + "/homebrew"), labels as the drive cards.
        const std::string usb = "d4ee56d1fc126c65f1b3aa4a4204f3834353a56fcd3a1bdf24ac0c9e3e3e9b35";
        const std::string internal = "ac8693c21e828063a2a2c899e2a4acd355b37eab840d30ac80efac48d33a5acb";
        const std::string usb_dest = "d0ddf736c01cb916986858777f4016031796771c162c119572f5fc2515dcaafd";
        const std::string internal_dest =
            "b32372df49f13b26371d51ecfc5fc76917ec9348db1311beda6089ad81c7d9a2";
        response.body =
            "{\"drives\":[{\"id\":\"" + usb +
            "\",\"path\":\"/mnt/usb0\",\"label\":\"USB storage 1\",\"mountPoint\":\"/mnt/usb0\","
            "\"filesystem\":\"\",\"readOnly\":false,\"totalBytes\":2000000000000,"
            "\"freeBytes\":1420000000000,\"usedBytes\":580000000000},{\"id\":\"" +
            internal +
            "\",\"path\":\"/data\",\"label\":\"Internal storage\",\"mountPoint\":\"/data\","
            "\"filesystem\":\"\",\"readOnly\":false,\"totalBytes\":825000000000,"
            "\"freeBytes\":41200000000,\"usedBytes\":783800000000}],\"destinations\":[{\"id\":\"" +
            usb_dest +
            "\",\"path\":\"/mnt/usb0/homebrew\",\"label\":\"USB storage 1\",\"mountPoint\":"
            "\"/mnt/usb0\",\"filesystem\":\"\",\"readOnly\":false,\"totalBytes\":2000000000000,"
            "\"freeBytes\":1420000000000,\"usedBytes\":580000000000},{\"id\":\"" +
            internal_dest +
            "\",\"path\":\"/data/homebrew\",\"label\":\"Internal storage\",\"mountPoint\":"
            "\"/data\",\"filesystem\":\"\",\"readOnly\":false,\"totalBytes\":825000000000,"
            "\"freeBytes\":41200000000,\"usedBytes\":783800000000}],\"busy\":false,\"stale\":false,"
            "\"updatedAt\":1791100000,\"error\":\"\"}";
        return response;
    }
    if (!get && path == "/api/v1/library/actions")
    {
        json::Value body;
        if (!json::parse(request.body, &body) || body.text("requestId").size() < 8)
        {
            response.status = 400;
            response.body =
                error_body("Expected a supported action and its current Library identifiers.");
            return response;
        }
        library_action_ = "{\"id\":" + json::quote(body.text("requestId")) +
                          ",\"action\":" + json::quote(body.text("action")) +
                          ",\"titleId\":" + json::quote(body.text("titleId")) +
                          ",\"state\":\"queued\",\"message\":\"Waiting to start\xE2\x80\xA6\"}";
        response.status = 202;
        response.body = library_json();
        return response;
    }
    if (get && path.rfind("/api/v1/art/", 0) == 0)
    {
        const std::string rest = path.substr(12);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos)
        {
            response.status = 404;
            response.body = error_body("Unknown game.");
            return response;
        }
        response.content_type = "image/png";
        response.body = art(rest.substr(0, slash), rest.substr(slash + 1) == "hero");
        return response;
    }
    if (!get && path == "/api/v1/downloads")
    {
        json::Value body;
        json::parse(request.body, &body);
        for (const FixtureGame &game : games())
        {
            for (const Option &option : game.options)
            {
                if (release_id(game, option) != body.text("releaseId"))
                    continue;
                Job job;
                job.id = "job" + std::to_string(next_job_++);
                job.release = release_id(game, option);
                job.game = game.id;
                job.title_id = game.title_id;
                job.title = game.title;
                job.storage = body.text("storageId");
                job.delivery = body.text("delivery") == "torbox" ? "torbox" : "direct";
                job.filename = filename(game, option);
                job.format = option.format;
                job.status = "queued";
                job.total = option.gigabytes * 1e9;
                jobs_.push_back(job);
                response.status = 201;
                response.body = "{\"id\":" + json::quote(job.id) + "}";
                return response;
            }
        }
        response.status = 404;
        response.body = error_body("Unknown release.");
        return response;
    }
    if (!get && path.rfind("/api/v1/downloads/", 0) == 0)
    {
        const std::string rest = path.substr(18);
        const std::size_t slash = rest.find('/');
        const std::string id = rest.substr(0, slash);
        const std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
        {
            if (it->id != id)
                continue;
            if (action == "pause")
                it->status = "paused", it->speed = 0.0;
            else if (action == "resume" || action == "retry")
                it->status = "queued", it->error.clear();
            else if (action == "cancel")
                it->status = "cancelled", it->speed = 0.0;
            else if (action == "delete" || action == "forget")
            {
                if (it->status != "cancelled")
                {
                    response.status = 409;
                    response.body = error_body("Only cancelled downloads can be deleted.");
                    return response;
                }
                jobs_.erase(it);
            }
            else if (action == "remove")
                jobs_.erase(it);
            response.body = jobs_json();
            return response;
        }
        response.status = 404;
        response.body = error_body("Unknown download.");
        return response;
    }
    response.status = 404;
    response.body = error_body("This request is not available in this version of XENO STORE.");
    return response;
}

} // namespace orbit::host
