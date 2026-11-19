#pragma once
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nb {
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    struct Number { std::string text; };
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> data = nullptr;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : data(value) {}
    Json(int value) : data(Number{std::to_string(value)}) {}
    Json(std::uint64_t value) : data(Number{std::to_string(value)}) {}
    Json(Number value) : data(std::move(value)) {}
    Json(const char* value) : data(std::string(value)) {}
    Json(std::string value) : data(std::move(value)) {}
    Json(Array value) : data(std::move(value)) {}
    Json(Object value) : data(std::move(value)) {}

    const Json& operator[](std::string_view key) const {
        static const Json missing;
        const auto* object = std::get_if<Object>(&data);
        if (!object) return missing;
        const auto it = object->find(key);
        return it == object->end() ? missing : it->second;
    }
    const Array& array() const {
        static const Array empty;
        const auto* array = std::get_if<Array>(&data);
        return array ? *array : empty;
    }
    std::string string() const {
        const auto* text = std::get_if<std::string>(&data);
        return text ? *text : std::string{};
    }
    int integer() const {
        const auto* number = std::get_if<Number>(&data);
        return number ? std::stoi(number->text) : 0;
    }

    static std::string quote(std::string_view text) {
        constexpr char hex[] = "0123456789abcdef";
        std::string output = "\"";
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') output += '\\';
            if (c < 32) {
                output += "\\u00";
                output += hex[c >> 4];
                output += hex[c & 15];
            } else output += static_cast<char>(c);
        }
        return output + '"';
    }
    std::string dump() const {
        if (std::holds_alternative<std::nullptr_t>(data)) return "null";
        if (const auto* flag = std::get_if<bool>(&data)) return *flag ? "true" : "false";
        if (const auto* number = std::get_if<Number>(&data)) return number->text;
        if (const auto* text = std::get_if<std::string>(&data)) return quote(*text);
        std::string output;
        if (const auto* array = std::get_if<Array>(&data)) {
            output = "[";
            for (const auto& value : *array) {
                if (output.size() > 1) output += ',';
                output += value.dump();
            }
            return output + ']';
        }
        output = "{";
        for (const auto& [key, value] : std::get<Object>(data)) {
            if (output.size() > 1) output += ',';
            output += quote(key) + ':' + value.dump();
        }
        return output + '}';
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text(text) {}
    Json parse() {
        auto result = value(0);
        spaces();
        if (!text.empty()) fail();
        return result;
    }
private:
    std::string_view text;
    [[noreturn]] static void fail() { throw std::runtime_error("invalid JSON"); }
    void spaces() {
        while (!text.empty() && std::string_view(" \t\r\n").contains(text.front())) text.remove_prefix(1);
    }
    char take() {
        if (text.empty()) fail();
        const char c = text.front();
        text.remove_prefix(1);
        return c;
    }
    bool match(char c) {
        spaces();
        if (text.empty() || text.front() != c) return false;
        take();
        return true;
    }
    unsigned hex() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = take();
            value <<= 4;
            if (c >= '0' && c <= '9') value += c - '0';
            else if (c >= 'a' && c <= 'f') value += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value += c - 'A' + 10;
            else fail();
        }
        return value;
    }
    void unicode(std::string& result) {
        auto code = hex();
        if (code >= 0xd800 && code <= 0xdbff) {
            if (take() != '\\' || take() != 'u') fail();
            const auto low = hex();
            if (low < 0xdc00 || low > 0xdfff) fail();
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) fail();
        if (code < 0x80) result += static_cast<char>(code);
        else {
            const int tail = code < 0x800 ? 1 : code < 0x10000 ? 2 : 3;
            const unsigned prefix[] = {0, 0xc0, 0xe0, 0xf0};
            result += static_cast<char>(prefix[tail] | (code >> (6 * tail)));
            for (int i = tail - 1; i >= 0; --i)
                result += static_cast<char>(0x80 | ((code >> (6 * i)) & 0x3f));
        }
    }
    std::string string() {
        std::string result;
        while (true) {
            const char c = take();
            if (c == '"') return result;
            if (static_cast<unsigned char>(c) < 32) fail();
            if (c != '\\') {
                result += c;
                continue;
            }
            switch (const char escape = take()) {
            case '"': case '\\': case '/': result += escape; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': unicode(result); break;
            default: fail();
            }
        }
    }
    bool numeric() const { return !text.empty() && text.front() >= '0' && text.front() <= '9'; }
    void digits() {
        if (!numeric()) fail();
        while (numeric()) take();
    }
    bool next(char c) {
        if (text.empty() || text.front() != c) return false;
        take();
        return true;
    }
    Json number() {
        const auto original = text;
        next('-');
        if (!next('0')) digits();
        if (next('.')) digits();
        if (next('e') || next('E')) {
            if (!next('+')) next('-');
            digits();
        }
        return Json::Number{std::string(original.substr(0, original.size() - text.size()))};
    }
    Json value(int depth) {
        if (depth == 128) fail();
        spaces();
        if (match('"')) return string();
        if (match('[')) {
            Json::Array result;
            if (match(']')) return result;
            do { result.push_back(value(depth + 1)); } while (match(','));
            if (!match(']')) fail();
            return result;
        }
        if (match('{')) {
            Json::Object result;
            if (match('}')) return result;
            do {
                if (!match('"')) fail();
                auto key = string();
                if (!match(':')) fail();
                result.insert_or_assign(std::move(key), value(depth + 1));
            } while (match(','));
            if (!match('}')) fail();
            return result;
        }
        if (numeric() || text.starts_with('-')) return number();
        for (const auto literal : {"true", "false", "null"}) {
            if (!text.starts_with(literal)) continue;
            const std::string_view token(literal);
            text.remove_prefix(token.size());
            if (token == "null") return {};
            return token == "true";
        }
        fail();
    }
};
}
