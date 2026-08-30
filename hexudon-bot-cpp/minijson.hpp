// minijson.hpp — JSON parser tối giản cho bot HEXUDON (đủ dùng cho protocol).
// Không phụ thuộc thư viện ngoài. C++17.
#pragma once
#include <cctype>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mj {

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    enum Type { OBJ, ARR, STR, NUM, BOOL, NUL } type = NUL;
    std::map<std::string, ValuePtr> obj;
    std::vector<ValuePtr> arr;
    std::string str;
    double num = 0;
    bool boolean = false;

    // Truy cập tiện dụng (trả mặc định nếu thiếu khoá — đủ an toàn cho bot).
    const Value& operator[](const std::string& k) const {
        static Value nul;
        auto it = obj.find(k);
        return it == obj.end() ? nul : *it->second;
    }
    const Value& operator[](size_t i) const {
        static Value nul;
        return i < arr.size() ? *arr[i] : nul;
    }
    size_t size() const { return arr.size(); }
    int asInt() const { return static_cast<int>(num); }
    bool isNull() const { return type == NUL; }
};

class Parser {
    const std::string& s;
    size_t i = 0;

    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++; }
    char peek() { ws(); if (i >= s.size()) throw std::runtime_error("json: het chuoi"); return s[i]; }
    void expect(char c) { if (peek() != c) throw std::runtime_error(std::string("json: mong '") + c + "'"); i++; }

    ValuePtr parseValue() {
        char c = peek();
        if (c == '{') return parseObj();
        if (c == '[') return parseArr();
        if (c == '"') { auto v = std::make_shared<Value>(); v->type = Value::STR; v->str = parseStr(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { i += 4; auto v = std::make_shared<Value>(); return v; }
        return parseNum();
    }
    ValuePtr parseObj() {
        auto v = std::make_shared<Value>(); v->type = Value::OBJ;
        expect('{');
        if (peek() == '}') { i++; return v; }
        while (true) {
            std::string key = parseStr();
            expect(':');
            v->obj[key] = parseValue();
            if (peek() == ',') { i++; continue; }
            expect('}'); break;
        }
        return v;
    }
    ValuePtr parseArr() {
        auto v = std::make_shared<Value>(); v->type = Value::ARR;
        expect('[');
        if (peek() == ']') { i++; return v; }
        while (true) {
            v->arr.push_back(parseValue());
            if (peek() == ',') { i++; continue; }
            expect(']'); break;
        }
        return v;
    }
    std::string parseStr() {
        expect('"');
        std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                i++;
                char c = s[i++];
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'u': i += 4; out += '?'; break; // bot không cần unicode
                    default: out += c;
                }
            } else out += s[i++];
        }
        i++; // đóng "
        return out;
    }
    ValuePtr parseBool() {
        auto v = std::make_shared<Value>(); v->type = Value::BOOL;
        if (s[i] == 't') { v->boolean = true; i += 4; } else { v->boolean = false; i += 5; }
        return v;
    }
    ValuePtr parseNum() {
        auto v = std::make_shared<Value>(); v->type = Value::NUM;
        size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) i++;
        v->num = std::stod(s.substr(start, i - start));
        return v;
    }

public:
    explicit Parser(const std::string& in) : s(in) {}
    ValuePtr parse() { return parseValue(); }
};

inline ValuePtr parse(const std::string& in) { return Parser(in).parse(); }

} // namespace mj
