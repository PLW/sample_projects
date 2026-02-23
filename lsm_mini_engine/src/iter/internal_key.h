#pragma once
#include <cstdint>
#include <string>
#include <cstring>
#include <compare>

enum class ValueType : uint8_t { kDel = 0, kPut = 1 };

inline uint64_t PackTrailer(uint64_t seq, ValueType t) {
  return (seq << 8) | uint64_t(uint8_t(t));
}

struct InternalKey {
  std::string user_key;
  uint64_t trailer{0}; // packed
};

inline std::string EncodeInternalKey(std::string_view user_key, uint64_t seq, ValueType t) {
  std::string out;
  out.append(user_key);
  uint64_t tr = PackTrailer(seq, t);
  char buf[8];
  std::memcpy(buf, &tr, 8);              // little-endian ok if consistent everywhere
  out.append(buf, 8);
  return out;
}

inline bool DecodeInternalKey(Slice k, Slice* user, uint64_t* trailer) {
  if (k.n < 8) return false;
  *user = Slice{k.p, k.n - 8};
  std::memcpy(trailer, k.p + (k.n - 8), 8);
  return true;
}

// Compare internal keys: user_key asc; trailer desc
struct InternalKeyComparator {
  int operator()(Slice a, Slice b) const {
    Slice au, bu; uint64_t at=0, bt=0;
    if (!DecodeInternalKey(a, &au, &at) || !DecodeInternalKey(b, &bu, &bt)) {
      // fallback raw compare
      auto sa = a.sv(), sb = b.sv();
      return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }
    if (au.sv() < bu.sv()) return -1;
    if (au.sv() > bu.sv()) return 1;
    // trailer desc => larger trailer sorts earlier
    if (at > bt) return -1;
    if (at < bt) return 1;
    return 0;
  }
};
