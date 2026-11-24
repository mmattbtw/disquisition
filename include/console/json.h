#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A small JSON value for the console community client. Signed community data
// is verified over Qt's compact serialization, so write() reproduces it:
// sorted object keys, integers without a fraction, and Qt's string escapes.
namespace console::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
public:
    enum class Type { Undefined, Null, Bool, Integer, Double, String, Array, Object };
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool value) : type_(Type::Bool), bool_(value) {}
    Value(int value) : type_(Type::Integer), integer_(value) {}
    Value(std::int64_t value) : type_(Type::Integer), integer_(value) {}
    Value(double value);
    Value(const char* value) : type_(Type::String), string_(value) {}
    Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Value(json::Array value);
    Value(json::Object value);
    static Value undefined();

    Type type() const { return type_; }
    bool isUndefined() const { return type_ == Type::Undefined; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    // Like QJsonValue::isDouble(), true for every number.
    bool isNumber() const { return type_ == Type::Integer || type_ == Type::Double; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool toBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double toDouble(double fallback = 0) const;
    // QJsonValue::toInt(): a whole number within int range, else the fallback.
    int toInt(int fallback = 0) const;
    std::int64_t toInteger(std::int64_t fallback = 0) const;
    const std::string& toString() const;
    const json::Array& toArray() const;
    const json::Object& toObject() const;
    json::Array& array();
    json::Object& object();

    // Missing keys and non-objects yield an undefined value.
    const Value& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    Type type_;
    bool bool_ = false;
    std::int64_t integer_ = 0;
    double double_ = 0;
    std::string string_;
    std::shared_ptr<json::Array> array_;
    std::shared_ptr<json::Object> object_;
};

// Throws std::runtime_error for malformed input or excessive nesting.
Value parse(std::string_view text);
// Compact serialization matching QJsonDocument::toJson(Compact).
std::string write(const Value& value);
// community::canonical(): the signed byte form.
inline std::string canonical(const Value& value) { return write(value); }

// Helpers for the subset of QString semantics the community model checks.
std::size_t utf16Length(std::string_view utf8);
bool hasControl(std::string_view utf8, bool allowNewlineAndTab = false);
bool blank(std::string_view utf8);

} // namespace console::json
