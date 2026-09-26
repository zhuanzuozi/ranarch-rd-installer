// SPDX-License-Identifier: MIT
// RanArch RD Installer — minimal vendored JSON value type + parser + serializer.
//
// Avoids an nlohmann-json dependency. Supports null/bool/int/double/string/
// array/object — enough for the IPC protocol. Not a general-purpose JSON
// library; optimized for correctness and small size, not speed.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ranarch {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    // ---- Constructors / factories ----
    Json() : m_type(Type::Null) {}
    static Json make_null()                  { Json j; return j; }
    static Json make_bool(bool b)            { Json j; j.m_type = Type::Bool; j.m_bool = b; return j; }
    static Json make_int(int64_t i)          { Json j; j.m_type = Type::Int;  j.m_int  = i; return j; }
    static Json make_string(std::string s)   { Json j; j.m_type = Type::String; j.m_string = std::move(s); return j; }
    static Json make_array()                 { Json j; j.m_type = Type::Array;  return j; }
    static Json make_object()                { Json j; j.m_type = Type::Object; return j; }

    // ---- Type queries ----
    Type type() const { return m_type; }
    bool is_null()   const { return m_type == Type::Null; }
    bool is_object() const { return m_type == Type::Object; }
    bool is_array()  const { return m_type == Type::Array; }
    bool is_string() const { return m_type == Type::String; }

    // ---- Accessors ----
    bool          as_bool()   const { return m_bool; }
    int64_t       as_int()    const { return m_int; }
    const std::string& as_string() const { return m_string; }
    std::vector<Json>&       as_array()  { return m_array; }
    std::map<std::string, Json>& as_object() { return m_object; }
    const std::vector<Json>&       as_array()  const { return m_array; }
    const std::map<std::string, Json>& as_object() const { return m_object; }

    // ---- Mutators ----
    void append(Json v) { m_type = Type::Array; m_array.push_back(std::move(v)); }
    void set(const std::string& key, Json v) {
        m_type = Type::Object; m_object[key] = std::move(v);
    }

    // ---- Lookup (object) ----
    const Json* find(const std::string& key) const {
        if (m_type != Type::Object) return nullptr;
        auto it = m_object.find(key);
        return it == m_object.end() ? nullptr : &it->second;
    }
    std::string get_string(const std::string& key, const std::string& def = "") const {
        const Json* p = find(key);
        return (p && p->is_string()) ? p->as_string() : def;
    }
    int64_t get_int(const std::string& key, int64_t def = 0) const {
        const Json* p = find(key);
        return (p && p->m_type == Type::Int) ? p->m_int : def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        const Json* p = find(key);
        return (p && p->m_type == Type::Bool) ? p->m_bool : def;
    }

    // ---- Serialization ----
    std::string dump() const;

    // ---- Parsing ----
    static Json parse(const std::string& s);

private:
    Type   m_type = Type::Null;
    bool   m_bool = false;
    int64_t m_int = 0;
    double m_double = 0.0;
    std::string m_string;
    std::vector<Json> m_array;
    std::map<std::string, Json> m_object;
};

} // namespace ranarch
