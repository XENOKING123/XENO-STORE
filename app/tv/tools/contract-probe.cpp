// XENO STORE TV app - Contract probe: the app's own client and parser against a live backend.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: tv-contract-probe <ipv4>:<port> [options]
//
// Links the app's src/orbit/http.cpp, json.cpp, api.cpp and model.cpp (plus
// i18n.cpp and core/save_file.cpp, which they need) and calls every
// orbit::api::Client method once, in the order the app meets them: the console
// session first (its token pairs every later call), then the status, sources,
// catalogue, art, favourites, drives, settings, downloads and each download
// action, TorBox, the provider browser, the Library, the updates and (XENO
// STORE) the Save Data Store's routes outside /api/v1.
//
// One JSON line per call on stdout:
//   {"call", "method", "path", "status", "ok", "error", "retryAfter", "bytes",
//    "limit", "contentType", "ms", "parse", "parseFailure", "counts", "values"}
// "parse" is the app's json::parse on the raw body ("ok", "empty", "image",
// or the parser's reason); "parseFailure" is true when the client took a
// parse-failure path: an error on a 2xx reply, a 2xx JSON body the parser
// rejects (the calls that ignore it silently), or a refusal whose
// {"error": "..."} envelope could not be read. Lines with "call": "wait"
// report the queue polls between download actions (and a save install's job
// polls). The last line is
// {"call": "summary", ...}; the exit status is 1 when any call hit a parse
// failure or a transport error, 2 for bad arguments.
//
// options
//   --only a,b,c      run only these calls (names as printed; prefixes such as
//                     "job_action" match every action)
//   --release ID      the option whose download walks pause, resume, retry,
//                     cancel, archive-password, refresh-link and forget
//                     (default: the first option with a direct download)
//   --complete ID     an option followed to "complete", then delete and remove
//                     (default: none; those two calls are skipped)
//   --browser ID      a browser-only option for start_browser (default: the first one)
//   --game ID         the game for art and favourites (default: --release's game)
//   --storage ID      the drive for downloads and the preferred drive (default: the first)
//   --save ID         the community save installed, followed until bytes arrive,
//                     then cancelled (default: the first one listed)
//   --wait SECONDS    the longest wait for a job state or art (default 30)
//   --timeout MS      the transport timeout (default 8000, as the app)

#include "orbit/api.hpp"
#include "orbit/http.hpp"
#include "orbit/i18n.hpp"
#include "orbit/json.hpp"
#include "orbit/model.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

using namespace orbit;
using api::ArtKind;

// Keeps the last exchange so the probe can show what the client was given.
class RecordingTransport final : public http::Transport
{
  public:
    RecordingTransport(std::string host, std::uint16_t port, int timeout_ms)
        : inner_(std::move(host), port, timeout_ms)
    {
    }
    http::Response send(const http::Request &request) override
    {
        const auto start = std::chrono::steady_clock::now();
        last_request = request;
        last_response = inner_.send(request);
        last_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        return last_response;
    }
    http::Request last_request;
    http::Response last_response;
    double last_ms = 0.0;

  private:
    http::SocketTransport inner_;
};

// A JSON object written member by member.
class Obj
{
  public:
    Obj &str(std::string_view key, std::string_view value)
    {
        return raw(key, json::quote(value));
    }
    Obj &boolean(std::string_view key, bool value)
    {
        return raw(key, value ? "true" : "false");
    }
    Obj &num(std::string_view key, double value);
    Obj &strings(std::string_view key, const std::vector<std::string> &values)
    {
        std::string list = "[";
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
                list += ',';
            list += json::quote(values[i]);
        }
        list += ']';
        return raw(key, list);
    }
    Obj &obj(std::string_view key, const Obj &value)
    {
        return raw(key, value.text());
    }
    Obj &objects(std::string_view key, const std::vector<Obj> &values)
    {
        std::string list = "[";
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
                list += ',';
            list += values[i].text();
        }
        list += ']';
        return raw(key, list);
    }
    Obj &raw(std::string_view key, std::string_view value)
    {
        if (!body_.empty())
            body_ += ',';
        body_ += json::quote(key);
        body_ += ':';
        body_ += value;
        return *this;
    }
    std::string text() const
    {
        return "{" + body_ + "}";
    }

  private:
    std::string body_;
};

// A number as the probe prints it: an integer when it is one, else %.17g.
std::string number_text(double value)
{
    char buffer[64];
    if (value == static_cast<double>(static_cast<std::int64_t>(value)) && value < 9.2e18 &&
        value > -9.2e18)
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
    else
        std::snprintf(buffer, sizeof buffer, "%.17g", value);
    return buffer;
}

