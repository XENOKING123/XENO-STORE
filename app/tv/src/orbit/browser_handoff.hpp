// Orbit Store - local native-to-browser navigation, without starting a transfer.
// SPDX-License-Identifier: GPL-3.0-or-later
// Modified for XENO STORE in 2026 (port: backend dialect, branding, paths). See app/tv/NOTICE.
#pragma once
#include <string>
#include <string_view>

namespace orbit
{
struct BrowserSelection
{
    std::string game_id;
    std::string release_id;
    std::string storage_id;
};
// XENO STORE: TorBox is set up in the browser version's Settings (Debrid),
// which its router opens at #settings. The selection stays on this page, so
// nothing from it is put into the address.
inline std::string browser_handoff_url(const BrowserSelection &selection)
{
    (void)selection;
    return "http://127.0.0.1:9095/?tv=1#settings";
}
// This game's page in the browser version (its router reads #game/<id>), for
// options the console cannot fetch by itself. The ID is percent-encoded, so
// any catalogue ID is safe in the address; an empty ID gives no address.
inline std::string browser_game_url(std::string_view game_id)
{
    if (game_id.empty())
        return {};
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string url = "http://127.0.0.1:9095/?tv=1#game/";
    for (const char c : game_id)
    {
        const auto byte = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            url += c;
        }
        else
        {
            url += '%';
            url += kHex[byte >> 4];
            url += kHex[byte & 0xF];
        }
    }
    return url;
}
} // namespace orbit
