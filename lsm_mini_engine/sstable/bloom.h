#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "iter/iterator.h"

struct Bloom {
  uint32_t num_bits{0};
  uint32_t k{0};
  std::string bits; // bytes

  bool MayContain(Slice key) const;
};

struct BloomBuilder {
  uint32_t k = 7;
  std::vector<uint32_t> hashes;

  void Add(Slice key);
  Bloom Finish(size_t bits_per_key = 10) const;
  static std::string Encode(const Bloom& b);
  static Status Decode(Slice in, Bloom* out);
};
