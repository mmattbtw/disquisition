#include "console/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace console::json {
namespace {
constexpr int kMaxDepth = 64;
const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;
const Value kUndefined = Value::undefined();

[[noreturn]] void fail(const char* reason) {
    throw std::runtime_error(reason);
}

// Decodes one UTF-8 (or generalized UTF-8 for lone surrogates) sequence.
// Returns the code point, or -1 for an invalid sequence.
long decodeUtf8(std::string_view text, std::size_t& at) {
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const unsigned char first = byte(at);
    if (first < 0x80) {
        ++at;
        return first;
    }
    int length = 0;
    long value = 0;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        value = first & 0x1f;
    }
    else if ((first & 0xf0) == 0xe0) {
        length = 3;
        value = first & 0x0f;
    }
    else if ((first & 0xf8) == 0xf0) {
        length = 4;
        value = first & 0x07;
    }
    else
        return -1;
    if (at + length > text.size())
        return -1;
    for (int index = 1; index < length; ++index) {
        const auto next = byte(at + index);
        if ((next & 0xc0) != 0x80)
            return -1;
        value = (value << 6) | (next & 0x3f);
    }
    static const long minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < minimum[length] || value > 0x10ffff)
        return -1;
    at += length;
    return value;
}

void appendUtf8(std::string& out, long code) {
    if (code < 0x80)
        out += char(code);
    else if (code < 0x800) {
        out += char(0xc0 | (code >> 6));
        out += char(0x80 | (code & 0x3f));
    }
    else if (code < 0x10000) {
        out += char(0xe0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3f));
        out += char(0x80 | (code & 0x3f));
    }
    else {
        out += char(0xf0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3f));
        out += char(0x80 | ((code >> 6) & 0x3f));
        out += char(0x80 | (code & 0x3f));
    }
}

char hexDigit(unsigned value) {
    return char(value < 10 ? '0' + value : 'a' + value - 10);
}

void escapeUnit(std::string& out, unsigned unit) {
    out += "\\u";
    out += hexDigit((unit >> 12) & 0xf);
    out += hexDigit((unit >> 8) & 0xf);
    out += hexDigit((unit >> 4) & 0xf);
    out += hexDigit(unit & 0xf);
}

// Qt escapes quotes, backslashes and C0 controls, writes other characters
// as UTF-8, and escapes UTF-16 units that cannot be encoded (lone surrogates).
void writeString(std::string& out, std::string_view text) {
    out += '"';
    for (std::size_t at = 0; at < text.size();) {
        const auto code = decodeUtf8(text, at);
        if (code < 0) {
            // Invalid bytes never come from the parser; replace like QString.
            ++at;
            out += "\xef\xbf\xbd";
            continue;
        }
        if (code >= 0xd800 && code <= 0xdfff) {
            escapeUnit(out, unsigned(code));
            continue;
        }
        if (code < 0x80) {
            switch (code) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (code < 0x20) {
                    out += "\\u00";
                    out += hexDigit(unsigned(code) >> 4);
                    out += hexDigit(unsigned(code) & 0xf);
                }
                else
                    out += char(code);
            }
            continue;
        }
        appendUtf8(out, code);
    }
    out += '"';
}

void writeDouble(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
        out += std::to_string(static_cast<long long>(value));
        return;
    }
    char buffer[40];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value)
            break;
    }
    out += buffer;
}

void writeValue(std::string& out, const Value& value) {
    switch (value.type()) {
    case Value::Type::Undefined:
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += value.toBool() ? "true" : "false"; break;
    case Value::Type::Integer:
    case Value::Type::Double: {
        if (value.type() == Value::Type::Double)
            writeDouble(out, value.toDouble());
        else
            out += std::to_string(static_cast<long long>(value.toInteger()));
        break;
    }
    case Value::Type::String: writeString(out, value.toString()); break;
    case Value::Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& item : value.toArray()) {
            if (!first)
                out += ',';
            first = false;
            writeValue(out, item);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.toObject()) {
            if (item.isUndefined())
                continue;
            if (!first)
                out += ',';
            first = false;
            writeString(out, key);
            out += ':';
            writeValue(out, item);
        }
        out += '}';
        break;
    }
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    Value document() {
        skip();
        auto result = value(0);
        skip();
        if (at_ != text_.size())
            fail("trailing JSON data");
        return result;
    }

