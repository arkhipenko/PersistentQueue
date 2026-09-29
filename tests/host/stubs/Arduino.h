// Host stub of the Arduino API used by PersistentQueue: the String subset it calls.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// arduino-esp32 3.x; the IDF 3.x build of the tests (PQ_TEST_IDF3) models a 1.x core without it
#ifndef PQ_TEST_IDF3
#define ESP_ARDUINO_VERSION_MAJOR 3
#endif

class String {
  public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    String(char c) : s_(1, c) {}
    unsigned int length() const { return (unsigned int) s_.size(); }
    const char* c_str() const { return s_.c_str(); }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    bool endsWith(const String& p) const {
      return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
    }
    void remove(unsigned int idx) { if (idx < s_.size()) s_.erase(idx); }
    // Same rules as the ESP32 core WString::substring
    String substring(unsigned int left, unsigned int right) const {
      if (left > right) { unsigned int t = left; left = right; right = t; }
      if (left > s_.size()) return String();
      if (right > s_.size()) right = (unsigned int) s_.size();
      return String(s_.substr(left, right - left));
    }
    long toInt() const { return atol(s_.c_str()); }
    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
    friend String operator+(const String& a, char c) { return String(a.s_ + c); }
    friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s_); }
    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator!=(const String& o) const { return s_ != o.s_; }
    const std::string& str() const { return s_; }
  private:
    std::string s_;
};
