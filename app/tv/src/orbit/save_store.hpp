// XENO STORE TV app - The Save Data Store's community saves, kept on a worker of their own.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The store's first answer to GET /api/saves/store can take 15 s (it builds
// its index then), and an install is a console job the app follows every
// second, so the saves have their own worker and transport: the Store's
// polls never wait for them. Results reach the frame loop through an inbox,
// as the Store's do; screens read a plain state and never wait.

#pragma once

#include "orbit/api.hpp"
#include "orbit/model.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace orbit
{

enum class SavesLoad : std::uint8_t
{
    idle,        // nobody asked for the saves yet
    loading,     // the first answer is on its way
    ready,       // listing holds the store's answer (it may still be building)
    unsupported, // this XENO STORE has no Save Data Store (the list is 404)
    failed,      // no answer: error says why
};

struct SavesState
{
    SavesLoad load = SavesLoad::idle;
    std::string error;
    SaveListing listing;
    std::uint64_t serial = 0; // changes with every answer
    bool have_profiles = false;
    std::vector<SaveProfile> profiles;

    const SaveRow *save(std::string_view id) const;
    // The store is still building its index: an empty list means "not yet".
    bool building() const
    {
        return load == SavesLoad::ready && listing.building;
    }
};

// The one install the player started: the save, then its job until it ends.
struct SaveInstall
{
    bool active = false; // asked for and not finished
    std::string save_id;
    std::string game; // the save's game, for the running-game sentence
    bool have_job = false;
    SaveJob job;
    std::string error; // the store refused to start it, or lost it
    bool cancelling = false;
    std::uint64_t serial = 0; // changes with every answer

    // It has ended (done, failed, cancelled or refused) and was not cleared.
    bool ended() const
    {
        return !active && (have_job || !error.empty());
    }
};

class SaveStore
{
  public:
    // The transport must outlive the store and wait long enough for the
    // store's first answer (15 s).
    explicit SaveStore(http::Transport &transport);
    ~SaveStore();
    SaveStore(const SaveStore &) = delete;
    SaveStore &operator=(const SaveStore &) = delete;

    void start();
    void stop();

    // ---- frame thread ----
    // The console session's token; nothing is asked for before it exists.
    void set_token(const std::string &token);
    // Applies whatever the worker finished.
    void tick();
    const SavesState &state() const
    {
        return state_;
    }
    const SaveInstall &install() const
    {
        return install_;
    }
    // A screen shows the saves (call it every frame it does): they are read
    // once, then again every few seconds while the store builds its index or
    // after a failure, and after five minutes.
    void want();
    // Reads the list again now; refresh asks the store to rebuild its index.
    void reload(bool refresh = false);
    // Installs one save for a profile (0: the signed-in one), backing up the
    // profile's current save first. One install at a time.
    void install_save(const SaveRow &save, std::uint32_t user);
    void cancel_install();
    // The outcome was shown: forget it.
    void clear_install();

    // ---- tests ----
    // Seconds before the list is asked again while the store builds (4),
    // after a failure (10), and between reads of a running job (1).
    void set_delays(double building, double failed, double job);
    // One worker step: does what is due; false when nothing was.
    bool work_once();

  private:
    void worker();
    void read_list(bool refresh);
    void post_job(api::Result<SaveJob> result, bool started);

    api::Client client_;
    Inbox inbox_;
    SavesState state_;   // frame thread
    SaveInstall install_; // frame thread

    // Shared with the worker.
    Mutex mutex_;
    Signal signal_;
    std::string token_;
    double wanted_at_ = -1e9;
    bool reload_ = false;
    bool refresh_ = false;
    bool start_install_ = false;
    std::string install_id_;
    std::uint32_t install_user_ = 0;
    bool cancel_ = false;
    bool stopping_ = false;
    double building_delay_ = 4.0;
    double failed_delay_ = 10.0;
    double job_delay_ = 1.0;
    Thread thread_;

    // Worker only.
    bool have_list_ = false;
    bool building_ = false;
    bool unsupported_ = false;
    bool have_profiles_ = false;
    double listed_at_ = 0.0;
    double next_list_ = 0.0;
    std::string job_id_;
    double next_job_ = 0.0;
};

} // namespace orbit
