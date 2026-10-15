// Orbit Store TV app - Typed calls to the Orbit backend's local API v1.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/api.hpp"
#include "orbit/i18n.hpp"
#include <cstdio>

namespace orbit::api
{

namespace
{

constexpr std::size_t kArtLimit = 24u << 20;

} // namespace

bool safe_path_segment(std::string_view value)
{
    if (value.empty() || value.size() > 128)
        return false;
    for (const char c : value)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            return false;
    }
    return value != "." && value != "..";
}

http::Response Client::send(const char *method, std::string path, std::string body,
                            std::size_t max_bytes)
{
    http::Request request;
    request.method = method;
    request.path = std::move(path);
    request.body = std::move(body);
    request.bearer = token_;
    request.max_bytes = max_bytes;
    return transport_.send(request);
}

template <typename T> bool Client::fail(const http::Response &response, Result<T> *result)
{
    result->status = response.status;
    result->retry_after = response.retry_after;
    if (!response.error.empty())
    {
        result->error = response.error;
        return true;
    }
    if (response.status >= 200 && response.status < 300)
        return false;
    // The backend explains every refusal as {"error": "..."}.
    json::Value value;
    if (json::parse(response.body, &value) && !value.text("error").empty())
        result->error = value.text("error");
    else
        result->error = tr("Orbit could not complete this request.");
    return true;
}

Result<System> Client::system()
{
    Result<System> result;
    const http::Response response = send("GET", "/api/v1/system");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable status.");
    else
        result.value = parse_system(value);
    refresh_zero();
    result.value.zero_enabled = zero_enabled_;
    result.value.zero_configured = !zero_address_.empty();
    result.value.zero_address = zero_address_;
    if (zero_enabled_) {
        const auto remote = zero_send("/api/native/status");
        json::Value status;
        if (remote.status == 200 && json::parse(remote.body,&status) && status.is_object()) {
            result.value.catalogue_revision = status["revision"].as_int();
            result.value.zero_preference = status.text("preference");
        } else result.value.zero_error = "Orbit Zero is unavailable. Keep it open on your computer and reconnect if its address changed.";
    }
    return result;
}

void Client::refresh_zero() {
    http::Request req; req.path="/api/v1/zero/companion";req.bearer=token_;req.max_bytes=4096;
    const auto response=transport_.send(req);json::Value value;
    if(response.status!=200 || !json::parse(response.body,&value)) {
        // Older services have no companion. A transient failure must not silently reroute a download.
        if(response.status==404) {zero_enabled_=false;zero_address_.clear();zero_token_.clear();}
        return;
    }
    const auto address=value.text("address"),token=value.text("token");
    unsigned a,b,c,d;int end=0;
    const bool valid=std::sscanf(address.c_str(),"%u.%u.%u.%u%n",&a,&b,&c,&d,&end)==4 &&
        end==static_cast<int>(address.size()) && a<=255 && b<=255 && c<=255 && d<=255 &&
        (a==10 || (a==172 && b>=16 && b<=31) || (a==192 && b==168));
    const int port=static_cast<int>(value["port"].as_int());
    if(!valid || token.size()!=64 || token.find_first_not_of("0123456789abcdef")!=std::string::npos || port<1024 || port>65535) {
        zero_enabled_=false;zero_address_.clear();zero_token_.clear();return;
    }
    zero_address_=address;zero_port_=port;zero_token_=token;zero_enabled_=value.flag("enabled");
}
http::Response Client::zero_send(std::string path,const char *method,std::string body,std::size_t limit) {
    if(zero_address_.empty()) {http::Response r;r.status=409;r.body="{\"error\":\"Connect Orbit Zero on your computer first.\"}";return r;}
    const bool quick=path=="/api/native/status" || path=="/api/native/downloads";
    http::SocketTransport transport(zero_address_,static_cast<std::uint16_t>(zero_port_),quick ? 2000 : 30000);
    http::Request request;request.path=std::move(path);request.method=method;request.body=std::move(body);
    request.bearer=zero_token_;request.max_bytes=limit;
    auto response=companion_ ? companion_->send(request) : transport.send(request);
    if(!response.error.empty()) {response.error.clear();response.status=503;response.body="{\"error\":\"Orbit Zero is unavailable. Keep it open on your computer.\"}";}
    return response;
}
Result<bool> Client::set_zero(bool enabled) {
    Result<bool> result;const auto response=send("POST","/api/v1/zero/companion",enabled?
        "{\"action\":\"enable\",\"enabled\":true}":"{\"action\":\"enable\",\"enabled\":false}");
    if(!fail(response,&result)){refresh_zero();result.value=zero_enabled_;}return result;
}

