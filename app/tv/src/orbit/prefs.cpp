// XENO STORE TV app - The app's own settings, kept on this console.
// Copyright (C) 2026 XENO STORE contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/prefs.hpp"

#include "core/save_file.hpp"
#include "orbit/json.hpp"

#include <utility>

namespace orbit
{

void Prefs::load(std::string path)
{
    path_ = std::move(path);
    sounds_ = true;
    std::string text;
    if (path_.empty() || !hui::save::read_file(path_, &text, 16u << 10))
        return;
    json::Value doc;
    if (!json::parse(text, &doc) || !doc.is_object())
        return;
    sounds_ = doc.flag("sounds", true);
}

std::string Prefs::set_sounds(bool on)
{
    sounds_ = on;
    return save();
}

std::string Prefs::save() const
{
    if (path_.empty())
        return {};
    const std::size_t slash = path_.rfind('/');
    if (slash != std::string::npos && slash > 0)
        (void)hui::save::ensure_directory(path_.substr(0, slash));
    return hui::save::write_atomic(path_, std::string("{\"sounds\":") +
                                              (sounds_ ? "true" : "false") + "}\n");
}

} // namespace orbit
