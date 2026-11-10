// Minimal JSON parser (objects, arrays, strings, numbers, bool, null).
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cctype>

namespace dds {

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double Num(double def = 0.0) const { return type == Number ? num : (type == Bool ? (b ? 1.0 : 0.0) : def); }
    int    Int(int def = 0) const { return (int)Num((double)def); }
    bool   AsBool(bool def = false) const { return type == Bool ? b : (type == Number ? num != 0.0 : def); }
    std::string Str(const char* def = "") const { return type == String ? str : def; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return true;
    }
private:
    const std::string& s_;
    size_t i_ = 0;

    void SkipWs() { while (i_ < s_.size() && (unsigned char)s_[i_] <= ' ') ++i_; }
    bool Eat(char c) { SkipWs(); if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }

    bool ParseValue(JsonValue& v) {
        SkipWs();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') { v.type = JsonValue::String; return ParseString(v.str); }
        if (c == 't' || c == 'f') return ParseBool(v);
        if (c == 'n') { i_ += 4; v.type = JsonValue::Null; return true; }
        return ParseNumber(v);
    }

    bool ParseString(std::string& out) {
        if (!Eat('"')) return false;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        unsigned code = 0;
                        for (int k = 0; k < 4 && i_ < s_.size(); ++k) {
                            char h = s_[i_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        }
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
                        else { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
                        break;
                    }
                    default: out += e; break;
                }
            } else out += c;
        }
        return false;
    }

    bool ParseNumber(JsonValue& v) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) ++i_;
        if (i_ == start) return false;
        v.type = JsonValue::Number;
        v.num = atof(s_.substr(start, i_ - start).c_str());
        return true;
    }

    bool ParseBool(JsonValue& v) {
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; v.type = JsonValue::Bool; v.b = true; return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.type = JsonValue::Bool; v.b = false; return true; }
        return false;
    }

    bool ParseArray(JsonValue& v) {
        if (!Eat('[')) return false;
        v.type = JsonValue::Array;
        SkipWs();
        if (Eat(']')) return true;
        while (true) {
            JsonValue item;
            if (!ParseValue(item)) return false;
            v.arr.push_back(std::move(item));
            SkipWs();
            if (Eat(',')) continue;
            return Eat(']');
        }
    }

    bool ParseObject(JsonValue& v) {
        if (!Eat('{')) return false;
        v.type = JsonValue::Object;
        SkipWs();
        if (Eat('}')) return true;
        while (true) {
            std::string key;
            SkipWs();
            if (!ParseString(key)) return false;
            if (!Eat(':')) return false;
            JsonValue val;
            if (!ParseValue(val)) return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (Eat(',')) continue;
            return Eat('}');
        }
    }
};

inline bool JsonParseFile(const std::string& text, JsonValue& out) {
    JsonParser p(text);
    return p.Parse(out);
}

} // namespace dds