// Every field the app keeps of one option, tab-separated (the catalogue's fieldsHash).
std::string release_line(const orbit::Release &r)
{
    const std::string fields[] = {r.id,
                                  r.game_id,
                                  r.title_id,
                                  r.platform,
                                  r.title,
                                  r.genre,
                                  r.tagline,
                                  r.description,
                                  r.provider,
                                  r.source_id,
                                  r.format,
                                  r.region,
                                  r.version,
                                  r.filename,
                                  r.notes,
                                  r.artwork_layout,
                                  r.publisher,
                                  r.release_date,
                                  r.added_at,
                                  std::to_string(r.new_on_orbit_rank),
                                  number_text(r.size_bytes),
                                  r.compression,
                                  number_text(r.extracted_bytes),
                                  r.archive_unverified ? "1" : "0",
                                  r.browser_available ? "1" : "0",
                                  r.direct_available ? "1" : "0"};
    std::string line;
    for (const std::string &field : fields)
    {
        if (!line.empty())
            line += '\t';
        line += field;
    }
    return line;
}

std::string fnv1a(const std::vector<std::string> &values)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::string &value : values)
    {
        for (const unsigned char c : value)
        {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        hash ^= '\n';
        hash *= 1099511628211ull;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

Obj &Obj::num(std::string_view key, double value)
{
    return raw(key, number_text(value));
}

struct Options
{
    std::string host;
    std::uint16_t port = 0;
    std::vector<std::string> only;
    std::string release;
    std::string complete;
    std::string browser;
    std::string game;
    std::string storage;
    std::string save;
    int wait_seconds = 30;
    int timeout_ms = 8000;
};

class Probe
{
  public:
    explicit Probe(const Options &options)
        : options_(options), transport_(options.host, options.port, options.timeout_ms),
          client_(transport_)
    {
    }

    int run();

  private:
    bool wanted(std::string_view call) const
    {
        if (options_.only.empty())
            return true;
        for (const std::string &name : options_.only)
        {
            if (call == name || (call.size() > name.size() && call.substr(0, name.size()) == name &&
                                 call[name.size()] == ':'))
                return true;
        }
        return false;
    }

    // The common part of every line, from the client's result and the raw exchange.
    template <typename T>
    void emit(std::string_view call, const api::Result<T> &result, const Obj &counts,
              const Obj &values, bool image = false)
    {
        const http::Request &request = transport_.last_request;
        const http::Response &response = transport_.last_response;
        std::string parse = "ok";
        json::Value value;
        std::string reason;
        const bool success = response.status >= 200 && response.status < 300;
        if (response.status == 0)
            parse = "none";
        else if (response.body.empty())
            parse = "empty";
        else if (image && success && response.status != 202)
            parse = "image";
        else if (!json::parse(response.body, &value, &reason))
            parse = reason.empty() ? std::string("rejected") : reason;
        bool parse_failure = false;
        if (response.status != 0)
        {
            if (success && !result.ok())
                parse_failure = true; // the client refused a successful reply
            if (success && parse != "ok" && parse != "image" &&
                !(parse == "empty" && response.status == 202))
                parse_failure = true; // a 2xx body the app's parser rejects
            if (!success && result.error == tr("XENO STORE could not complete this request."))
                parse_failure = true; // the refusal's sentence envelope could not be read
        }
        if (parse_failure)
            ++parse_failures_;
        if (response.status == 0)
            ++transport_failures_;
        ++calls_;
        Obj line;
        line.str("call", call)
            .str("method", request.method)
            .str("path", request.path)
            .num("status", result.status)
            .boolean("ok", result.ok())
            .str("error", result.error)
            .num("retryAfter", result.retry_after)
            .num("bytes", static_cast<double>(response.body.size()))
            .num("limit", static_cast<double>(request.max_bytes != 0 ? request.max_bytes
                                                                     : http::kDefaultLimit))
            .str("contentType", response.content_type)
            .num("ms", static_cast<double>(static_cast<long>(transport_.last_ms)))
            .str("parse", parse)
            .boolean("parseFailure", parse_failure)
            .obj("counts", counts)
            .obj("values", values);
        std::printf("%s\n", line.text().c_str());
        std::fflush(stdout);
    }

    // Items in the last reply as the parser sees them (the whole array, or one
    // member's), before the model drops any; -1 when the body does not parse.
    double raw_count(std::string_view member = {}) const
    {
        json::Value value;
        if (!json::parse(transport_.last_response.body, &value))
            return -1.0;
        return static_cast<double>(member.empty() ? value.size() : value[member].size());
    }

    void wait_line(std::string_view job, std::string_view want, std::string_view got, long ms,
                   int polls)
    {
        Obj line;
        line.str("call", "wait")
            .str("job", job)
            .str("want", want)
            .str("got", got)
            .num("ms", static_cast<double>(ms))
            .num("polls", polls);
        std::printf("%s\n", line.text().c_str());
        std::fflush(stdout);
    }

    // Polls the queue (as the Downloads screen does) until the job has one of the states.
    std::string wait_for(const std::string &job_id, std::initializer_list<std::string_view> states)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(options_.wait_seconds);
        std::string got = "?";
        int polls = 0;
        std::string want;
        for (std::string_view s : states)
        {
            if (!want.empty())
                want += '|';
            want += s;
        }
        while (std::chrono::steady_clock::now() < deadline)
        {
            ++polls;
            const api::Result<std::vector<Job>> jobs = client_.downloads();
            got = jobs.ok() ? "missing" : "error: " + jobs.error;
            if (jobs.ok())
            {
                for (const Job &job : jobs.value)
                {
                    if (job.id == job_id)
                        got = job.status;
                }
            }
            bool done = false;
            for (std::string_view s : states)
                done = done || got == s;
            if (done)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
        wait_line(job_id, want, got, ms, polls);
        return got;
    }

    Obj jobs_counts(const std::vector<Job> &jobs) const
    {
        Obj counts;
        counts.num("jobs", static_cast<double>(jobs.size())).num("raw", raw_count());
        return counts;
    }

    static Obj jobs_values(const std::vector<Job> &jobs, const std::string &job_id)
    {
        Obj values;
        std::vector<std::string> ids;
        std::vector<std::string> states;
        const Job *target = nullptr;
        for (const Job &job : jobs)
        {
            ids.push_back(job.id);
            states.push_back(job.status);
            if (job.id == job_id)
                target = &job;
        }
        values.strings("ids", ids).strings("statuses", states);
        if (target != nullptr)
        {
            Obj job;
            job.str("id", target->id)
                .str("releaseId", target->release_id)
                .str("gameId", target->game_id)
                .str("titleId", target->title_id)
                .str("storageId", target->storage_id)
                .str("filename", target->filename)
                .str("format", target->format)
                .str("status", target->status)
                .str("error", target->error)
                .str("delivery", target->delivery)
                .num("total", target->total)
                .num("received", target->received)
                .num("order", target->order)
                .boolean("canRefreshLink", target->can_refresh_link)
                .boolean("canEnterArchivePassword", target->can_enter_archive_password);
            values.obj("job", job);
        }
        return values;
    }

    void action(const std::string &job_id, std::string_view name, bool delete_partial = false,
                std::string_view password = {})
    {
        const std::string call = "job_action:" + std::string(name);
        if (!wanted(call) || job_id.empty())
            return;
        const auto result = client_.job_action(job_id, name, delete_partial, password);
        emit(call, result, jobs_counts(result.value), jobs_values(result.value, job_id));
    }

    template <typename T> Obj library_values(const T &snapshot)
    {
        Obj values;
        std::vector<std::string> titles;
        for (const LibraryGame &game : snapshot.games)
            titles.push_back(game.title_id);
        std::vector<Obj> games;
        for (const LibraryGame &game : snapshot.games)
        {
            Obj item;
            item.str("titleId", game.title_id)
                .str("title", game.title)
                .str("platform", game.platform)
                .str("format", game.format)
                .str("path", game.path)
                .str("runtimePath", game.runtime_path)
                .str("sourceKey", game.source_key)
                .str("location", game.location)
                .boolean("installed", game.installed)
                .boolean("mounted", game.mounted)
                .boolean("onDrive", game.on_drive)
                .boolean("managed", game.managed)
                .boolean("canManageSource", game.can_manage_source)
                .num("installedSizeBytes", game.installed_size)
                .num("sizeBytes", game.size)
                .str("sizeStatus", game.size_status);
            games.push_back(item);
        }
        values.objects("games", games)
            .str("status", snapshot.status)
            .str("message", snapshot.message)
            .str("provider", snapshot.provider)
            .boolean("busy", snapshot.busy)
            .boolean("stale", snapshot.stale)
            .boolean("hasAction", snapshot.has_action)
            .str("actionState", snapshot.action.state)
            .str("actionId", snapshot.action.id)
            .boolean("hasStorageJob", snapshot.has_storage_job)
            .strings("capabilities", snapshot.capabilities)
            .strings("titleIds", titles);
        return values;
    }

    template <typename T> Obj storage_values(const T &storage)
    {
        Obj values;
        std::vector<std::string> drives;
        std::vector<std::string> destinations;
        for (const LibraryDrive &drive : storage.drives)
            drives.push_back(drive.id);
        for (const LibraryDrive &drive : storage.destinations)
            destinations.push_back(drive.id);
        const auto detail = [](const std::vector<LibraryDrive> &list)
        {
            std::vector<Obj> out;
            for (const LibraryDrive &drive : list)
            {
                Obj item;
                item.str("id", drive.id)
                    .str("path", drive.path)
                    .str("label", drive.label)
                    .str("filesystem", drive.filesystem)
                    .boolean("readOnly", drive.read_only)
                    .num("totalBytes", drive.total)
                    .num("freeBytes", drive.free)
                    .num("usedBytes", drive.used);
                out.push_back(item);
            }
            return out;
        };
        values.strings("drives", drives)
            .strings("destinations", destinations)
            .objects("drivesDetail", detail(storage.drives))
            .objects("destinationsDetail", detail(storage.destinations))
            .boolean("busy", storage.busy)
            .boolean("stale", storage.stale)
            .str("error", storage.error);
        return values;
    }

    static Obj update_values(const ServiceUpdate &update)
    {
        Obj values;
        values.boolean("available", update.available)
            .boolean("updateAvailable", update.update_available)
            .boolean("busy", update.busy)
            .boolean("restartRequired", update.restart_required)
            .str("runningVersion", update.running_version)
            .str("instanceId", update.instance_id)
            .str("savedVersion", update.saved_version)
            .str("latestVersion", update.latest_version)
            .str("phase", update.phase)
            .str("error", update.error);
        return values;
    }

    static Obj tv_app_values(const TvAppStatus &status)
    {
        Obj values;
        values.boolean("available", status.available)
            .str("installed", status.installed)
            .str("installedVersion", status.installed_version)
            .str("latestVersion", status.latest_version)
            .str("phase", status.phase)
            .boolean("busy", status.busy)
            .num("size", status.size)
            .str("error", status.error);
        return values;
    }

    static Obj catalogue_values(const CatalogueStatus &status)
    {
        Obj values;
        values.boolean("available", status.available)
            .boolean("busy", status.busy)
            .boolean("cached", status.cached)
            .num("revision", static_cast<double>(status.revision))
            .num("gameCount", status.game_count)
            .str("error", status.error);
        return values;
    }

    static Obj browser_values(const BrowserSession &session)
    {
        Obj values;
        values.boolean("available", session.available)
            .boolean("active", session.active)
            .str("id", session.id)
            .str("state", session.state)
            .str("title", session.title)
            .str("message", session.message)
            .str("jobId", session.job_id);
        return values;
    }

    static Obj save_job_values(const SaveJob &job)
    {
        Obj values;
        values.str("id", job.id)
            .str("type", job.type)
            .str("state", job.state)
            .str("message", job.message)
            .str("error", job.error)
            .str("errorCode", job.error_code)
            .num("done", job.done)
            .num("total", job.total)
            .num("percent", job.percent)
            .str("preBackup", job.pre_backup);
        return values;
    }

    // Reads a save install's job (as the Saves tab does, every second; here
    // every 100 ms) until it has one of the states, with bytes when asked.
    std::string wait_save_job(const std::string &job_id,
                              std::initializer_list<std::string_view> states, bool bytes)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(options_.wait_seconds);
        std::string got = "?";
        std::string want;
        for (std::string_view s : states)
            want += (want.empty() ? "" : "|") + std::string(s);
        int polls = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            ++polls;
            const api::Result<SaveJob> job = client_.save_job(job_id);
            got = job.ok() ? job.value.state : "error: " + job.error;
            bool done = false;
            for (std::string_view s : states)
                done = done || (got == s && (!bytes || job.value.done > 0.0));
            if (done)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
        wait_line(job_id, want, got, ms, polls);
        return got;
    }

    void saves();
    void art(ArtKind kind);

    Options options_;
    RecordingTransport transport_;
    api::Client client_;
    int calls_ = 0;
    int parse_failures_ = 0;
    int transport_failures_ = 0;
};

void Probe::art(ArtKind kind)
{
    const char *call = kind == ArtKind::hero ? "art:hero" : "art:cover";
    if (!wanted(call) || options_.game.empty())
        return;
    // The app asks again while the backend answers 202 (still fetching).
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(options_.wait_seconds);
    api::Result<api::Art> result = client_.art(options_.game, kind);
    const int first = result.status;
    int polls = 1;
    while (result.ok() && result.value.pending && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result = client_.art(options_.game, kind);
        ++polls;
    }
    Obj counts;
    counts.num("polls", polls).num("bytes", static_cast<double>(result.value.bytes.size()));
    Obj values;
    values.str("game", options_.game)
        .num("firstStatus", first)
        .boolean("pending", result.value.pending)
        .str("contentType", result.value.content_type);
    emit(call, result, counts, values, true);
}

// XENO STORE: the Save Data Store (docs/API.md "/api/saves/..."), as the Saves
// tab calls it: the list, the profiles, one install followed until bytes
// arrive and then cancelled, and an install of a save the store does not list.
void Probe::saves()
{
    std::string save = options_.save;
    if (wanted("save_store"))
    {
        const auto result = client_.save_store(false);
        if (save.empty() && !result.value.saves.empty())
            save = result.value.saves.front().id;
        std::vector<Obj> rows;
        for (const SaveRow &row : result.value.saves)
        {
            Obj item;
            item.str("id", row.id)
                .str("titleId", row.title_id)
                .str("gameId", row.game_id)
                .str("title", row.title)
                .str("name", row.name)
                .str("description", row.description)
                .str("author", row.author)
                .str("category", row.category)
                .str("gameVersion", row.game_version)
                .str("region", row.region)
                .str("createdAt", row.created_at)
                .str("source", row.source)
                .num("size", row.size)
                .boolean("installed", row.installed)
                .str("installedVersion", row.installed_version)
                .num("versionCompare", row.version_compare)
                .str("compat", row.compat)
                .str("compatText", row.compat_text);
            rows.push_back(item);
        }
        Obj counts;
        counts.num("saves", static_cast<double>(result.value.saves.size()))
            .num("raw", raw_count("saves"));
        Obj values;
        values.str("backend", result.value.backend)
            .str("backendName", result.value.backend_name)
            .boolean("building", result.value.building)
            .objects("saves", rows);
        emit("save_store", result, counts, values);
    }
    if (wanted("save_users"))
    {
        const auto result = client_.save_users();
        std::vector<Obj> users;
        for (const SaveProfile &profile : result.value)
        {
            Obj item;
            item.num("id", profile.id).str("name", profile.name).boolean("foreground", profile.foreground);
            users.push_back(item);
        }
        Obj counts;
        counts.num("users", static_cast<double>(result.value.size())).num("raw", raw_count("users"));
        Obj values;
        values.objects("users", users);
        emit("save_users", result, counts, values);
    }
    std::string job;
    if (wanted("install_save") && !save.empty())
    {
        // For the signed-in profile (0), always backing up its save first.
        const auto result = client_.install_save(save, 0);
        job = result.value.id;
        Obj values = save_job_values(result.value);
        values.str("save", save);
        emit("install_save", result, Obj(), values);
    }
    if (!job.empty() && wanted("save_job"))
    {
        wait_save_job(job, {"running"}, true);
        const auto result = client_.save_job(job);
        emit("save_job", result, Obj(), save_job_values(result.value));
    }
    if (!job.empty() && wanted("cancel_save_job"))
    {
        const auto result = client_.cancel_save_job(job);
        emit("cancel_save_job", result, Obj(), save_job_values(result.value));
        wait_save_job(job, {"cancelled"}, false);
    }
    if (wanted("install_save:unknown"))
    {
        const auto result = client_.install_save("probe-unknown-save", 0);
        Obj values;
        values.str("code", result.code);
        emit("install_save:unknown", result, Obj(), values);
    }
}

int Probe::run()
{
    // 1. The console session: its token pairs every later call.
    if (wanted("session"))
    {
        const auto result = client_.session();
        if (result.ok())
            client_.set_token(result.value.token);
        Obj values;
        values.num("tokenLength", static_cast<double>(result.value.token.size()))
            .num("pairCodeLength", static_cast<double>(result.value.pair_code.size()))
            .str("address", result.value.address);
        emit("session", result, Obj(), values);
    }
    if (client_.token().empty())
    {
        const auto result = client_.session();
        if (result.ok())
            client_.set_token(result.value.token);
    }

    if (wanted("system"))
    {
        const auto result = client_.system();
        const System &s = result.value;
        Obj values;
        values.str("name", s.name)
            .str("version", s.version)
            .str("instanceId", s.instance_id)
            .boolean("stopping", s.stopping)
            .str("platform", s.platform)
            .boolean("paired", s.paired)
            .boolean("localSessionAvailable", s.local_session)
            .boolean("stateHealthy", s.state_healthy)
            .num("catalogueRevision", static_cast<double>(s.catalogue_revision))
            .str("preferredStorage", s.preferred_storage)
            .num("concurrentDownloads", s.concurrent_downloads)
            .boolean("deleteArchives", s.delete_archives);
        emit("system", result, Obj(), values);
    }

    Sources sources;
    if (wanted("sources") || wanted("save_sources"))
    {
        const auto result = client_.sources();
        sources = result.value;
        if (wanted("sources"))
        {
            std::vector<std::string> ids;
            std::vector<std::string> counts_list;
            for (const SourceOption &option : sources.options)
            {
                ids.push_back(option.id);
                counts_list.push_back(option.id + "=" + std::to_string(option.release_count) +
                                      (option.download_ready ? "/ready" : ""));
            }
            Obj counts;
            counts.num("options", static_cast<double>(sources.options.size()))
                .num("raw", raw_count("options"));
            Obj values;
            values.strings("enabled", sources.enabled)
                .boolean("acknowledged", sources.acknowledged)
                .num("noticeVersion", sources.notice_version)
                .strings("options", ids)
                .strings("releaseCounts", counts_list);
            emit("sources", result, counts, values);
        }
    }
    if (wanted("save_sources"))
    {
        // The Settings screen saves the draft with the notice version it showed.
        const int notice = sources.notice_version > 0 ? sources.notice_version : 1;
        const auto result = client_.save_sources(sources.enabled, notice);
        Obj values;
        values.strings("enabled", result.value.enabled)
            .boolean("acknowledged", result.value.acknowledged)
            .num("noticeVersion", result.value.notice_version);
        emit("save_sources", result, Obj(), values);
    }

    std::vector<Release> releases;
    if (wanted("catalog") || options_.release.empty() || options_.browser.empty() ||
        options_.game.empty())
    {
        const auto result = client_.catalog();
        releases = result.value;
        if (wanted("catalog"))
        {
            const double raw = raw_count();
            // What the app spends after the transfer: the whole-document parse
            // and the model (as Client::catalog), then the cards (Store).
            const auto t0 = std::chrono::steady_clock::now();
            json::Value document;
            const bool parsed = json::parse(transport_.last_response.body, &document);
            const std::size_t again = parsed ? parse_catalog(document).size() : 0;
            const auto t1 = std::chrono::steady_clock::now();
            const std::vector<Game> games = group_games(releases);
            const auto t2 = std::chrono::steady_clock::now();
            std::vector<std::string> ids;
            std::vector<std::string> game_ids;
            std::vector<std::string> lines;
            double size_sum = 0.0;
            int browser_only = 0;
            int ranked = 0;
            int ps4 = 0;
            for (const Release &release : releases)
            {
                ids.push_back(release.id);
                lines.push_back(release_line(release));
                size_sum += release.size_bytes;
                browser_only += release.browser_only() ? 1 : 0;
                ranked += release.new_on_orbit_rank > 0 ? 1 : 0;
                ps4 += release.platform == "ps4" ? 1 : 0;
            }
            for (const Game &game : games)
                game_ids.push_back(game.id);
            Obj counts;
            counts.num("releases", static_cast<double>(releases.size()))
                .num("raw", raw)
                .num("dropped", raw >= 0 ? raw - static_cast<double>(releases.size()) : -1.0)
                .num("games", static_cast<double>(games.size()))
                .num("browserOnly", browser_only)
                .num("ranked", ranked)
                .num("ps4", ps4);
            Obj values;
            values.str("idsHash", fnv1a(ids))
                .str("fieldsHash", fnv1a(lines))
                .str("gamesHash", fnv1a(game_ids))
                .num("sizeSum", size_sum)
                .num("parseMs", static_cast<double>(static_cast<long>(
                                    std::chrono::duration<double, std::milli>(t1 - t0).count())))
                .num("groupMs", static_cast<double>(static_cast<long>(
                                    std::chrono::duration<double, std::milli>(t2 - t1).count())))
                .num("reparsed", static_cast<double>(again))
                .str("first", ids.empty() ? std::string() : ids.front())
                .str("last", ids.empty() ? std::string() : ids.back());
            emit("catalog", result, counts, values);
        }
    }

    // Defaults from the catalogue when the caller named none.
    Options &opts = options_;
    for (const Release &release : releases)
    {
        if (opts.release.empty() && release.direct_available)
            opts.release = release.id;
        if (opts.browser.empty() && release.browser_only())
            opts.browser = release.id;
    }
    if (opts.game.empty())
    {
        for (const Release &release : releases)
        {
            if (release.id == opts.release)
                opts.game = release.game_id;
        }
    }

    if (wanted("catalogue_status"))
    {
        const auto result = client_.catalogue_status();
        emit("catalogue_status", result, Obj(), catalogue_values(result.value));
    }
    if (wanted("refresh_catalogue"))
    {
        const auto result = client_.refresh_catalogue();
        emit("refresh_catalogue", result, Obj(), catalogue_values(result.value));
    }

    art(ArtKind::cover);
    art(ArtKind::hero);

    if (wanted("favourites"))
    {
        const auto result = client_.favourites();
        Obj counts;
        counts.num("favourites", static_cast<double>(result.value.size()));
        Obj values;
        values.strings("ids", result.value);
        emit("favourites", result, counts, values);
    }
    if (wanted("set_favourite") && !options_.game.empty())
    {
        const auto result = client_.set_favourite(options_.game, true);
        Obj counts;
        counts.num("favourites", static_cast<double>(result.value.size()));
        Obj values;
        values.strings("ids", result.value);
        emit("set_favourite", result, counts, values);
    }

    std::vector<Drive> drives;
    if (wanted("storage") || options_.storage.empty())
    {
        const auto result = client_.storage();
        drives = result.value;
        if (wanted("storage"))
        {
            std::vector<std::string> ids;
            std::vector<std::string> labels;
            int external = 0;
            int with_space = 0;
            for (const Drive &drive : drives)
            {
                ids.push_back(drive.id);
                labels.push_back(drive.label);
                external += drive.external ? 1 : 0;
                with_space += drive.free_bytes > 0 && drive.total_bytes >= drive.free_bytes ? 1 : 0;
            }
            Obj counts;
            counts.num("drives", static_cast<double>(drives.size()))
                .num("raw", raw_count())
                .num("external", external)
                .num("withSpace", with_space);
            std::vector<Obj> detail;
            for (const Drive &drive : drives)
            {
                Obj item;
                item.str("id", drive.id)
                    .str("label", drive.label)
                    .str("path", drive.path)
                    .num("freeBytes", drive.free_bytes)
                    .num("totalBytes", drive.total_bytes)
                    .num("pendingBytes", drive.pending_bytes)
                    .num("projectedFreeBytes", drive.projected_free_bytes)
                    .boolean("external", drive.external);
                detail.push_back(item);
            }
            Obj values;
            values.strings("ids", ids).strings("labels", labels).objects("drives", detail);
            emit("storage", result, counts, values);
        }
    }
    if (opts.storage.empty() && !drives.empty())
        opts.storage = drives.front().id;

    if (wanted("set_preferred_storage"))
    {
        const auto result = client_.set_preferred_storage(options_.storage);
        Obj values;
        values.str("preferredStorage", result.value);
        emit("set_preferred_storage", result, Obj(), values);
    }
    if (wanted("set_download_limit"))
    {
        const auto result = client_.set_download_limit(2);
        Obj values;
        values.num("concurrentDownloads", result.value);
        emit("set_download_limit", result, Obj(), values);
    }
    if (wanted("set_delete_archives"))
    {
        const auto result = client_.set_delete_archives(true);
        Obj values;
        values.boolean("deleteArchives", result.value);
        emit("set_delete_archives", result, Obj(), values);
    }

    if (wanted("debrid"))
    {
        const auto result = client_.debrid();
        Obj counts;
        counts.num("hosts", static_cast<double>(result.value.hosts.size()));
        std::vector<Obj> hosts;
        for (const Debrid::Host &host : result.value.hosts)
        {
            Obj item;
            item.str("sourceId", host.source_id)
                .boolean("available", host.available)
                .num("maxFileSize", host.max_file_size);
            hosts.push_back(item);
        }
        Obj values;
        values.boolean("available", result.value.available)
            .boolean("connected", result.value.connected)
            .boolean("busy", result.value.busy)
            .objects("hosts", hosts);
        emit("debrid", result, counts, values);
    }

    if (wanted("browser_session"))
    {
        const auto result = client_.browser_session();
        emit("browser_session", result, Obj(), browser_values(result.value));
    }
    std::string browser_id;
    if (wanted("start_browser") && !options_.browser.empty())
    {
        const auto result = client_.start_browser(options_.browser, options_.storage);
        browser_id = result.value.id;
        Obj values = browser_values(result.value);
        values.str("releaseId", options_.browser);
        emit("start_browser", result, Obj(), values);
    }
    if (wanted("cancel_browser"))
    {
        const auto result =
            client_.cancel_browser(browser_id.empty() ? std::string("none") : browser_id);
        emit("cancel_browser", result, Obj(), browser_values(result.value));
    }

    // Downloads: the list, one download walked through every action, and (with
    // --complete) a finished one for delete and remove.
    if (wanted("downloads"))
    {
        const auto result = client_.downloads();
        emit("downloads", result, jobs_counts(result.value), jobs_values(result.value, ""));
    }
    std::string job;
    if (wanted("create_download") || wanted("job_action"))
    {
        const auto result = client_.create_download(options_.release, options_.storage);
        job = result.value;
        Obj values;
        values.str("id", job).str("releaseId", options_.release).str("storageId", options_.storage);
        if (wanted("create_download"))
            emit("create_download", result, Obj(), values);
    }
    if (!job.empty() && wanted("job_action"))
    {
        wait_for(job, {"downloading"});
        action(job, "pause");
        wait_for(job, {"paused"});
        action(job, "resume");
        wait_for(job, {"downloading"});
        action(job, "retry");
        action(job, "cancel", true);
        wait_for(job, {"cancelled"});
        action(job, "archive-password", false, "probe-password");
        action(job, "refresh-link");
        action(job, "forget");
    }
    if (!options_.complete.empty() && (wanted("create_download:complete") || wanted("job_action")))
    {
        const auto result = client_.create_download(options_.complete, options_.storage);
        const std::string done = result.value;
        Obj values;
        values.str("id", done)
            .str("releaseId", options_.complete)
            .str("storageId", options_.storage);
        emit("create_download:complete", result, Obj(), values);
        if (!done.empty())
        {
            wait_for(done, {"complete", "error", "cancelled"});
            action(done, "delete");
            action(done, "remove");
        }
    }

    // The Library.
    if (wanted("library"))
    {
        const auto result = client_.library(false);
        Obj counts;
        counts.num("games", static_cast<double>(result.value.games.size()))
            .num("raw", raw_count("games"));
        emit("library", result, counts, library_values(result.value));
    }
    if (wanted("library:refresh"))
    {
        const auto result = client_.library(true);
        Obj counts;
        counts.num("games", static_cast<double>(result.value.games.size()))
            .num("raw", raw_count("games"));
        emit("library:refresh", result, counts, library_values(result.value));
    }
    if (wanted("library_storage"))
    {
        const auto result = client_.library_storage(false);
        Obj counts;
        counts.num("drives", static_cast<double>(result.value.drives.size()))
            .num("destinations", static_cast<double>(result.value.destinations.size()));
        emit("library_storage", result, counts, storage_values(result.value));
    }
    if (wanted("library_storage:refresh"))
    {
        const auto result = client_.library_storage(true);
        Obj counts;
        counts.num("drives", static_cast<double>(result.value.drives.size()))
            .num("destinations", static_cast<double>(result.value.destinations.size()));
        emit("library_storage:refresh", result, counts, storage_values(result.value));
    }
    if (wanted("library_action"))
    {
        // As the Library screen's "Scan again" (library.cpp: run("scan", true, ...)).
        api::Client::LibraryRequest request;
        request.action = "scan";
        request.confirmed = true;
        char id[48];
        std::snprintf(
            id, sizeof id, "probe-%lld",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count()));
        request.request_id = id;
        const auto result = client_.library_action(request);
        Obj counts;
        counts.num("games", static_cast<double>(result.value.games.size()))
            .num("raw", raw_count("games"));
        Obj values = library_values(result.value);
        values.str("requestId", request.request_id);
        emit("library_action", result, counts, values);
    }

    // Updates: the service's and this app's.
    if (wanted("service_update"))
    {
        const auto result = client_.service_update();
        emit("service_update", result, Obj(), update_values(result.value));
    }
    if (wanted("service_action"))
    {
        api::Client::UpdateRequest request;
        request.action = "check";
        const auto result = client_.service_action(request);
        emit("service_action", result, Obj(), update_values(result.value));
    }
    if (wanted("tv_app"))
    {
        const auto result = client_.tv_app();
        emit("tv_app", result, Obj(), tv_app_values(result.value));
    }
    if (wanted("tv_app_action"))
    {
        api::Client::UpdateRequest request;
        request.action = "check";
        const auto result = client_.tv_app_action(request);
        emit("tv_app_action", result, Obj(), tv_app_values(result.value));
    }

    saves();

    Obj summary;
    summary.str("call", "summary")
        .num("calls", calls_)
        .num("parseFailures", parse_failures_)
        .num("transportFailures", transport_failures_)
        .str("release", options_.release)
        .str("complete", options_.complete)
        .str("browser", options_.browser)
        .str("game", options_.game)
        .str("storage", options_.storage)
        .str("save", options_.save);
    std::printf("%s\n", summary.text().c_str());
    return parse_failures_ == 0 && transport_failures_ == 0 ? 0 : 1;
}

