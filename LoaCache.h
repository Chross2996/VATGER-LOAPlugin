#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include "LOAPlugin.h"

using LOAMatchTimestamp = std::chrono::time_point<std::chrono::steady_clock>;

// ✅ C++14-compatible struct
struct CachedLOAResult {
    CachedLOAResult() : match(nullptr), source() {}
    CachedLOAResult(const LOAEntry* m, const std::string& s) : match(m), source(s) {}

    const LOAEntry* match;
    std::string source;
};

// ✅ Ensure this matches exactly where used in LOAPlugin.cpp
extern std::unordered_map<std::string, std::pair<CachedLOAResult, LOAMatchTimestamp>> matchCache;