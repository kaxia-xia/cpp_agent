// json.hpp - minimal self-contained JSON value, parser and serializer.
// No external dependencies; C++20.
#pragma once
 
#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
 
namespace json {
 
class Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array  = std::vector<Value>;
 
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
 
private:
    Type type_{Type::Null};
    std::variant<std::monostate, bool, double, std::string, Array, Object> data_{};
 
public:
    Value() = default;
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), data_(b) {}
    Value(int v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    Value(long v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    Value(long long v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    Value(unsigned v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    Value(double v) : type_(Type::Number), data_(v) {}
    Value(const char* s) : type_(Type::String), data_(std::string(s)) {}
    Value(std::string s) : type_(Type::String), data_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), data_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), data_(std::move(o)) {}
 
    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }
 
    [[nodiscard]] bool as_bool() const { return std::get<bool>(data_); }
    [[nodiscard]] double as_number() const { return std::get<double>(data_); }
    [[nodiscard]] long long as_integer() const { return static_cast<long long>(std::get<double>(data_)); }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data_); }
    [[nodiscard]] const Array& as_array() const { return std::get<Array>(data_); }
    [[nodiscard]] const Object& as_object() const { return std::get<Object>(data_); }
 
    // Convenience: as_integer but tolerant of string-encoded numbers.
    [[nodiscard]] long long to_integer() const {
        if (is_number()) return as_integer();
        if (is_string()) {
            long long v{};
            std::from_chars(as_string().data(), as_string().data() + as_string().size(), v);
            return v;
        }
        return 0;
    }
 
    // Object access helpers.
    const Value& operator[](std::string_view key) const {
        static const Value null_value{};
        if (!is_object()) return null_value;
        const Object& o = as_object();
        if (auto it = o.find(key); it != o.end()) return it->second;
        return null_value;
    }
    [[nodiscard]] bool contains(std::string_view key) const {
        if (!is_object()) return false;
        return as_object().find(key) != as_object().end();
    }
 
    [[nodiscard]] std::string serialize(bool pretty = false, int indent = 2) const {
        std::string out;
        write(out, *this, pretty ? indent : -1, 0);
        return out;
    }
 
private:
    static void write_str(std::string& out, std::string_view s) {
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<int>(c));
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out.push_back('"');
    }
 
    static void write(std::string& out, const Value& v, int indent, int depth) {
        const auto pad = [&](int d) {
            if (indent < 0) return;
            for (int i = 0; i < indent * d; ++i) out.push_back(' ');
        };
        const auto nl = [&]() { if (indent >= 0) out.push_back('\n'); };
 
        switch (v.type_) {
            case Type::Null:   out += "null"; return;
            case Type::Bool:   out += (v.as_bool() ? "true" : "false"); return;
            case Type::Number: {
                double d = v.as_number();
                if (std::isfinite(d)) {
                    // Render integers without trailing decimals when safe.
                    if (d == std::floor(d) && std::abs(d) < 1e15) {
                        out += std::format("{}", static_cast<long long>(d));
                    } else {
                        out += std::format("{}", d);
                    }
                } else {
                    out += "null";
                }
                return;
            }
            case Type::String: write_str(out, v.as_string()); return;
            case Type::Array: {
                const Array& a = v.as_array();
                if (a.empty()) { out += "[]"; return; }
                out.push_back('['); nl();
                for (size_t i = 0; i < a.size(); ++i) {
                    pad(depth + 1);
                    write(out, a[i], indent, depth + 1);
                    if (i + 1 < a.size()) out.push_back(',');
                    nl();
                }
                pad(depth);
                out.push_back(']');
                return;
            }
            case Type::Object: {
                const Object& o = v.as_object();
                if (o.empty()) { out += "{}"; return; }
                out.push_back('{'); nl();
                size_t i = 0;
                for (const auto& [k, val] : o) {
                    pad(depth + 1);
                    write_str(out, k);
                    out.push_back(':');
                    if (indent >= 0) out.push_back(' ');
                    write(out, val, indent, depth + 1);
                    if (++i < o.size()) out.push_back(',');
                    nl();
                }
                pad(depth);
                out.push_back('}');
                return;
            }
        }
    }
};
 
