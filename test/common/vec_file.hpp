// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/hex.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace knx_test::vec {

inline bool readTextFile(const std::string& path, std::string& out)
{
    const std::filesystem::path requested(path);

    const auto tryRead = [&](const std::filesystem::path& candidate) -> bool {
        std::ifstream f(candidate);
        if (!f.is_open()) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    };

    if (requested.is_absolute()) {
        return tryRead(requested);
    }

    if (tryRead(requested)) return true;

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) return false;

    return tryRead(cwd / requested)
        || tryRead(cwd.parent_path() / requested)
        || tryRead(cwd.parent_path().parent_path() / requested);
}

inline std::string trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

inline bool parseVec(const std::string& text, std::map<std::string, std::string>& kv)
{
    kv.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty()) continue;
        if (line[0] == '#') continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if (k.empty()) return false;
        kv[k] = v;
    }
    return true;
}

inline bool getHex(const std::map<std::string, std::string>& kv,
                   const std::string& key,
                   std::vector<uint8_t>& out)
{
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    // Use parseHex with vector as backing span
    out.clear();
    out.resize(it->second.size() / 2 + 1);
    auto res = knx::util::parseHex(it->second, std::span<uint8_t>{out});
    if (!res.isOk()) return false;
    out.resize(res.value());
    return true;
}

inline bool getU64(const std::map<std::string, std::string>& kv, const std::string& key, uint64_t& out)
{
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    const std::string v = trim(it->second);
    if (v.empty()) return false;
    try {
        out = static_cast<uint64_t>(std::stoull(v, nullptr, 10));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace knx_test::vec
