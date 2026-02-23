#pragma once
#include <cstdint>

constexpr uint64_t kSstMagic = 0x4C534D535354424CL; // "LSMSSTBL"

struct BlockHandle { uint64_t offset{0}, size{0}; };

struct Footer {
  BlockHandle index;
  BlockHandle meta;
};
