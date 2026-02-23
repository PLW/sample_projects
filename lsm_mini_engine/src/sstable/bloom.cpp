#include "sstable/bloom.h"

#include <cstring>
#include <string>

#include "sstable/varint.h"

// Simple 32-bit hash (FNV-1a). Replace later with xxhash if you like.
static uint32_t fnv1a32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

bool Bloom::MayContain(Slice key) const {
  if (num_bits == 0 || bits.empty()) return true; // conservative
  uint32_t h1 = fnv1a32(reinterpret_cast<const uint8_t*>(key.p), key.n);
  uint32_t h2 = rotl32(h1, 15) * 0x9e3779b1u;

  for (uint32_t i = 0; i < k; i++) {
    uint32_t bit = (h1 + i * h2) % num_bits;
    uint32_t byte = bit >> 3;
    uint8_t mask = uint8_t(1u << (bit & 7));
    if (byte >= bits.size()) return true;
    if ((uint8_t(bits[byte]) & mask) == 0) return false;
  }
  return true;
}

void BloomBuilder::Add(Slice key) {
  uint32_t h = fnv1a32(reinterpret_cast<const uint8_t*>(key.p), key.n);
  hashes.push_back(h);
}

Bloom BloomBuilder::Finish(size_t bits_per_key) const {
  Bloom b;
  b.k = k;
  if (hashes.empty()) return b;

  uint64_t nb = uint64_t(hashes.size()) * uint64_t(bits_per_key);
  if (nb < 64) nb = 64;
  b.num_bits = uint32_t(nb);
  size_t nbytes = (b.num_bits + 7) / 8;
  b.bits.assign(nbytes, '\0');

  for (uint32_t h1 : hashes) {
    uint32_t h2 = rotl32(h1, 15) * 0x9e3779b1u;
    for (uint32_t i = 0; i < b.k; i++) {
      uint32_t bit = (h1 + i * h2) % b.num_bits;
      b.bits[bit >> 3] = char(uint8_t(b.bits[bit >> 3]) | uint8_t(1u << (bit & 7)));
    }
  }
  return b;
}

std::string BloomBuilder::Encode(const Bloom& b) {
  // meta := varint32 num_bits | varint32 k | bytes
  std::string out;
  PutVarint32(out, b.num_bits);
  PutVarint32(out, b.k);
  out.append(b.bits);
  return out;
}

Status BloomBuilder::Decode(Slice in, Bloom* out) {
  Slice s = in;
  uint64_t nb = 0, kk = 0;
  if (!GetVarint64(&s, &nb)) return Status::Corruption("bloom: nb");
  if (!GetVarint64(&s, &kk)) return Status::Corruption("bloom: k");
  if (nb > (1u<<31) || kk > 64) return Status::Corruption("bloom: bad params");

  out->num_bits = uint32_t(nb);
  out->k = uint32_t(kk);
  out->bits.assign(s.p, s.n);
  return Status::OK();
}
