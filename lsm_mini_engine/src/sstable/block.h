#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iter/iterator.h"   // Status, Slice

namespace sstable {

// A finished block is:
//   entries... + [num_entries fixed32] + [offsets fixed32[num_entries]]
//
// Each entry is:
//   key_len varint32
//   val_len varint32
//   key bytes
//   val bytes
//
// offsets[i] points to start of entry i (from block start).
class BlockBuilder {
public:
  explicit BlockBuilder(size_t target_bytes);

  // Keys must be added in strictly increasing bytewise order.
  Status Add(Slice key, Slice value);

  // Finalize and return the encoded block bytes.
  // After Finish(), builder is reset and can be reused.
  std::string Finish();

  void Reset();

  size_t CurrentSizeEstimate() const { return buf_.size() + offsets_.size() * 4 + 4; }
  bool Empty() const { return offsets_.empty(); }

private:
  size_t target_;
  std::string buf_;
  std::vector<uint32_t> offsets_;
  std::string last_key_;
};

struct BlockView {
  Slice data;              // whole block bytes
  uint32_t num_entries{0};
  const uint32_t* offsets{nullptr}; // points into backing string; little-endian assumed
};

// Parse a block view from raw bytes (does not copy).
Status ParseBlock(Slice raw, BlockView* out);

// Decode entry i into key/value slices that reference the block bytes.
Status EntryAt(const BlockView& b, uint32_t i, Slice* key_out, Slice* val_out);

} // namespace sstable
