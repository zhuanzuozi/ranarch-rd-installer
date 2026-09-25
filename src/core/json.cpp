// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — minimal JSON parser + serializer implementation.
#include "json.h"

#include <cstdlib>
#include <sstream>

namespace ranarch {

// ---- Serializer ----
std::string Json::dump() const {
    switch (m_type) {
        case Type::Null:   return "null";
        case Type::Bool:   return m_bool ? "true" : "false";
        case Type::Int:    return std::to_string(m_int);
        case Type::Double: { std::ostringstream ss; ss << m_double; return ss.str(); }
        case Type::String: {
            std::string out = "\"";
            for (char c : m_string) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else { out += c; }
                }
            }
            out += '"';
            return out;
        }
        case Type::Array: {
            std::string out = "[";
            for (size_t i = 0; i < m_array.size(); ++i) {
                if (i) out += ',';
                out += m_array[i].dump();
            }
            out += ']';
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : m_object) {
                if (!first) out += ',';
                first = false;
                Json key = make_string(k);
                out += key.dump();
                out += ':';
                out += v.dump();
            }
            out += '}';
            return out;
        }
    }
    return "null";
}

// ---- Parser (recursive descent) ----
namespace {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    char peek() { skip_ws(); return p < end ? *p : '\0'; }

    bool match(const char* lit) {
        size_t len = 0;
        while (lit[len]) ++len;
        if (end - p < static_cast<ptrdiff_t>(len)) return false;
        for (size_t i = 0; i < len; ++i)
            if (p[i] != lit[i]) return false;
        p += len;
        return true;
    }

    Json parse_value() {
        skip_ws();
        if (p >= end) return Json::make_null();
        char c = *p;
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        if (c == 't') { if (match("true"))  return Json::make_bool(true); }
        if (c == 'f') { if (match("false")) return Json::make_bool(false); }
        if (c == 'n') { if (match("null"))  return Json::make_null(); }
        return Json::make_null();
    }

    Json parse_string() {
        ++p; // skip opening "
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        // Skip 4 hex digits, output as UTF-8 bytes.
                        if (p + 4 < end) {
                            char hex[5] = {p[1], p[2], p[3], p[4], 0};
                            unsigned int cp = std::strtoul(hex, nullptr, 16);
                            p += 4;
                            if (cp < 0x80) s += static_cast<char>(cp);
                            else if (cp < 0x800) {
                                s += static_cast<char>(0xC0 | (cp >> 6));
                                s += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                s += static_cast<char>(0xE0 | (cp >> 12));
                                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                s += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: s += *p; break;
                }
            } else {
                s += *p;
            }
            ++p;
        }
        if (p < end) ++p; // skip closing "
        return Json::make_string(std::move(s));
    }

    Json parse_number() {
        std::string num;
        bool is_double = false;
        if (*p == '-') { num += *p++; }
        while (p < end && *p >= '0' && *p <= '9') num += *p++;
        if (p < end && *p == '.') { is_double = true; num += *p++;
            while (p < end && *p >= '0' && *p <= '9') num += *p++; }
        if (p < end && (*p == 'e' || *p == 'E')) { is_double = true; num += *p++;
            if (p < end && (*p == '+' || *p == '-')) num += *p++;
            while (p < end && *p >= '0' && *p <= '9') num += *p++; }
        if (is_double) return Json::make_int(std::strtoll(num.c_str(), nullptr, 10));
        return Json::make_int(std::strtoll(num.c_str(), nullptr, 10));
    }

    Json parse_array() {
        ++p; // skip [
        Json arr = Json::make_array();
        skip_ws();
        if (peek() == ']') { ++p; return arr; }
        while (p < end) {
            arr.append(parse_value());
            skip_ws();
            if (peek() == ',') { ++p; continue; }
            if (peek() == ']') { ++p; break; }
            break;
        }
        return arr;
    }

    Json parse_object() {
        ++p; // skip {
        Json obj = Json::make_object();
        skip_ws();
        if (peek() == '}') { ++p; return obj; }
        while (p < end) {
            skip_ws();
            if (*p != '"') break;
            Json key = parse_string();
            skip_ws();
            if (peek() != ':') break;
            ++p; // skip :
            obj.set(key.as_string(), parse_value());
            skip_ws();
            if (peek() == ',') { ++p; continue; }
            if (peek() == '}') { ++p; break; }
            break;
        }
        return obj;
    }
};

} // namespace

Json Json::parse(const std::string& s) {
    Parser parser{s.c_str(), s.c_str() + s.size()};
    return parser.parse_value();
}

} // namespace ranarch