int usage()
{
    std::fprintf(
        stderr,
        "usage: tv-contract-probe <ipv4>:<port> [--only a,b] [--release ID] [--complete ID]\n"
        "       [--browser ID] [--game ID] [--storage ID] [--save ID] [--wait SECONDS]\n"
        "       [--timeout MS]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    Options options;
    const std::string target = argv[1];
    const std::size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0)
        return usage();
    options.host = target.substr(0, colon);
    const long port = std::strtol(target.c_str() + colon + 1, nullptr, 10);
    if (port < 1 || port > 65535)
        return usage();
    options.port = static_cast<std::uint16_t>(port);
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc)
            return usage();
        const std::string value = argv[++i];
        if (flag == "--only")
        {
            std::size_t start = 0;
            while (start <= value.size())
            {
                const std::size_t comma = value.find(',', start);
                const std::string name = value.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!name.empty())
                    options.only.push_back(name);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }
        else if (flag == "--release")
            options.release = value;
        else if (flag == "--complete")
            options.complete = value;
        else if (flag == "--browser")
            options.browser = value;
        else if (flag == "--game")
            options.game = value;
        else if (flag == "--storage")
            options.storage = value;
        else if (flag == "--save")
            options.save = value;
        else if (flag == "--wait")
            options.wait_seconds = std::atoi(value.c_str());
        else if (flag == "--timeout")
            options.timeout_ms = std::atoi(value.c_str());
        else
            return usage();
    }
    if (options.wait_seconds < 1 || options.timeout_ms < 100)
        return usage();
    Probe probe(options);
    return probe.run();
}
