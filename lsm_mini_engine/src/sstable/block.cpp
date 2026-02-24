#include "sstable/block.h"

#include <algorithm>
#include <cstring>

namespace sstable {

static inline void PutFixed32(std::string& dst, uint32_t v) {
  char b[4];
  std::memcpy(b, &v, 4);
  dst.append(b, 4);
}

static inline bool GetFixed32(const char* p, uint32_t* out) {
  std::memcpy(out, p, 4);
  return true;
}

// Minimal varint32 encoder/decoder (7-bit continuation).
static inline void PutVarint32(std::string& dst, uint32_t v) {
  while (v >= 128) {
    dst.push_back(char((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst.push_back(char(v));
}

static inline bool GetVarint32(const char* base, size_t n, size_t* consumed, uint32_t* out) {
  uint32_t result = 0;
  int shift = 0;
  size_t i = 0;
  while (i < n && shift <= 28) {
    uint8_t byte = uint8_t(base[i]);
    result |= uint32_t(byte & 0x7F) << shift;
    i++;
    if ((byte & 0x80) == 0) {
      *out = result;
      *consumed = i;
      return true;
    }
    shift += 7;
  }
  return false;
}

BlockBuilder::BlockBuilder(size_t target_bytes) : target_(target_bytes) {}

Status BlockBuilder::Add(Slice key, Slice value) {
  // enforce bytewise strictly increasing keys
  if (!last_key_.empty()) {
    std::string_view a(last_key_);
    std::string_view b = key.sv();
    if (!(a < b)) return Status::Invalid("block: keys not increasing");
  }

  uint32_t off = uint32_t(buf_.size());
  offsets_.push_back(off);

  PutVarint32(buf_, uint32_t(key.n));
  PutVarint32(buf_, uint32_t(value.n));
  buf_.append(key.p, key.n);
  buf_.append(value.p, value.n);

  last_key_.assign(key.p, key.n);
  return Status::OK();
}

std::string BlockBuilder::Finish() {
  std::string out = buf_;
  // trailer: num_entries then offsets[]
  PutFixed32(out, uint32_t(offsets_.size()));
  for (uint32_t off : offsets_) PutFixed32(out, off);

  Reset();
  return out;
}

void BlockBuilder::Reset() {
  buf_.clear();
  offsets_.clear();
  last_key_.clear();
}

Status ParseBlock(Slice raw, BlockView* out) {
  if (raw.n < 4) return Status::Corruption("block: too small");
  uint32_t n = 0;
  GetFixed32(raw.p + (raw.n - 4), &n);

  uint64_t trailer_bytes = 4ull + 4ull * uint64_t(n);
  if (raw.n < trailer_bytes) return Status::Corruption("block: bad trailer");

  const char* offsets_base = raw.p + (raw.n - trailer_bytes) + 4;
  out->data = raw;
  out->num_entries = n;
  out->offsets = reinterpret_cast<const uint32_t*>(offsets_base);
  return Status::OK();
}

Status EntryAt(const BlockView& b, uint32_t i, Slice* key_out, Slice* val_out) {
  if (i >= b.num_entries) return Status::Invalid("block: index OOB");

  // trailer layout: [num_entries][offsets...]
  uint64_t trailer_bytes = 4ull + 4ull * uint64_t(b.num_entries);
  uint32_t block_limit = uint32_t(b.data.n - trailer_bytes);

  // offsets are little-endian fixed32
  uint32_t off = 0;
  std::memcpy(&off, reinterpret_cast<const char*>(b.offsets) + i * 4, 4);
  if (off >= block_limit) return Status::Corruption("block: offset past limit");

  const char* p = b.data.p + off;
  size_t n = size_t(block_limit - off);

  uint32_t klen = 0, vlen = 0;
  size_t c1 = 0, c2 = 0;
  if (!GetVarint32(p, n, &c1, &klen)) return Status::Corruption("block: bad klen");
  p += c1; n -= c1;
  if (!GetVarint32(p, n, &c2, &vlen)) return Status::Corruption("block: bad vlen");
  p += c2; n -= c2;

  if (n < uint64_t(klen) + uint64_t(vlen)) return Status::Corruption("block: entry short");

  *key_out = Slice(p, klen);
  p += klen;
  *val_out = Slice(p, vlen);
  return Status::OK();
}

} // namespace sstable
