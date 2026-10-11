#pragma once
// Type-checked JSON access. The WASM build compiles nlohmann/json with JSON_NOEXCEPTION, where a type mismatch
// aborts the module, so code that reads untrusted documents (imports, panel messages) must go through these helpers
// instead of value()/at()/get<T>() on unchecked nodes.

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>

namespace fscs::js {

using json = nlohmann::json;

inline const json& null() {
    static const json n = nullptr;
    return n;
}

inline const json& get(const json& o, const char* key) {
    if (!o.is_object()) return null();
    auto it = o.find(key);
    return it == o.end() ? null() : *it;
}

inline bool has(const json& o, const char* key) { return o.is_object() && o.find(key) != o.end(); }

inline std::string str(const json& v, std::string fallback = {}) { return v.is_string() ? v.get<std::string>() : fallback; }
inline std::string str(const json& o, const char* key, std::string fallback = {}) { return str(get(o, key), std::move(fallback)); }

inline double num(const json& v, double fallback) {
    if (!v.is_number()) return fallback;
    const double d = v.get<double>();
    return std::isfinite(d) ? d : fallback;
}
inline double num(const json& o, const char* key, double fallback) { return num(get(o, key), fallback); }

inline std::int64_t integer(const json& v, std::int64_t fallback) {
    if (v.is_number_integer()) return v.get<std::int64_t>();
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::isfinite(d) && std::floor(d) == d && std::abs(d) < 9.0e15) return static_cast<std::int64_t>(d);
    }
    return fallback;
}
inline std::int64_t integer(const json& o, const char* key, std::int64_t fallback) { return integer(get(o, key), fallback); }

inline bool boolean(const json& v, bool fallback) { return v.is_boolean() ? v.get<bool>() : fallback; }
inline bool boolean(const json& o, const char* key, bool fallback) { return boolean(get(o, key), fallback); }

inline const json& object(const json& o, const char* key) {
    static const json empty = json::object();
    const json& v = get(o, key);
    return v.is_object() ? v : empty;
}

inline const json& array(const json& o, const char* key) {
    static const json empty = json::array();
    const json& v = get(o, key);
    return v.is_array() ? v : empty;
}

// Parses without exceptions; returns a discarded value on error.
inline json parse(const std::string& text) { return json::parse(text, nullptr, false); }

inline std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

bool validUtf8(const std::string& s);
std::uint32_t crc32(const std::string& bytes);

}  // namespace fscs::js
