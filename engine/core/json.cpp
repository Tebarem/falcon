#include "core/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fx {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Json parse()
    {
        Json v = value();
        skip();
        if (i_ != s_.size())
            fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what)
    {
        size_t line = 1;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k)
            line += s_[k] == '\n';
        throw std::runtime_error(std::string(what) + " at line " + std::to_string(line));
    }
    void skip()
    {
        while (i_ < s_.size()) {
            if (std::isspace(uint8_t(s_[i_])))
                ++i_;
            else if (s_.substr(i_, 2) == "//")
                while (i_ < s_.size() && s_[i_] != '\n')
                    ++i_;
            else
                break;
        }
    }
    bool eat(char c)
    {
        skip();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    std::string string()
    {
        if (!eat('"'))
            fail("expected a string");
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                const char e = s_[i_++];
                if (e == 'u' && i_ + 4 <= s_.size()) {  // \uXXXX (BMP) -> UTF-8
                    const uint32_t cp = uint32_t(std::strtoul(std::string(s_.substr(i_, 4)).c_str(), nullptr, 16));
                    i_ += 4;
                    if (cp < 0x80) {
                        out += char(cp);
                    } else if (cp < 0x800) {
                        out += char(0xc0 | (cp >> 6));
                        out += char(0x80 | (cp & 0x3f));
                    } else {
                        out += char(0xe0 | (cp >> 12));
                        out += char(0x80 | ((cp >> 6) & 0x3f));
                        out += char(0x80 | (cp & 0x3f));
                    }
                    continue;
                }
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            }
            out += c;
        }
        if (!eat('"'))
            fail("unterminated string");
        return out;
    }
    Json value()
    {
        skip();
        Json v;
        if (i_ >= s_.size())
            fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.type = Json::Type::Object;
            if (!eat('}')) {
                do {
                    std::string key = string();
                    if (!eat(':'))
                        fail("expected ':'");
                    v.object.emplace_back(std::move(key), value());
                } while (eat(','));
                if (!eat('}'))
                    fail("expected '}'");
            }
        } else if (c == '[') {
            ++i_;
            v.type = Json::Type::Array;
            if (!eat(']')) {
                do
                    v.array.push_back(value());
                while (eat(','));
                if (!eat(']'))
                    fail("expected ']'");
            }
        } else if (c == '"') {
            v.type = Json::Type::String;
            v.str = string();
        } else if (s_.substr(i_, 4) == "true" || s_.substr(i_, 5) == "false") {
            v.type = Json::Type::Bool;
            v.b = s_[i_] == 't';
            i_ += v.b ? 4 : 5;
        } else if (s_.substr(i_, 4) == "null") {
            i_ += 4;
        } else {
            const std::string number(s_.substr(i_, std::min<size_t>(64, s_.size() - i_)));
            char* end = nullptr;
            v.type = Json::Type::Number;
            v.number = std::strtod(number.c_str(), &end);
            if (end == number.c_str())
                fail("unexpected character");
            i_ += size_t(end - number.c_str());
        }
        return v;
    }

    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

const Json* Json::get(const char* key) const
{
    for (const auto& [k, v] : object)
        if (k == key)
            return &v;
    return nullptr;
}

std::string Json::getString(const char* key, const char* fallback) const
{
    const Json* v = isObject() ? get(key) : nullptr;
    return v && v->type == Type::String ? v->str : fallback;
}

double Json::getNumber(const char* key, double fallback) const
{
    const Json* v = isObject() ? get(key) : nullptr;
    return v && v->type == Type::Number ? v->number : fallback;
}

bool Json::getBool(const char* key, bool fallback) const
{
    const Json* v = isObject() ? get(key) : nullptr;
    return v && v->type == Type::Bool ? v->b : fallback;
}

Json Json::parse(std::string_view text) { return Parser(text).parse(); }