Result<Sources> Client::sources()
{
    Result<Sources> result;
    const http::Response response = send("GET", "/api/v1/sources");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable source settings.");
    else
        result.value = parse_sources(value);
    return result;
}

Result<Debrid> Client::debrid()
{
    Result<Debrid> result;
    const http::Response response = send("GET", "/api/v1/debrid");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = "Orbit sent unreadable TorBox availability.";
    else
        result.value = parse_debrid(value);
    return result;
}

Result<std::vector<Release>> Client::catalog()
{
    Result<std::vector<Release>> result;
    const http::Response response = zero_enabled_ ? zero_send("/api/native/catalogue","GET",{},16u<<20)
        : send("GET", "/api/v1/catalog", {}, 16u << 20);
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent an unreadable catalogue.");
    else
        result.value = parse_catalog(value);
    return result;
}

Result<BrowserSession> Client::browser_request(std::string body)
{
    Result<BrowserSession> result;
    const bool read = body.empty();
    const auto response = send(read ? "GET" : "POST", "/api/v1/browser", std::move(body), 16384);
    if (fail(response, &result)) return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object() ||
        (value.text("state") != "idle" && value.text("state") != "opening" &&
         value.text("state") != "waiting" && value.text("state") != "waiting-link" &&
         value.text("state") != "validating" &&
         value.text("state") != "queued" && value.text("state") != "failed" &&
         value.text("state") != "cancelled") ||
        (value.flag("active") && value.text("id").empty()))
        result.error = tr("Orbit sent an unreadable verification status.");
    else result.value = parse_browser_session(value);
    return result;
}

Result<BrowserSession> Client::browser_session() { return browser_request(); }

Result<BrowserSession> Client::start_browser(std::string_view release_id, std::string_view storage_id)
{
    return browser_request("{\"action\":\"start\",\"releaseId\":" + json::quote(release_id) +
                           ",\"storageId\":" + json::quote(storage_id) + "}");
}

Result<BrowserSession> Client::cancel_browser(std::string_view session_id)
{
    return browser_request("{\"action\":\"cancel\",\"id\":" + json::quote(session_id) + "}");
}

Result<Session> Client::session()
{
    Result<Session> result;
    const http::Response response = send("GET", "/api/v1/session");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || value.text("token").size() != 64)
        result.error = tr("Orbit did not open a console session.");
    else
        result.value = parse_session(value);
    return result;
}

Result<std::vector<Drive>> Client::storage()
{
    Result<std::vector<Drive>> result;
    const http::Response response = send("GET", "/api/v1/storage");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent unreadable storage details.");
    else
        result.value = parse_storage(value);
    return result;
}

Result<std::vector<std::string>> Client::favourites()
{
    Result<std::vector<std::string>> result;
    const http::Response response = send("GET", "/api/v1/favorites");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent unreadable favourites.");
    else
        result.value = parse_favourites(value);
    return result;
}

Result<std::vector<std::string>> Client::set_favourite(std::string_view game_id, bool favourite)
{
    Result<std::vector<std::string>> result;
    std::string body = "{\"gameId\":";
    body += json::quote(game_id);
    body += favourite ? ",\"favorite\":true}" : ",\"favorite\":false}";
    const http::Response response = send("POST", "/api/v1/favorites", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_array())
        result.value = parse_favourites(value);
    return result;
}

Result<LibrarySnapshot> Client::library(bool refresh)
{
    Result<LibrarySnapshot> result;
    const http::Response response =
        refresh ? send("POST", "/api/v1/library", "{}") : send("GET", "/api/v1/library");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable Library.");
    else
        result.value = parse_library(value);
    return result;
}

Result<LibraryStorage> Client::library_storage(bool refresh)
{
    Result<LibraryStorage> result;
    const http::Response response = refresh ? send("POST", "/api/v1/library/storage", "{}")
                                            : send("GET", "/api/v1/library/storage");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable drive details.");
    else
        result.value = parse_library_storage(value);
    return result;
}

