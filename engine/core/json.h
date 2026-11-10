#pragma once

// Minimal JSON: a DOM parser (objects keep insertion order; `//` line comments allowed) and a
// streaming writer. Used by the cooker (animation graphs) and the UI message bridge.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fx {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double number = 0.0;
    std::string str;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* get(const char* key) const;
    bool isObject() const { return type == Type::Object; }
    // Typed lookups with fallbacks (objects only).
    std::string getString(const char* key, const char* fallback = "") const;
    double getNumber(const char* key, double fallback = 0.0) const;
    bool getBool(const char* key, bool fallback = false) const;

    // Throws std::runtime_error with a line number on malformed input.
    static Json parse(std::string_view text);

    // Building and editing (preference files).
    static Json makeNumber(double v);
    static Json makeBool(bool v);
    static Json makeString(std::string v);
    static Json makeObject();
    Json& set(const std::string& key, Json value);  // insert or replace (becomes an object)
    Json& child(const std::string& key);            // existing or new empty object
    std::string dump(int indent = 2) const;         // pretty-printed JSON text
};

// Builds compact JSON text. Keys and values are appended in order; commas are automatic.
class JsonWriter {
public:
    JsonWriter& beginObject(const char* key = nullptr);
    JsonWriter& endObject();
    JsonWriter& beginArray(const char* key = nullptr);
    JsonWriter& endArray();
    JsonWriter& value(const char* key, std::string_view s);
    JsonWriter& value(const char* key, const char* s) { return value(key, std::string_view(s)); }
    JsonWriter& value(const char* key, double v);
    JsonWriter& value(const char* key, bool v);
    JsonWriter& value(const char* key, uint64_t v);
    JsonWriter& value(const char* key, int64_t v);
    JsonWriter& value(const char* key, uint32_t v) { return value(key, uint64_t(v)); }
    JsonWriter& value(const char* key, int32_t v) { return value(key, int64_t(v)); }
    // The integer types the fixed-width ones aren't here (unsigned long long on Linux, unsigned
    // long and size_t on macOS, long on Windows).
    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) >= 4 && !std::is_same_v<T, uint64_t> &&
                 !std::is_same_v<T, int64_t> && !std::is_same_v<T, uint32_t> && !std::is_same_v<T, int32_t>)
    JsonWriter& value(const char* key, T v)
    {
        if constexpr (std::is_signed_v<T>)
            return value(key, int64_t(v));
        else
            return value(key, uint64_t(v));
    }
    JsonWriter& null(const char* key);
    JsonWriter& raw(const char* key, std::string_view json);  // already JSON text
    const std::string& str() const { return out_; }

private:
    void separator(const char* key);
    void string(std::string_view s);
    std::string out_;
    std::vector<bool> first_ = {true};
};

}  // namespace fx
