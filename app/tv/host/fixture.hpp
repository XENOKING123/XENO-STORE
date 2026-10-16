// Orbit Store TV app - A stand-in Orbit backend for PC previews.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
//
// Answers the API calls the app makes with invented games, two drives and a
// queue in every state, and draws simple cover art as PNG. Nothing here is a
// real product or real artwork. Preview only: never part of the console app.

#pragma once

#include "orbit/http.hpp"
#include "orbit/thread.hpp"

#include <string>
#include <vector>

namespace orbit::host
{

enum class FixtureMode
{
    normal,  // a catalogue, drives, a queue, favourites and a Library
    debrid,  // normal fixture plus a connected TorBox account
    offline, // the backend is not running
    setup,   // sources not chosen yet
    updates, // as normal, with newer releases of the download service and the TV app
    // XENO STORE's Save Data Store (otherwise as normal):
    saves_empty,  // the store builds its index for two lists, then has no saves
    saves_older,  // a store without the Save Data Store (404 on /api/saves)
    saves_garlic, // installs fail: Garlic Save Manager cannot be started
    saves_failed, // the list answers 503 (the first answer after 1.2 s, so it shows loading)
    saves_partial, // the XenoKing site cannot be reached, then (after a reload) the
                   // Settings backend is not usable: the GitHub index only
};

// Whose answers the fixture gives where the two backends differ.
enum class Dialect
{
    // The reference backend the upstream unit tests exercise: native provider
    // verification (/browser starts and cancels Vikingfile sessions), service
    // versions 0.6.0, then 0.6.1 in the updates mode (TV app 1.0.0, then
    // 1.1.0), and ShadowMount's Library with mounting and measuring.
    orbit,
    // XENO STORE, as the preview shows it: no provider capture (/browser is
    // idle with available:false and the Vikingfile message, a start is
    // refused), service and TV app versions 1.2.1, then 1.2.2, and a Library
    // that copies and moves but neither mounts nor measures (sizes come with
    // the list).
    xeno,
};

class Fixture final : public http::Transport
{
  public:
    explicit Fixture(FixtureMode mode, Dialect dialect = Dialect::orbit);
    http::Response send(const http::Request &request) override;

  private:
    struct Job
    {
        std::string id;
        std::string release;
        std::string game;
        std::string title_id;
        std::string title;
        std::string storage;
        std::string filename;
        std::string format;
        std::string status;
        std::string error;
        double received = 0.0;
        double total = 0.0;
        double speed = 0.0;
        double retry_at = 0.0;
        std::string delivery = "direct";
    };
    // A save install the fixture runs (POST /api/saves/store/{id}/install).
    struct SaveJob
    {
        std::string id;
        std::string save;
        double size = 0.0;
        double started = 0.0; // steady seconds
        bool cancelled = false;
    };
    std::string jobs_json() const;
    std::string art(const std::string &game, bool hero) const;
    std::string saves_json();
    // state: the job's state now (running, done, ...), for the cancel's 404.
    std::string save_job_json(const SaveJob &job, std::string *state = nullptr) const;

    std::string library_json() const;
    std::string favourites_json() const;
    std::string sources_json() const;
    std::string catalogue_json() const;
    std::string newer_service(bool newer) const; // the dialect's current or next version
    std::string newer_tv_app(bool newer) const;
    std::string service_json() const;
    std::string tv_app_json() const;

    FixtureMode mode_;
    Dialect dialect_;
    Mutex mutex_;
    std::vector<Job> jobs_;
    int next_job_ = 100;
    std::vector<std::string> favourites_{"tidewater"};
    std::string library_action_; // the last accepted Library action, as JSON
    std::string browser_state_ = "idle"; // Dialect::orbit's provider verification
    std::string browser_id_;
    std::string browser_title_;
    int browser_count_ = 0;

    // Settings and updates.
    std::vector<std::string> enabled_;
    bool acknowledged_ = false;
    std::string preferred_ = "usb0";
    int concurrent_ = 1;          // simultaneous downloads, 1..4 as XENO STORE allows
    bool delete_archives_ = true; // remove a ZIP once its files are in place
    double catalogue_checked_ = 0.0; // Unix seconds
    double catalogue_after_ = 0.0;
    std::string running_; // the download service: the dialect's current version
    std::string saved_;
    double service_checked_ = 0.0;
    std::string service_phase_ = "idle";
    double stopped_until_ = 0.0; // after a stop: refused until then (steady seconds)
    std::string tv_installed_;
    double tv_checked_ = 0.0;
    std::string tv_phase_ = "idle";

    // The Save Data Store.
    std::vector<SaveJob> save_jobs_;
    int next_save_job_ = 1;
    int save_lists_ = 0; // lists answered, for saves_empty's building phase
};

} // namespace orbit::host
