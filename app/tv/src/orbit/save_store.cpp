// XENO STORE TV app - The Save Data Store's community saves, kept on a worker of their own.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/save_store.hpp"

#include "orbit/i18n.hpp"

#include <utility>

namespace orbit
{

namespace
{

// A screen that stopped calling want() this long ago no longer shows the saves.
constexpr double kWantedFor = 2.0;
// A list the store answered is read again after this long while it is shown.
constexpr double kListFresh = 300.0;
// The worker looks for work at least this often (job reads, retries).
constexpr double kIdleWait = 0.25;

} // namespace

std::string SaveInstall::outcome() const
{
    // A job the store lost while it ran ends with that error, not the job's last state.
    if (have_job && (job.finished() || error.empty()))
        return save_install_outcome(job, game, profile);
    return error;
}

const SaveRow *SavesState::save(std::string_view id) const
{
    for (const SaveRow &row : listing.saves)
    {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

SaveStore::SaveStore(http::Transport &transport) : client_(transport)
{
}

SaveStore::~SaveStore()
{
    stop();
}

void SaveStore::start()
{
    thread_.start([this] { worker(); });
}

void SaveStore::stop()
{
    {
        Lock lock(mutex_);
        stopping_ = true;
    }
    signal_.notify();
    thread_.join();
}

void SaveStore::set_token(const std::string &token)
{
    bool changed = false;
    {
        Lock lock(mutex_);
        changed = token_ != token;
        if (changed)
        {
            forget_ = forget_ || !token_.empty();
            token_ = token;
        }
    }
    if (changed)
        signal_.notify();
}

void SaveStore::set_instance(const std::string &instance)
{
    bool restarted = false;
    {
        Lock lock(mutex_);
        if (instance.empty() || instance == instance_)
            return;
        restarted = !instance_.empty();
        instance_ = instance;
        forget_ = forget_ || restarted;
    }
    if (restarted)
        signal_.notify();
}

void SaveStore::tick()
{
    inbox_.drain();
}

void SaveStore::want()
{
    if (state_.load == SavesLoad::idle)
        state_.load = SavesLoad::loading;
    bool woke = false;
    {
        Lock lock(mutex_);
        const double now = now_seconds();
        woke = now - wanted_at_ >= kWantedFor;
        wanted_at_ = now;
    }
    if (woke)
        signal_.notify();
}

void SaveStore::reload(bool refresh)
{
    if (state_.load != SavesLoad::ready)
        state_.load = SavesLoad::loading;
    {
        Lock lock(mutex_);
        reload_ = true;
        refresh_ = refresh_ || refresh;
    }
    signal_.notify();
}

void SaveStore::install_save(const SaveRow &save, std::uint32_t user)
{
    if (install_.active)
        return;
    const std::uint64_t serial = install_.serial;
    install_ = {};
    install_.active = true;
    install_.run = ++runs_;
    install_.save_id = save.id;
    install_.name = save.name;
    install_.game = save.title.empty() ? save.title_id : save.title;
    // The profile by name (0 is the signed-in one), for the outcome.
    for (const SaveProfile &profile : state_.profiles)
    {
        if (profile.id == user || (user == 0 && profile.foreground))
            install_.profile = profile.name;
    }
    install_.serial = serial + 1;
    {
        Lock lock(mutex_);
        start_install_ = true;
        install_id_ = save.id;
        install_user_ = user;
    }
    signal_.notify();
}

void SaveStore::cancel_install()
{
    if (!install_.active || install_.cancelling)
        return;
    install_.cancelling = true;
    ++install_.serial;
    {
        Lock lock(mutex_);
        cancel_ = true;
    }
    signal_.notify();
}

void SaveStore::clear_install()
{
    if (install_.active)
        return;
    const std::uint64_t serial = install_.serial;
    install_ = {};
    install_.serial = serial + 1;
}

void SaveStore::set_delays(double building, double failed, double job, double older)
{
    Lock lock(mutex_);
    building_delay_ = building;
    failed_delay_ = failed;
    job_delay_ = job;
    older_delay_ = older;
}

void SaveStore::worker()
{
    for (;;)
    {
        {
            Lock lock(mutex_);
            if (stopping_)
                return;
        }
        if (work_once())
            continue;
        Lock lock(mutex_);
        if (stopping_)
            return;
        signal_.wait(mutex_, kIdleWait);
    }
}

bool SaveStore::work_once()
{
    const double now = now_seconds();
    std::string token;
    bool wanted = false;
    bool reload = false;
    bool refresh = false;
    bool start = false;
    std::string save_id;
    std::uint32_t user = 0;
    bool cancel = false;
    double building_delay = 0.0;
    double failed_delay = 0.0;
    double job_delay = 0.0;
    double older_delay = 0.0;
    bool forget = false;
    {
        Lock lock(mutex_);
        token = token_;
        if (token.empty())
            return false; // nothing is asked for before the console session
        forget = std::exchange(forget_, false);
        wanted = now - wanted_at_ < kWantedFor;
        reload = std::exchange(reload_, false);
        refresh = std::exchange(refresh_, false);
        start = std::exchange(start_install_, false);
        save_id = install_id_;
        user = install_user_;
        cancel = std::exchange(cancel_, false);
        building_delay = building_delay_;
        failed_delay = failed_delay_;
        job_delay = job_delay_;
        older_delay = older_delay_;
    }
    client_.set_token(token);
    bool worked = false;
    if (forget)
    {
        // Another session, or the store restarted (perhaps updated): what it
        // said about its saves no longer holds.
        have_list_ = false;
        building_ = false;
        unsupported_ = false;
        have_profiles_ = false;
        next_list_ = 0.0;
    }

    // The install first: the player is waiting on it.
    if (start)
    {
        job_id_.clear();
        post_job(client_.install_save(save_id, user), true);
        worked = true;
    }
    if (cancel && !job_id_.empty())
    {
        // The store refuses to cancel a job that is no longer running (404),
        // as when it ended just before the cancel reached it: the job's own
        // state then says how it ended.
        api::Result<SaveJob> result = client_.cancel_save_job(job_id_);
        const bool refused = !result.ok();
        if (refused)
            result = client_.save_job(job_id_);
        post_job(std::move(result), false, refused);
        worked = true;
    }
    if (!job_id_.empty() && (worked || now >= next_job_))
    {
        if (!start && !cancel)
        {
            post_job(client_.save_job(job_id_), false);
            worked = true;
        }
        next_job_ = now_seconds() + job_delay;
    }

    // The list: read once, again while the store builds or after a failure,
    // and after a while; a 404 means this store has no Save Data Store (it is
    // asked again now and then, as an update adds it).
    const bool due = wanted && now >= next_list_ &&
                     (!have_list_ || building_ || unsupported_ || now - listed_at_ >= kListFresh);
    if (reload || due)
    {
        read_list(refresh);
        const double after = now_seconds();
        next_list_ = after + (unsupported_ ? older_delay
                              : building_  ? building_delay
                              : have_list_ ? kListFresh
                                           : failed_delay);
        worked = true;
    }
    return worked;
}

void SaveStore::read_list(bool refresh)
{
    api::Result<SaveListing> result = client_.save_store(refresh);
    if (result.ok())
    {
        have_list_ = true;
        unsupported_ = false;
        building_ = result.value.building;
        listed_at_ = now_seconds();
    }
    else
    {
        unsupported_ = result.status == 404;
    }
    // The profiles a save installs for, once the store answers.
    api::Result<std::vector<SaveProfile>> profiles;
    const bool read_profiles = result.ok() && !have_profiles_;
    if (read_profiles)
    {
        profiles = client_.save_users();
        have_profiles_ = profiles.ok();
    }
    const bool unsupported = unsupported_;
    inbox_.post(
        [this, result = std::move(result), profiles = std::move(profiles), read_profiles,
         unsupported]() mutable
        {
            if (result.ok())
            {
                state_.load = SavesLoad::ready;
                state_.listing = std::move(result.value);
                state_.error.clear();
            }
            else if (unsupported)
            {
                state_.load = SavesLoad::unsupported;
                state_.listing = {};
                state_.error = result.error;
            }
            else if (state_.load != SavesLoad::ready)
            {
                // A list already shown stays; only a first read fails the screen.
                state_.load = SavesLoad::failed;
                state_.error = result.error;
            }
            if (read_profiles && profiles.ok())
            {
                state_.profiles = std::move(profiles.value);
                state_.have_profiles = true;
            }
            ++state_.serial;
        });
}

// One answer about the install: the job the store started, or its state now.
// cancel_refused: the store did not take a cancel, and this is the job read after it.
void SaveStore::post_job(api::Result<SaveJob> result, bool started, bool cancel_refused)
{
    if (result.ok())
        job_id_ = result.value.finished() ? std::string() : result.value.id;
    else if (started || result.status == 404)
    {
        // Refused, or the store no longer knows the job (it restarted).
        job_id_.clear();
    }
    // A failed read of a running job is asked again at the next step.
    inbox_.post(
        [this, result = std::move(result), started, cancel_refused]() mutable
        {
            if (result.ok())
            {
                install_.have_job = true;
                install_.job = std::move(result.value);
                if (install_.job.finished())
                {
                    install_.active = false;
                    install_.cancelling = false;
                }
            }
            else if (started)
            {
                install_.active = false;
                install_.cancelling = false;
                install_.error = result.error;
            }
            else if (result.status == 404)
            {
                install_.active = false;
                install_.cancelling = false;
                install_.error = tr("XENO STORE lost track of this install (it may have restarted). "
                                    "Install the save again if the game does not have it.");
            }
            // A cancel that did not take (no reply, or the job still runs): Cancel works again.
            if (cancel_refused && install_.active)
                install_.cancelling = false;
            ++install_.serial;
        });
}

} // namespace orbit