Json Json::makeNumber(double v)
{
    Json j;
    j.type = Type::Number;
    j.number = v;
    return j;
}

Json Json::makeBool(bool v)
{
    Json j;
    j.type = Type::Bool;
    j.b = v;
    return j;
}

Json Json::makeString(std::string v)
{
    Json j;
    j.type = Type::String;
    j.str = std::move(v);
    return j;
}

Json Json::makeObject()
{
    Json j;
    j.type = Type::Object;
    return j;
}

Json& Json::set(const std::string& key, Json value)
{
    type = Type::Object;
    for (auto& [k, v] : object)
        if (k == key)
            return v = std::move(value);
    object.emplace_back(key, std::move(value));
    return object.back().second;
}

Json& Json::child(const std::string& key)
{
    type = Type::Object;
    for (auto& [k, v] : object)
        if (k == key) {
            if (!v.isObject())
                v = makeObject();
            return v;
        }
    return set(key, makeObject());
}

namespace {

void dumpString(std::string& out, std::string_view s)
{
    JsonWriter w;
    w.value(nullptr, s);
    out += w.str();
}

void dumpValue(std::string& out, const Json& j, int indent, int depth)
{
    const auto newline = [&](int d) {
        if (indent > 0) {
            out += '\n';
            out.append(size_t(indent * d), ' ');
        }
    };
    switch (j.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.b ? "true" : "false"; break;
    case Json::Type::Number: {
        JsonWriter w;
        w.value(nullptr, j.number);
        out += w.str();
        break;
    }
    case Json::Type::String: dumpString(out, j.str); break;
    case Json::Type::Array:
        out += '[';
        for (size_t i = 0; i < j.array.size(); ++i) {
            out += i ? ", " : "";
            dumpValue(out, j.array[i], 0, depth + 1);
        }
        out += ']';
        break;
    case Json::Type::Object:
        out += '{';
        for (size_t i = 0; i < j.object.size(); ++i) {
            out += i ? "," : "";
            newline(depth + 1);
            dumpString(out, j.object[i].first);
            out += ": ";
            dumpValue(out, j.object[i].second, indent, depth + 1);
        }
        if (!j.object.empty())
            newline(depth);
        out += '}';
        break;
    }
}

}  // namespace

std::string Json::dump(int indent) const
{
    std::string out;
    dumpValue(out, *this, indent, 0);
    return out;
}

// ---- writer

void JsonWriter::separator(const char* key)
{
    if (!first_.back())
        out_ += ',';
    first_.back() = false;
    if (key) {
        string(key);
        out_ += ':';
    }
}

void JsonWriter::string(std::string_view s)
{
    out_ += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
                out_ += buf;
            } else {
                out_ += c;
            }
        }
    }
    out_ += '"';
}

JsonWriter& JsonWriter::beginObject(const char* key)
{
    separator(key);
    out_ += '{';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endObject()
{
    first_.pop_back();
    out_ += '}';
    return *this;
}

JsonWriter& JsonWriter::beginArray(const char* key)
{
    separator(key);
    out_ += '[';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray()
{
    first_.pop_back();
    out_ += ']';
    return *this;
}

JsonWriter& JsonWriter::raw(const char* key, std::string_view json)
{
    separator(key);
    out_ += json;
    return *this;
}

JsonWriter& JsonWriter::value(const char* key, std::string_view s)
{
    separator(key);
    string(s);
    return *this;
}

JsonWriter& JsonWriter::value(const char* key, double v)
{
    separator(key);
    if (!std::isfinite(v)) {
        out_ += "null";
        return *this;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    out_ += buf;
    return *this;
}

JsonWriter& JsonWriter::value(const char* key, bool v)
{
    separator(key);
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(const char* key, uint64_t v)
{
    separator(key);
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(const char* key, int64_t v)
{
    separator(key);
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::null(const char* key)
{
    separator(key);
    out_ += "null";
    return *this;
}

}  // namespace fx