private:
    std::string_view text_;
    std::size_t at_ = 0;

    void skip() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }
    bool literal(const char* word) {
        const auto size = std::strlen(word);
        if (text_.substr(at_, size) != word)
            return false;
        at_ += size;
        return true;
    }
    Value value(int depth) {
        if (depth > kMaxDepth)
            fail("JSON nesting is too deep");
        if (at_ >= text_.size())
            fail("truncated JSON");
        const char first = text_[at_];
        if (first == '{')
            return object(depth);
        if (first == '[')
            return array(depth);
        if (first == '"')
            return Value(string());
        if (literal("true"))
            return Value(true);
        if (literal("false"))
            return Value(false);
        if (literal("null"))
            return Value(nullptr);
        return number();
    }
    Value object(int depth) {
        ++at_;
        Object result;
        skip();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return Value(std::move(result));
        }
        for (;;) {
            skip();
            if (at_ >= text_.size() || text_[at_] != '"')
                fail("expected JSON object key");
            auto key = string();
            skip();
            if (at_ >= text_.size() || text_[at_] != ':')
                fail("expected JSON colon");
            ++at_;
            skip();
            result[std::move(key)] = value(depth + 1);
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return Value(std::move(result));
            }
            fail("expected JSON object separator");
        }
    }
    Value array(int depth) {
        ++at_;
        Array result;
        skip();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return Value(std::move(result));
        }
        for (;;) {
            skip();
            result.push_back(value(depth + 1));
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return Value(std::move(result));
            }
            fail("expected JSON array separator");
        }
    }
    unsigned hex4() {
        if (at_ + 4 > text_.size())
            fail("truncated JSON escape");
        unsigned result = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = text_[at_++];
            result <<= 4;
            if (ch >= '0' && ch <= '9')
                result |= unsigned(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                result |= unsigned(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                result |= unsigned(ch - 'A' + 10);
            else
                fail("invalid JSON escape");
        }
        return result;
    }
    std::string string() {
        ++at_;
        std::string result;
        for (;;) {
            if (at_ >= text_.size())
                fail("unterminated JSON string");
            const auto ch = static_cast<unsigned char>(text_[at_]);
            if (ch == '"') {
                ++at_;
                return result;
            }
            if (ch < 0x20)
                fail("control character in JSON string");
            if (ch == '\\') {
                ++at_;
                if (at_ >= text_.size())
                    fail("truncated JSON escape");
                const char escape = text_[at_++];
                switch (escape) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    long code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff && at_ + 6 <= text_.size() &&
                        text_[at_] == '\\' && text_[at_ + 1] == 'u') {
                        const auto saved = at_;
                        at_ += 2;
                        const auto low = hex4();
                        if (low >= 0xdc00 && low <= 0xdfff)
                            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                        else
                            at_ = saved;
                    }
                    // Lone surrogates stay distinct so they serialize unchanged.
                    appendUtf8(result, code);
                    break;
                }
                default: fail("invalid JSON escape");
                }
                continue;
            }
            const auto start = at_;
            if (decodeUtf8(text_, at_) < 0)
                fail("invalid UTF-8 in JSON string");
            result.append(text_.substr(start, at_ - start));
        }
    }
    Value number() {
        const auto start = at_;
        bool integer = true;
        if (at_ < text_.size() && text_[at_] == '-')
            ++at_;
        const auto digits = [&] {
            const auto from = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
                ++at_;
            return at_ - from;
        };
        if (at_ < text_.size() && text_[at_] == '0')
            ++at_;
        else if (digits() == 0)
            fail("invalid JSON value");
        if (at_ < text_.size() && text_[at_] == '.') {
            integer = false;
            ++at_;
            if (digits() == 0)
                fail("invalid JSON number");
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            integer = false;
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-'))
                ++at_;
            if (digits() == 0)
                fail("invalid JSON number");
        }
        const std::string token(text_.substr(start, at_ - start));
        if (integer) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0')
                return Value(static_cast<std::int64_t>(parsed));
        }
        return Value(std::strtod(token.c_str(), nullptr));
    }
};
} // namespace

Value::Value(double value) : type_(Type::Double), double_(value) {}
Value::Value(json::Array value) :
    type_(Type::Array), array_(std::make_shared<json::Array>(std::move(value))) {}