// ---- Parser (recursive descent) ----
class ParseError : public std::runtime_error {
public:
    explicit ParseError(std::string msg) : std::runtime_error(msg) {}
};
 
class Parser {
    std::string_view src_;
    size_t pos_{0};
 
public:
    explicit Parser(std::string_view s) : src_(s) {}
 
    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != src_.size())
            throw ParseError(std::format("trailing data at offset {}", pos_));
        return v;
    }
 
private:
    [[noreturn]] void fail(std::string_view msg) const {
        throw ParseError(std::format("json parse error at {}: {}", pos_, msg));
        __builtin_unreachable();
    }
 
    char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char get() { return pos_ < src_.size() ? src_[pos_++] : '\0'; }
 
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }
 
    Value parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
            case 'n': return parse_literal("null", Value{nullptr});
            case 't': return parse_literal("true", Value{true});
            case 'f': return parse_literal("false", Value{false});
            case '"': return Value{parse_string()};
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
                fail("unexpected character");
        }
    }
 
    Value parse_literal(std::string_view lit, Value v) {
        if (src_.substr(pos_, lit.size()) != lit) fail("invalid literal");
        pos_ += lit.size();
        return v;
    }
 
    Value parse_number() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.' ||
               src_[pos_] == 'e' || src_[pos_] == 'E' || src_[pos_] == '+' || src_[pos_] == '-'))
            ++pos_;
        std::string num(src_.substr(start, pos_ - start));
        try {
            size_t idx;
            double d = std::stod(num, &idx);
            if (idx != num.size()) fail("invalid number");
            return Value{d};
        } catch (...) { fail("invalid number"); }
    }
 
    std::string parse_string() {
        if (get() != '"') fail("expected string");
        std::string out;
        while (true) {
            if (pos_ >= src_.size()) fail("unterminated string");
            char c = src_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= src_.size()) fail("bad escape");
                char e = src_[pos_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) fail("bad unicode escape");
                        unsigned cp = 0;
                        auto hex = [](char ch) -> unsigned {
                            if (ch >= '0' && ch <= '9') return ch - '0';
                            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                            return 0;
                        };
                        for (int i = 0; i < 4; ++i) cp = (cp << 4) | hex(src_[pos_++]);
                        encode_utf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }
 
    static void encode_utf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) { out.push_back(static_cast<char>(cp)); }
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
 
    Value parse_array() {
        if (get() != '[') fail("expected '['");
        Array a;
        skip_ws();
        if (peek() == ']') { ++pos_; return Value{std::move(a)}; }
        while (true) {
            a.push_back(parse_value());
            skip_ws();
            char c = get();
            if (c == ',') { skip_ws(); continue; }
            if (c == ']') break;
            fail("expected ',' or ']'");
        }
        return Value{std::move(a)};
    }
 
    Value parse_object() {
        if (get() != '{') fail("expected '{'");
        Object o;
        skip_ws();
        if (peek() == '}') { ++pos_; return Value{std::move(o)}; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (get() != ':') fail("expected ':'");
            o.emplace(std::move(key), parse_value());
            skip_ws();
            char c = get();
            if (c == ',') { continue; }
            if (c == '}') break;
            fail("expected ',' or '}'");
        }
        return Value{std::move(o)};
    }
};
 
inline Value parse(std::string_view s) { return Parser{s}.parse(); }
 
// ---- Builder helpers ----
template <std::convertible_to<Value> T>
Value make_array(std::initializer_list<T> items) {
    Array a;
    a.reserve(items.size());
    for (const auto& it : items) a.emplace_back(it);
    return Value{std::move(a)};
}
 
} // namespace json
