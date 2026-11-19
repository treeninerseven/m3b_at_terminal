#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

// Independent, bounded view of the captured UART bytes. No client helpers.
namespace TestProtocol {
struct View {
  const uint8_t *data = nullptr;
  size_t size = 0;
  bool equals(const char *s) const {
    return size == strlen(s) && memcmp(data, s, size) == 0;
  }
};

inline bool nextLine(View input, size_t &offset, View &line) {
  const size_t start = offset;
  while (offset + 1 < input.size) {
    if (input.data[offset] == '\r' && input.data[offset + 1] == '\n') {
      line = {input.data + start, offset - start};
      offset += 2;
      return true;
    }
    ++offset;
  }
  offset = input.size;
  return false; // An unfinished line never counts as a response.
}

inline bool field(View input, const char *key, View &value) {
  size_t offset = 0;
  View line;
  const size_t n = strlen(key);
  while (nextLine(input, offset, line)) {
    if (line.size && (line.data[0] == '-' || line.data[0] == '*')) {
      ++line.data; --line.size;
    }
    if (line.size >= n + 1 && memcmp(line.data, key, n) == 0 && line.data[n] == ':') {
      value = {line.data + n + 1, line.size - n - 1};
      return true;
    }
  }
  return false;
}

inline bool number(View v, int64_t &out, unsigned base = 10) {
  if (!v.size || (base != 10 && base != 16)) return false;
  const bool negative = v.data[0] == '-';
  size_t i = negative ? 1 : 0;
  if (i == v.size || (negative && base != 10)) return false;
  uint64_t result = 0;
  for (; i < v.size; ++i) {
    const uint8_t c = v.data[i];
    unsigned digit = c >= '0' && c <= '9' ? c - '0' :
      c >= 'A' && c <= 'F' ? c - 'A' + 10 :
      c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
    if (digit >= base || result > (uint64_t(INT64_MAX) - digit) / base) return false;
    result = result * base + digit;
  }
  out = negative ? -int64_t(result) : int64_t(result);
  return true;
}

inline bool integer(View input, const char *key, int64_t &out) {
  View v;
  return field(input, key, v) && number(v, out);
}

inline bool bytes(View value, uint8_t *out, size_t capacity, size_t &length) {
  size_t tab = 0;
  while (tab < value.size && value.data[tab] != '\t') ++tab;
  int64_t count;
  if (tab == value.size || !number({value.data, tab}, count) || count < 0 ||
      uint64_t(count) > capacity) return false;
  View hex = {value.data + tab + 1, value.size - tab - 1};
  if (hex.size && hex.data[hex.size - 1] == 0x1A) --hex.size;
  if (hex.size != size_t(count) * 2) return false;
  for (size_t i = 0; i < size_t(count); ++i) {
    int64_t byte;
    if (!number({hex.data + 2 * i, 2}, byte, 16)) return false;
    out[i] = uint8_t(byte);
  }
  length = size_t(count);
  return true;
}

struct Result {
  int finalCode = -1;
  int info = 0, parserError = 0;
  bool hasInfo = false, hasParserError = false, hasMacError = false;
};

inline Result result(View input) {
  Result r;
  size_t offset = 0;
  View line;
  // The first final result belongs to this command; later boot/events stay in the trace.
  while (nextLine(input, offset, line)) {
    if (line.equals("0")) { r.finalCode = 0; break; }
    if (line.equals("1")) { r.finalCode = 1; break; }
    if (line.equals("2")) { r.finalCode = 2; break; }
    if (line.equals("100")) { r.finalCode = 100; break; }
  }
  int64_t v;
  r.hasInfo = integer(input, "INFO", v) || integer(input, "MINFO", v);
  if (r.hasInfo) r.info = int(v);
  r.hasParserError = integer(input, "AT!ERR", v);
  if (r.hasParserError) r.parserError = int(v);
  r.hasMacError = integer(input, "MAC_ERR", v) || integer(input, "MERR", v);
  return r;
}

// Numeric public enum values, checked against the header in host tests.
inline int mappedCode(const Result &r) {
  if (r.finalCode == 0) return 0;
  if (r.finalCode == 100) return 100;
  if (r.finalCode == 2) {
    if (!r.hasParserError) return 16;
    const int codes[] = {16, 17, 18, 19, 20, 24, 21, 22, 25};
    return r.parserError >= 1 && r.parserError <= 8 ? codes[r.parserError] : 16;
  }
  if (r.finalCode != 1) return -1;
  if (r.hasInfo) {
    switch (r.info) {
      case -1: return 5;
      case -2: return 4;
      case -3: return 6;
      case -4: return 8;
      case -5: return 11;
      case -6: return 15;
      case -9: return 13;
      case -100: return 100;
      default: return 9; // Public API has no dedicated code for the other INFO values.
    }
  }
  return r.hasMacError ? 1 : 9;
}

inline bool event(View input, const char *key, int expected) {
  // Inspect every occurrence: ACTIVE:1 followed by ACTIVE:0 is normal.
  size_t offset = 0;
  View line;
  while (nextLine(input, offset, line)) {
    // Reuse field() with the original CRLF included.
    int64_t value;
    if (integer({line.data, line.size + 2}, key, value) && value == expected) return true;
  }
  return false;
}
}
