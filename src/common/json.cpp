#include "common/json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace ast {

namespace {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (std::isspace((unsigned char)*p))) ++p;
    }

    bool consume(char c) {
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }

    Error err(std::string m) { return Error(Err::InvalidArgument, std::move(m)); }

    Result<Json> parse_value() {
        skip_ws();
        if (p >= end) return err("unexpected end of input");
        char c = *p;
        switch (c) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': {
            auto s = parse_string();
            if (!s.ok()) return s.error();
            return Json(s.value());
        }
        case 't': return parse_literal("true", Json(true));
        case 'f': return parse_literal("false", Json(false));
        case 'n': return parse_literal("null", Json(nullptr));
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            return err("unexpected character");
        }
    }

    Result<Json> parse_literal(const char* lit, Json val) {
        for (const char* q = lit; *q; ++q) {
            if (p >= end || *p != *q) return err("invalid literal");
            ++p;
        }
        return val;
    }

    Result<std::string> parse_string() {
        if (!consume('"')) return err("expected '\"'");
        std::string out;
        while (p < end) {
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= end) return err("bad escape");
                char e = *p++;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (p + 4 > end) return err("bad \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = *p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                        else return err("bad hex in \\u");
                    }
                    // Encode as UTF-8.
                    if (cp < 0x80) out.push_back((char)cp);
                    else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return err("bad escape char");
                }
            } else if ((unsigned char)c < 0x20) {
                return err("control char in string");
            } else {
                out.push_back(c);
            }
        }
        return err("unterminated string");
    }

    Result<Json> parse_number() {
        const char* start = p;
        if (consume('-')) {}
        if (p < end && *p == '0') { ++p; }
        else {
            if (p >= end || !std::isdigit((unsigned char)*p)) return err("bad number");
            while (p < end && std::isdigit((unsigned char)*p)) ++p;
        }
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || !std::isdigit((unsigned char)*p)) return err("bad number");
            while (p < end && std::isdigit((unsigned char)*p)) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || !std::isdigit((unsigned char)*p)) return err("bad number");
            while (p < end && std::isdigit((unsigned char)*p)) ++p;
        }
        std::string num(start, p);
        char* e = nullptr;
        double d = std::strtod(num.c_str(), &e);
        if (e == num.c_str()) return err("bad number");
        return Json(d);
    }

    Result<Json> parse_object() {
        if (!consume('{')) return err("expected '{'");
        std::map<std::string, Json> obj;
        skip_ws();
        if (consume('}')) return Json(std::move(obj));
        while (true) {
            skip_ws();
            auto key = parse_string();
            if (!key.ok()) return key.error();
            skip_ws();
            if (!consume(':')) return err("expected ':'");
            auto val = parse_value();
            if (!val.ok()) return val.error();
            obj[key.value()] = val.value();
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            return err("expected ',' or '}'");
        }
        return Json(std::move(obj));
    }

    Result<Json> parse_array() {
        if (!consume('[')) return err("expected '['");
        std::vector<Json> arr;
        skip_ws();
        if (consume(']')) return Json(std::move(arr));
        while (true) {
            auto val = parse_value();
            if (!val.ok()) return val.error();
            arr.push_back(val.value());
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            return err("expected ',' or ']'");
        }
        return Json(std::move(arr));
    }
};

void dump_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace

std::string Json::dump() const {
    std::string out;
    switch (type_) {
    case Type::Null: out = "null"; break;
    case Type::Bool: out = bool_ ? "true" : "false"; break;
    case Type::Number: {
        char buf[40];
        if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
            snprintf(buf, sizeof(buf), "%lld", (long long)num_);
        } else {
            snprintf(buf, sizeof(buf), "%.17g", num_);
        }
        out = buf;
        break;
    }
    case Type::String: dump_string(str_, out); break;
    case Type::Array: {
        out.push_back('[');
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out.push_back(',');
            out += arr_[i].dump();
        }
        out.push_back(']');
        break;
    }
    case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (auto& kv : obj_) {
            if (!first) out.push_back(',');
            first = false;
            dump_string(kv.first, out);
            out.push_back(':');
            out += kv.second.dump();
        }
        out.push_back('}');
        break;
    }
    }
    return out;
}

Result<Json> parse_json(std::string_view text) {
    // Skip a UTF-8 byte-order mark if present (PowerShell's -Encoding UTF8
    // writes one).
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB &&
        (uint8_t)text[2] == 0xBF) {
        text.remove_prefix(3);
    }
    Parser p{text.data(), text.data() + text.size()};
    auto v = p.parse_value();
    if (!v.ok()) return v.error();
    p.skip_ws();
    if (p.p != p.end) return Error(Err::InvalidArgument, "trailing characters after JSON value");
    return v.value();
}

} // namespace ast
