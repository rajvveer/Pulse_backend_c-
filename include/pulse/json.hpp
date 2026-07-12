// json.hpp — compact, self-contained JSON value + parser + serializer.
//
// Dependency-free (no nlohmann fetch required). Supports the subset the Pulse
// algorithms exchange with Node: objects, arrays, strings, doubles, bools,
// null. UTF-8 is passed through byte-for-byte (we never need to decode it; the
// algorithms only do ASCII keyword/emoji byte matching, and emoji matching is
// done on raw UTF-8 byte sequences which is exactly what we want).
//
// API mirrors a tiny slice of nlohmann/json so the algo code reads naturally:
//   Json j = Json::parse(str);
//   double v = j["foo"].num();          j["arr"].size();   j.contains("k")
//   for (auto& el : j.arr()) ...        j["k"].str()       Json::object()
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <cstdint>

namespace pj {

class Json {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() : type_(Type::Null) {}
  Json(std::nullptr_t) : type_(Type::Null) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(double d) : type_(Type::Number), num_(d) {}
  Json(int i) : type_(Type::Number), num_(static_cast<double>(i)) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(const std::string& s) : type_(Type::String), str_(s) {}

  static Json object() { Json j; j.type_ = Type::Object; return j; }
  static Json array() { Json j; j.type_ = Type::Array; return j; }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isBool() const { return type_ == Type::Bool; }

  // Accessors with safe defaults (never throw on wrong type — return default).
  double num(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
  bool boolean(bool def = false) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
  }
  const std::string& str() const { static std::string empty; return type_ == Type::String ? str_ : empty; }
  std::string str(const std::string& def) const { return type_ == Type::String ? str_ : def; }

  // Object access. operator[] on a const object returns a static null if absent.
  bool contains(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
  }
  const Json& operator[](const std::string& key) const {
    static const Json nullVal;
    if (type_ != Type::Object) return nullVal;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullVal : it->second;
  }
  Json& operator[](const std::string& key) {
    if (type_ != Type::Object) { type_ = Type::Object; }
    return obj_[key];
  }
  const std::map<std::string, Json>& obj() const { return obj_; }

  // Array access.
  size_t size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
  }
  const std::vector<Json>& arr() const { return arr_; }
  std::vector<Json>& arr() { if (type_ != Type::Array) type_ = Type::Array; return arr_; }
  const Json& at(size_t i) const { static const Json nullVal; return i < arr_.size() ? arr_[i] : nullVal; }
  void push_back(const Json& v) { if (type_ != Type::Array) { type_ = Type::Array; } arr_.push_back(v); }

  // Convenience: read a nested numeric path like get("stats","likes").
  const Json& path(const std::string& a) const { return (*this)[a]; }

  // ── Serialization ──
  std::string dump() const {
    std::ostringstream os;
    write(os);
    return os.str();
  }

  // ── Parsing ──
  static Json parse(const std::string& s) {
    size_t i = 0;
    Json result = parseValue(s, i);
    skipWs(s, i);
    return result;
  }

private:
  Type type_;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Json> arr_;
  std::map<std::string, Json> obj_;

  void write(std::ostream& os) const {
    switch (type_) {
      case Type::Null: os << "null"; break;
      case Type::Bool: os << (bool_ ? "true" : "false"); break;
      case Type::Number: {
        if (std::isnan(num_) || std::isinf(num_)) { os << "0"; break; }
        if (num_ == static_cast<int64_t>(num_) && std::fabs(num_) < 1e15) {
          os << static_cast<int64_t>(num_);
        } else {
          std::ostringstream tmp; tmp.precision(10); tmp << num_; os << tmp.str();
        }
        break;
      }
      case Type::String: writeString(os, str_); break;
      case Type::Array: {
        os << '[';
        for (size_t k = 0; k < arr_.size(); ++k) { if (k) os << ','; arr_[k].write(os); }
        os << ']';
        break;
      }
      case Type::Object: {
        os << '{';
        bool first = true;
        for (auto& kv : obj_) {
          if (!first) os << ','; first = false;
          writeString(os, kv.first); os << ':'; kv.second.write(os);
        }
        os << '}';
        break;
      }
    }
  }

  static void writeString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
      switch (c) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); os << buf;
          } else {
            os << c; // pass UTF-8 bytes through
          }
      }
    }
    os << '"';
  }

  static void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  static Json parseValue(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return Json();
    char c = s[i];
    if (c == '{') return parseObject(s, i);
    if (c == '[') return parseArray(s, i);
    if (c == '"') return Json(parseString(s, i));
    if (c == 't' || c == 'f') return parseBool(s, i);
    if (c == 'n') { i += 4; return Json(); } // null
    return parseNumber(s, i);
  }

  static Json parseObject(const std::string& s, size_t& i) {
    Json o = Json::object();
    ++i; // {
    skipWs(s, i);
    if (i < s.size() && s[i] == '}') { ++i; return o; }
    while (i < s.size()) {
      skipWs(s, i);
      std::string key = parseString(s, i);
      skipWs(s, i);
      if (i < s.size() && s[i] == ':') ++i;
      o.obj_[key] = parseValue(s, i);
      skipWs(s, i);
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; break; }
      break;
    }
    return o;
  }

  static Json parseArray(const std::string& s, size_t& i) {
    Json a = Json::array();
    ++i; // [
    skipWs(s, i);
    if (i < s.size() && s[i] == ']') { ++i; return a; }
    while (i < s.size()) {
      a.arr_.push_back(parseValue(s, i));
      skipWs(s, i);
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; break; }
      break;
    }
    return a;
  }

  static std::string parseString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i; // opening quote
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') break;
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            // Decode \uXXXX to UTF-8 (BMP; handles surrogate pairs).
            if (i + 4 <= s.size()) {
              unsigned cp = std::stoul(s.substr(i, 4), nullptr, 16);
              i += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                unsigned lo = std::stoul(s.substr(i + 2, 4), nullptr, 16);
                i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              }
              appendUtf8(out, cp);
            }
            break;
          }
          default: out += e;
        }
      } else {
        out += c;
      }
    }
    return out;
  }

  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) out += static_cast<char>(cp);
    else if (cp <= 0x7FF) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  static Json parseBool(const std::string& s, size_t& i) {
    if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
    ++i; return Json(false);
  }

  static Json parseNumber(const std::string& s, size_t& i) {
    size_t start = i;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                            s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
    try { return Json(std::stod(s.substr(start, i - start))); }
    catch (...) { return Json(0.0); }
  }
};

} // namespace pj
