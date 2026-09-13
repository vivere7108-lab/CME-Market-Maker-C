#include "harvester/util/json.hpp"

#include <cmath>
#include <cstdlib>
#include <format>
#include <stdexcept>

#include "harvester/util/format.hpp"

namespace harvester::json {

void Writer::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!counts_.empty()) {
        if (counts_.back() > 0) out_ += ',';
        ++counts_.back();
    }
}

Writer& Writer::begin_object() {
    separate();
    out_ += '{';
    counts_.push_back(0);
    return *this;
}

Writer& Writer::end_object() {
    counts_.pop_back();
    out_ += '}';
    return *this;
}

Writer& Writer::begin_array() {
    separate();
    out_ += '[';
    counts_.push_back(0);
    return *this;
}

Writer& Writer::end_array() {
    counts_.pop_back();
    out_ += ']';
    return *this;
}

Writer& Writer::key(std::string_view name) {
    separate();
    escape(name);
    out_ += ':';
    after_key_ = true;
    return *this;
}

Writer& Writer::null() {
    separate();
    out_ += "null";
    return *this;
}

Writer& Writer::value(bool v) {
    separate();
    out_ += v ? "true" : "false";
    return *this;
}

Writer& Writer::value(long long v) {
    separate();
    out_ += std::format("{}", v);
    return *this;
}

Writer& Writer::value(double v) {
    if (std::isnan(v) || std::isinf(v)) return null();
    separate();
    out_ += fmt::repr(v);
    return *this;
}

Writer& Writer::value(std::string_view v) {
    separate();
    escape(v);
    return *this;
}

Writer& Writer::value(const std::vector<std::string>& v) {
    begin_array();
    for (const auto& item : v) value(item);
    return end_array();
}

void Writer::escape(std::string_view text) {
    out_ += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20) {
                    out_ += std::format("\\u{:04x}", c);
                } else {
                    out_ += static_cast<char>(c);
                }
        }
    }
    out_ += '"';
}

const Value* Value::get(std::string_view name) const {
    if (!is_object()) return nullptr;
    const auto& obj = object();
    const auto it = obj.find(std::string{name});
    return it == obj.end() ? nullptr : &it->second;
}

std::optional<double> Value::number(std::string_view name) const {
    const Value* v = get(name);
    if (v == nullptr || !v->is_number()) return std::nullopt;
    return v->number();
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value document() {
        Value v = value();
        skip();
        if (pos_ != text_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::format("json: {} at offset {}", what, pos_));
    }
    void skip() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                       text_[pos_] == '\t')) {
            ++pos_;
        }
    }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++pos_;
    }
    bool consume(std::string_view word) {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Value value() {
        skip();
        switch (peek()) {
            case '{': return object();
            case '[': return array();
            case '"': return Value{string()};
            case 't': if (consume("true")) return Value{true}; break;
            case 'f': if (consume("false")) return Value{false}; break;
            case 'n': if (consume("null")) return Value{nullptr}; break;
            case 'N': if (consume("NaN")) return Value{std::nan("")}; break;
            case 'I': if (consume("Infinity")) return Value{INFINITY}; break;
            default: return number();
        }
        fail("bad literal");
    }

    Value object() {
        expect('{');
        Object out;
        skip();
        if (peek() == '}') {
            ++pos_;
            return Value{std::move(out)};
        }
        while (true) {
            skip();
            std::string name = string();
            skip();
            expect(':');
            out[std::move(name)] = value();
            skip();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return Value{std::move(out)};
        }
    }

    Value array() {
        expect('[');
        Array out;
        skip();
        if (peek() == ']') {
            ++pos_;
            return Value{std::move(out)};
        }
        while (true) {
            out.push_back(value());
            skip();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            return Value{std::move(out)};
        }
    }

    std::string string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) fail("bad escape");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) fail("bad unicode escape");
                    const unsigned code = std::strtoul(std::string(text_.substr(pos_, 4)).c_str(), nullptr, 16);
                    pos_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    Value number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.' ||
                                       text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
                                       text_[pos_] == '-')) {
            ++pos_;
        }
        if (start == pos_) fail("expected a value");
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') fail("bad number");
        return Value{v};
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace

Value parse(std::string_view text) { return Parser(text).document(); }

}  // namespace harvester::json
