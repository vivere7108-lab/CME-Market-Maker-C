// A small JSON writer for the journal and a small reader for ``report``.
//
// The writer produces exactly what Python's ``json.dumps(...,
// separators=(",", ":"))`` produced for the same rows -- compact, keys in
// insertion order, ``NaN`` written as ``null`` -- so a journal from either
// implementation reads back with the same tool.  It appends to one string
// and never allocates beyond it; a snapshot row is a few hundred bytes.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace harvester::json {

class Writer {
public:
    Writer& begin_object();
    Writer& end_object();
    Writer& begin_array();
    Writer& end_array();
    // Writes the key of the next member; follow with one ``value``.
    Writer& key(std::string_view name);
    Writer& null();
    Writer& value(bool v);
    Writer& value(long long v);
    Writer& value(int v) { return value(static_cast<long long>(v)); }
    Writer& value(long v) { return value(static_cast<long long>(v)); }
    Writer& value(unsigned v) { return value(static_cast<long long>(v)); }
    Writer& value(unsigned long v) { return value(static_cast<long long>(v)); }
    Writer& value(unsigned long long v) { return value(static_cast<long long>(v)); }
    Writer& value(double v);
    Writer& value(std::string_view v);
    Writer& value(const char* v) { return value(std::string_view{v}); }
    Writer& value(const std::string& v) { return value(std::string_view{v}); }
    template <typename T>
    Writer& value(const std::optional<T>& v) {
        return v ? value(*v) : null();
    }
    Writer& value(const std::vector<std::string>& v);
    // A ``key`` and its ``value`` in one call.
    template <typename T>
    Writer& member(std::string_view name, const T& v) {
        key(name);
        return value(v);
    }
    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void separate();
    void escape(std::string_view text);
    std::string out_;
    // Number of values written so far in each open container.
    std::vector<int> counts_;
    bool after_key_ = false;
};

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }
    double number() const { return std::get<double>(data); }
    const std::string& string() const { return std::get<std::string>(data); }
    const Array& array() const { return std::get<Array>(data); }
    const Object& object() const { return std::get<Object>(data); }
    // A member of an object, or nullptr.
    const Value* get(std::string_view name) const;
    // A numeric member, if present and numeric.
    std::optional<double> number(std::string_view name) const;
};

// Parses one JSON document. Throws ``std::runtime_error`` on malformed input.
Value parse(std::string_view text);

}  // namespace harvester::json
