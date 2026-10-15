// Orbit Store TV app - Starting Orbit when it isn't running.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
//
// The app is a screen for Orbit, which runs as a payload so downloads carry on
// after the app is suspended or closed. When nothing answers on Orbit's port,
// the app hands a copy of Orbit to the ELF loader the console's jailbreak runs
// on 127.0.0.1:9021, as a payload sender would. It sends the copy Orbit saved
// in /data/xeno-store (which its updater keeps current) unless the copy in
// this package is a newer release, or the saved one can't be read.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace orbit::starter
{

// Release versions as Orbit numbers them: "0.4.2", "v0.5.0-beta.1".
// Returns -1, 0 or 1; anything else sorts below every real version.
int compare_versions(std::string_view a, std::string_view b);

// XENO STORE: the copy its updater saves (update_apply writes the ELF, then a
// .version beside it) and the copy bundled in PPSA99500.
struct Locations
{
    std::string saved_elf = "/data/xeno-store/xeno-store.elf";
    std::string saved_version = "/data/xeno-store/xeno-store.elf.version";
    std::string bundled_elf = "/app0/xeno/xeno-store.elf";
    std::string bundled_version = "/app0/xeno/version.txt";
    std::uint16_t loader_port = 9021; // the jailbreak's ELF loader on 127.0.0.1
};

struct Payload
{
    std::string path; // empty when there is nothing to start
    std::string version;
    const char *source = ""; // "saved" or "bundled"
    long long bytes = 0;
    bool saved_avoided = false; // the bundled copy stands in for a saved one (avoid_saved)
};

// The copy to start: a readable x86-64 ELF of at most 64 MiB. With avoid_saved,
// the bundled copy is chosen whenever it is readable.
Payload choose(const Locations &where, bool avoid_saved = false);

enum class Outcome : std::uint8_t
{
    sent,       // the loader took the whole file
    no_loader,  // nothing listens on the loader's port
    no_payload, // neither copy of Orbit can be read
    failed,     // the transfer broke off
};

// Streams the file to the loader on 127.0.0.1:port.
Outcome send(const std::string &path, std::uint16_t port, int timeout_ms, std::string *error);

struct Result
{
    Outcome outcome = Outcome::failed;
    std::string detail; // for the screen and the log
    std::string version; // the selected copy; a successful send is not proof it started
    const char *source = ""; // the selected copy: "saved", "bundled" or "" for none
};

// What the store calls to start Orbit; tests substitute their own.
class Starter
{
  public:
    virtual ~Starter() = default;
    // Blocks while the payload is sent; runs on the store's worker.
    virtual Result start() = 0;

    // Set by the store when the saved copy was sent and never answered (an
    // update that crashes at start): later starts send the bundled copy, whose
    // start sets the saved one aside. Cleared once the backend answers.
    void set_avoid_saved(bool avoid)
    {
        avoid_saved_ = avoid;
    }
    bool avoid_saved() const
    {
        return avoid_saved_;
    }

  protected:
    bool avoid_saved_ = false;
};

class LoaderStarter final : public Starter
{
  public:
    explicit LoaderStarter(Locations where, std::function<void(const std::string &)> log = {});
    // The loader's port named apart from where; it replaces where.loader_port.
    LoaderStarter(Locations where, std::uint16_t port,
                  std::function<void(const std::string &)> log = {});
    Result start() override;

  private:
    Locations where_;
    std::function<void(const std::string &)> log_;
};

} // namespace orbit::starter