Result<LibrarySnapshot> Client::library_action(const LibraryRequest &request)
{
    Result<LibrarySnapshot> result;
    std::string body = "{\"action\":";
    body += json::quote(request.action);
    if (!request.title_id.empty())
    {
        body += ",\"titleId\":";
        body += json::quote(request.title_id);
    }
    if (!request.source_key.empty())
    {
        body += ",\"sourceKey\":";
        body += json::quote(request.source_key);
    }
    if (!request.destination_id.empty())
    {
        body += ",\"destinationId\":";
        body += json::quote(request.destination_id);
    }
    if (request.job_id > 0)
        body += ",\"jobId\":" + std::to_string(request.job_id);
    if (request.confirmed)
        body += ",\"confirmed\":true";
    body += ",\"requestId\":";
    body += json::quote(request.request_id);
    body += "}";
    const http::Response response = send("POST", "/api/v1/library/actions", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_library(value);
    return result;
}

Result<std::vector<Job>> Client::downloads()
{
    Result<std::vector<Job>> result;
    const http::Response response = send("GET", "/api/v1/downloads");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent an unreadable download queue.");
    else
        result.value = parse_jobs(value);
    if(result.ok() && !zero_address_.empty()) {
        const auto remote=zero_send("/api/native/downloads");json::Value jobs;
        if(remote.status==200 && json::parse(remote.body,&jobs) && jobs.is_array()) {
            zero_jobs_=parse_jobs(jobs);
        } else {
            for(auto &job:zero_jobs_) {
                job.speed=job.download_speed=0;
                if(job.status!="complete" && job.status!="cancelled") {
                    job.status="error";job.error="Computer unavailable. Reopen Orbit Zero to refresh this transfer.";
                }
            }
        }
        result.value.insert(result.value.end(),zero_jobs_.begin(),zero_jobs_.end());
    }
    return result;
}

Result<std::string> Client::create_download(std::string_view release_id,
                                            std::string_view storage_id, bool torbox)
{
    Result<std::string> result;
    std::string body = "{\"releaseId\":";
    body += json::quote(release_id);
    body += ",\"storageId\":";
    body += json::quote(storage_id);
    if (torbox)
        body += ",\"delivery\":\"torbox\"";
    body += "}";
    const http::Response response = zero_enabled_ ? zero_send("/api/native/download","POST",std::move(body))
        : send("POST", "/api/v1/downloads", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value))
        result.value = value.flag("browser") ? "zero-provider" : value.text("id");
    return result;
}

Result<std::vector<Job>> Client::job_action(std::string_view job_id, std::string_view action,
                                            bool delete_partial, std::string_view password)
{
    Result<std::vector<Job>> result;
    if (!safe_path_segment(job_id) || !safe_path_segment(action))
    {
        result.error = tr("Unknown download action.");
        return result;
    }
    if(job_id.substr(0,5)=="zero-") {
        const std::string mapped=action=="cancel" && !delete_partial ? "pause" : std::string(action);
        std::string body = "{\"id\":"+json::quote(job_id)+",\"action\":"+json::quote(mapped);
        if (action == "archive-password") body += ",\"password\":" + json::quote(password);
        body += "}";
        const auto response=zero_send("/api/native/action","POST",std::move(body));
        if(fail(response,&result))return result;
        return downloads();
    }
    std::string path = "/api/v1/downloads/";
    path += job_id;
    path += '/';
    path += action;
    const http::Response response =
        send("POST", std::move(path), action == "archive-password"
             ? "{\"password\":" + json::quote(password) + "}"
             : delete_partial ? "{\"deletePartial\":true}" : "{}");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_array())
        result.value = parse_jobs(value);
    return result;
}

Result<Art> Client::art(std::string_view game_id, ArtKind kind)
{
    Result<Art> result;
    if (!safe_path_segment(game_id))
    {
        result.error = tr("Unknown game.");
        return result;
    }
    std::string path = "/api/v1/art/";
    path += game_id;
    path += kind == ArtKind::hero ? "/hero" : "/cover";
    refresh_zero();
    const http::Response response = zero_enabled_ ? zero_send("/api/native/art/"+std::string(game_id)+(kind==ArtKind::hero?"/hero":"/cover"),"GET",{},kArtLimit)
        : send("GET", std::move(path), {}, kArtLimit);
    if (fail(response, &result))
        return result;
    if (response.status == 202)
    {
        result.value.pending = true;
        return result;
    }
    result.value.bytes = response.body;
    result.value.content_type = response.content_type;
    return result;
}

