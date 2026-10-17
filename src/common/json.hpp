#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace ast {

// Minimal JSON document model + parser, dependency-free, used for config.
// Supports null, bool, number (double + integer), string, array, object.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(int64_t n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(std::vector<Json> a) : type_(Type::Array), arr_(std::move(a)) {}
    Json(std::map<std::string, Json> o) : type_(Type::Object), obj_(std::move(o)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return bool_; }
    double as_number() const { return num_; }
    int64_t as_int() const { return static_cast<int64_t>(num_); }
    const std::string& as_string() const { return str_; }
    const std::vector<Json>& as_array() const { return arr_; }
    const std::map<std::string, Json>& as_object() const { return obj_; }

    const Json* find(std::string_view key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = obj_.find(std::string(key));
        return it == obj_.end() ? nullptr : &it->second;
    }

    // Typed accessors with defaults.
    std::optional<std::string> get_string(std::string_view key) const {
        const Json* v = find(key);
        if (v && v->is_string()) return v->as_string();
        return std::nullopt;
    }
    std::optional<int64_t> get_int(std::string_view key) const {
        const Json* v = find(key);
        if (v && v->is_number()) return v->as_int();
        return std::nullopt;
    }
    std::optional<double> get_double(std::string_view key) const {
        const Json* v = find(key);
        if (v && v->is_number()) return v->as_number();
        return std::nullopt;
    }
    std::optional<bool> get_bool(std::string_view key) const {
        const Json* v = find(key);
        if (v && v->is_bool()) return v->as_bool();
        return std::nullopt;
    }
    const Json* get(std::string_view key) const { return find(key); }

    void set(std::string key, Json v) { obj_[std::move(key)] = std::move(v); type_ = Type::Object; }

    // Serialize to compact JSON (for diagnostics / test data).
    std::string dump() const;

private:
    Type type_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

// Parse a JSON document. Returns error on malformed input.
Result<Json> parse_json(std::string_view text);

} // namespace ast
