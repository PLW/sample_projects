#pragma once
#include <cstdint>
#include <string>
#include "iter/iterator.h"

inline void PutFixed32(std::string& dst, uint32_t v) {
  char b[4]; std::memcpy(b, &v, 4); dst.append(b, 4);
}
inline void PutFixed64(std::string& dst, uint64_t v) {
  char b[8]; std::memcpy(b, &v, 8); dst.append(b, 8);
}

inline void PutVarint32(std::string& dst, uint32_t v) {
  while (v >= 128) { dst.push_back(char((v & 0x7F) | 0x80)); v >>= 7; }
  dst.push_back(char(v));
}
inline void PutVarint64(std::string& dst, uint64_t v) {
  while (v >= 128) { dst.push_back(char((v & 0x7F) | 0x80)); v >>= 7; }
  dst.push_back(char(v));
}

inline bool GetVarint64(Slice* in, uint64_t* out) {
  uint64_t result = 0;
  int shift = 0;
  const char* p = in->p;
  size_t n = in->n;
  for (size_t i = 0; i < n && shift <= 63; i++) {
    uint8_t byte = uint8_t(p[i]);
    result |= uint64_t(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      in->p += i + 1; in->n -= i + 1;
      return true;
    }
    shift += 7;
  }
  return false;
}
