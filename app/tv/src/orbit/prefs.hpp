// XENO STORE TV app - The app's own settings, kept on this console.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings that belong to this app rather than to the store, whose service
// keeps the rest: a small JSON file in the title's writable storage, as
// Orbit Store 1.3.5's TV app keeps its Appearance choice. They never reach
// XENO STORE.

#pragma once

#include <string>

namespace orbit
{

// Native titles cannot write the store's /data directory; /download0 is the
// title's persistent writable mount (its runtime log is there too).
inline constexpr const char *kPrefsPath = "/download0/xeno-store/settings.json";

class Prefs
{
  public:
    // Reads the saved settings; an empty path keeps them in memory only
    // (previews, tests). A missing or unreadable file gives the defaults.
    void load(std::string path);
    // The interface's sounds: navigation, selection and the like (on by default).
    bool sounds() const
    {
        return sounds_;
    }
    // Changes the setting and saves the file. A failed save keeps the choice
    // for this session and returns why ("" once saved, or in memory only).
    std::string set_sounds(bool on);

  private:
    std::string save() const;

    std::string path_;
    bool sounds_ = true;
};

} // namespace orbit