Value::Value(json::Object value) :
    type_(Type::Object), object_(std::make_shared<json::Object>(std::move(value))) {}

Value Value::undefined() {
    Value result;
    result.type_ = Type::Undefined;
    return result;
}

double Value::toDouble(double fallback) const {
    if (type_ == Type::Integer)
        return static_cast<double>(integer_);
    if (type_ == Type::Double)
        return double_;
    return fallback;
}

std::int64_t Value::toInteger(std::int64_t fallback) const {
    if (type_ == Type::Integer)
        return integer_;
    if (type_ == Type::Double && std::isfinite(double_) && double_ == std::floor(double_) &&
        std::fabs(double_) < 9223372036854775807.0)
        return static_cast<std::int64_t>(double_);
    return fallback;
}

int Value::toInt(int fallback) const {
    if (type_ == Type::Integer)
        return integer_ >= std::numeric_limits<int>::min() &&
                       integer_ <= std::numeric_limits<int>::max()
                   ? int(integer_)
                   : fallback;
    if (type_ == Type::Double && std::isfinite(double_) && double_ == std::floor(double_) &&
        double_ >= std::numeric_limits<int>::min() && double_ <= std::numeric_limits<int>::max())
        return int(double_);
    return fallback;
}

const std::string& Value::toString() const {
    return type_ == Type::String ? string_ : kEmptyString;
}
const json::Array& Value::toArray() const {
    return type_ == Type::Array ? *array_ : kEmptyArray;
}
const json::Object& Value::toObject() const {
    return type_ == Type::Object ? *object_ : kEmptyObject;
}
json::Array& Value::array() {
    if (type_ != Type::Array)
        *this = Value(json::Array{});
    else if (array_.use_count() > 1)
        array_ = std::make_shared<json::Array>(*array_);
    return *array_;
}
json::Object& Value::object() {
    if (type_ != Type::Object)
        *this = Value(json::Object{});
    else if (object_.use_count() > 1)
        object_ = std::make_shared<json::Object>(*object_);
    return *object_;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object)
        return kUndefined;
    const auto found = object_->find(key);
    return found == object_->end() ? kUndefined : found->second;
}

bool Value::contains(const std::string& key) const {
    return type_ == Type::Object && object_->count(key) != 0;
}

bool Value::operator==(const Value& other) const {
    if (isNumber() && other.isNumber())
        return toDouble() == other.toDouble();
    if (type_ != other.type_)
        return false;
    switch (type_) {
    case Type::Undefined:
    case Type::Null: return true;
    case Type::Bool: return bool_ == other.bool_;
    case Type::String: return string_ == other.string_;
    case Type::Array: return array_ == other.array_ || *array_ == *other.array_;
    case Type::Object: return object_ == other.object_ || *object_ == *other.object_;
    default: return false;
    }
}

Value parse(std::string_view text) {
    return Parser(text).document();
}

std::string write(const Value& value) {
    std::string out;
    writeValue(out, value);
    return out;
}

std::size_t utf16Length(std::string_view utf8) {
    std::size_t length = 0;
    for (std::size_t at = 0; at < utf8.size();) {
        const auto code = decodeUtf8(utf8, at);
        if (code < 0)
            ++at;
        length += code >= 0x10000 ? 2 : 1;
    }
    return length;
}

bool hasControl(std::string_view utf8, bool allowNewlineAndTab) {
    for (std::size_t at = 0; at < utf8.size();) {
        const auto code = decodeUtf8(utf8, at);
        if (code < 0) {
            ++at;
            continue;
        }
        if (allowNewlineAndTab && (code == '\n' || code == '\t'))
            continue;
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f))
            return true;
    }
    return false;
}

bool blank(std::string_view utf8) {
    for (std::size_t at = 0; at < utf8.size();) {
        const auto code = decodeUtf8(utf8, at);
        if (code < 0) {
            ++at;
            return false;
        }
        const bool space = (code >= 0x09 && code <= 0x0d) || code == 0x20 || code == 0x85 ||
                           code == 0xa0 || code == 0x1680 || (code >= 0x2000 && code <= 0x200a) ||
                           code == 0x2028 || code == 0x2029 || code == 0x202f || code == 0x205f ||
                           code == 0x3000;
        if (!space)
            return false;
    }
    return true;
}

} // namespace console::json
