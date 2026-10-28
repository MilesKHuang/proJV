#pragma once
#include <string>
#include <vector>
#include "json.hpp"

// Extract a string value for `key` from a JSON arguments string.
// Uses nlohmann/json for proper parsing.
inline std::string extractStringArg(const std::string& json, const std::string& key) {
    try {
        auto j = nlohmann::json::parse(json);
        if (j.contains(key) && j[key].is_string()) {
            return j[key].get<std::string>();
        }
    } catch (...) {
    }
    return "";
}

// Extract a string value with an optional default.
inline std::string extractStringArg(const std::string& json, const std::string& key,
                                    const std::string& fallback) {
    std::string val = extractStringArg(json, key);
    return val.empty() ? fallback : val;
}

// Extract a boolean value for `key` from a JSON arguments string.
inline bool extractBoolArg(const std::string& json, const std::string& key, bool fallback = false) {
    try {
        auto j = nlohmann::json::parse(json);
        if (j.contains(key) && j[key].is_boolean()) {
            return j[key].get<bool>();
        }
    } catch (...) {
    }
    return fallback;
}

// Extract an array of strings for `key`. A single string is accepted too, so a
// model that sends "path" instead of ["path"] still works.
inline std::vector<std::string> extractStringArrayArg(const std::string& json,
                                                      const std::string& key) {
    std::vector<std::string> out;
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.contains(key)) return out;
        const auto& v = j[key];
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        } else if (v.is_array()) {
            for (const auto& e : v)
                if (e.is_string()) out.push_back(e.get<std::string>());
        }
    } catch (...) {
    }
    return out;
}

// Extract an integer value for `key` from a JSON arguments string.
inline int extractIntArg(const std::string& json, const std::string& key, int fallback = 0) {
    try {
        auto j = nlohmann::json::parse(json);
        if (j.contains(key) && j[key].is_number_integer()) {
            return j[key].get<int>();
        }
    } catch (...) {
    }
    return fallback;
}