namespace
{

std::string update_body(const Client::UpdateRequest &request)
{
    std::string body = "{\"action\":";
    body += json::quote(request.action);
    if (!request.version.empty())
    {
        body += ",\"version\":";
        body += json::quote(request.version);
    }
    if (!request.checksum.empty())
    {
        body += ",\"checksum\":";
        body += json::quote(request.checksum);
    }
    if (request.automatic)
        body += ",\"automatic\":true";
    if (request.confirmed)
        body += ",\"confirmed\":true";
    if (request.replace)
        body += ",\"replace\":true";
    body += "}";
    return body;
}

} // namespace

Result<Sources> Client::save_sources(const std::vector<std::string> &enabled, int notice_version)
{
    Result<Sources> result;
    std::string body = "{\"enabled\":[";
    for (std::size_t i = 0; i < enabled.size(); ++i)
    {
        if (i > 0)
            body += ",";
        body += json::quote(enabled[i]);
    }
    body += "],\"acknowledged\":true,\"noticeVersion\":";
    body += std::to_string(notice_version);
    body += "}";
    const http::Response response = send("POST", "/api/v1/sources", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable source settings.");
    else
        result.value = parse_sources(value);
    return result;
}

Result<std::string> Client::set_preferred_storage(std::string_view storage_id)
{
    Result<std::string> result;
    std::string body = "{\"storageId\":";
    body += json::quote(storage_id);
    body += "}";
    const http::Response response = send("POST", "/api/v1/storage/preferred", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value))
        result.value = value.text("preferredStorage");
    return result;
}

Result<int> Client::set_download_limit(int limit)
{
    Result<int> result;
    if (limit < 1 || limit > 8) { result.error = tr("Choose a whole number from 1 to 8."); return result; }
    const http::Response response = send("POST", "/api/v1/settings/downloads",
        "{\"concurrentDownloads\":" + std::to_string(limit) + "}");
    if (fail(response, &result)) return result;
    json::Value value;
    if (!json::parse(response.body, &value) || value["concurrentDownloads"].as_int() < 1 ||
        value["concurrentDownloads"].as_int() > 8)
        result.error = tr("Could not save the setting.");
    else result.value = static_cast<int>(value["concurrentDownloads"].as_int());
    return result;
}

Result<bool> Client::set_delete_archives(bool enabled)
{
    Result<bool> result;
    const http::Response response = send("POST", "/api/v1/settings/extraction",
                                         enabled ? "{\"deleteArchives\":true}" : "{\"deleteArchives\":false}");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value))
        result.value = value.flag("deleteArchives", enabled);
    return result;
}

Result<CatalogueStatus> Client::catalogue_status()
{
    Result<CatalogueStatus> result;
    const http::Response response = send("GET", "/api/v1/catalog/updates");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable catalogue status.");
    else
        result.value = parse_catalogue_status(value);
    return result;
}

Result<CatalogueStatus> Client::refresh_catalogue()
{
    Result<CatalogueStatus> result;
    const http::Response response =
        send("POST", "/api/v1/catalog/updates", "{\"action\":\"refresh\"}");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_catalogue_status(value);
    return result;
}

Result<ServiceUpdate> Client::service_update()
{
    Result<ServiceUpdate> result;
    const http::Response response = send("GET", "/api/v1/updates");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable update status.");
    else
        result.value = parse_service_update(value);
    return result;
}

Result<ServiceUpdate> Client::service_action(const UpdateRequest &request)
{
    Result<ServiceUpdate> result;
    const http::Response response = send("POST", "/api/v1/updates", update_body(request));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_service_update(value);
    return result;
}

Result<TvAppStatus> Client::tv_app()
{
    Result<TvAppStatus> result;
    const http::Response response = send("GET", "/api/v1/tv-app");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable TV app status.");
    else
        result.value = parse_tv_app(value);
    return result;
}

Result<TvAppStatus> Client::tv_app_action(const UpdateRequest &request)
{
    Result<TvAppStatus> result;
    const http::Response response = send("POST", "/api/v1/tv-app", update_body(request));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_tv_app(value);
    return result;
}

} // namespace orbit::api
