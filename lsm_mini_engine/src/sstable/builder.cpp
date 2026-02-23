#include "sstable/builder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "sstable/bloom.h"
#include "sstable/format.h"
#include "sstable/varint.h"

static void PutFixed64LE(std::string& dst, uint64_t v) { PutFixed64(dst, v); }

static std::string ShortSeparator(std::string_view a, std::string_view b) {
  // Return a key >= a and < b (best effort). If impossible, return a.
  size_t n = std::min(a.size(), b.size());
  size_t diff = 0;
  while (diff < n && a[diff] == b[diff]) diff++;
  if (diff == n) return std::string(a);
  uint8_t ca = uint8_t(a[diff]);
  uint8_t cb = uint8_t(b[diff]);
  if (ca + 1 < cb) {
    std::string out(a);
    out[diff] = char(ca + 1);
    out.resize(diff + 1);
    return out;
  }
  return std::string(a);
}

static void AppendEntry(std::string& block,
                        std::string_view last_key,
                        std::string_view key,
                        std::string_view value,
                        std::vector<uint32_t>& restarts,
                        int restart_interval,
                        int& entries_since_restart) {
  uint32_t shared = 0;
  if (entries_since_restart < restart_interval) {
    size_t n = std::min(last_key.size(), key.size());
    while (shared < n && last_key[shared] == key[shared]) shared++;
  } else {
    restarts.push_back(uint32_t(block.size()));
    entries_since_restart = 0;
    shared = 0;
  }
  uint32_t unshared = uint32_t(key.size() - shared);

  PutVarint32(block, shared);
  PutVarint32(block, unshared);
  PutVarint32(block, uint32_t(value.size()));
  block.append(key.data() + shared, unshared);
  block.append(value.data(), value.size());

  entries_since_restart++;
}

static void FinishBlock(std::string& block, std::vector<uint32_t>& restarts) {
  if (restarts.empty()) restarts.push_back(0);
  for (uint32_t off : restarts) PutFixed32(block, off);
  PutFixed32(block, uint32_t(restarts.size()));
}

static void EncodeBlockHandle(std::string& dst, const BlockHandle& h) {
  PutVarint64(dst, h.offset);
  PutVarint64(dst, h.size);
}

SSTableBuilder::SSTableBuilder(SstBuilderOptions opt, WritableFile* out)
  : opt_(opt), out_(out) {
  restarts_.push_back(0);
  index_restarts_.push_back(0);
}

Status SSTableBuilder::Add(Slice internal_key, Slice value) {
  if (finished_) return Status::Invalid("sst builder: finished");
  if (!last_key_.empty()) {
    if (cmp_(Slice(last_key_), internal_key) >= 0) {
      return Status::Invalid("sst builder: keys not increasing");
    }
  }

  // Flush pending index entry if set and we now know next key.
  if (has_pending_index_) {
    std::string sep = ShortSeparator(pending_index_key_, internal_key.sv());
    // add to index_block_ as a block-encoded entry
    std::string bh;
    EncodeBlockHandle(bh, pending_handle_);
    AppendEntry(index_block_, /*last_key=*/last_key_, sep, bh,
                index_restarts_, opt_.restart_interval, index_entries_since_restart_);
    has_pending_index_ = false;
  }

  // If data block would exceed target, flush it.
  // Rough estimate: varints + key/value bytes.
  size_t approx = data_block_.size() + internal_key.n + value.n + 20;
  if (approx >= opt_.block_size && !data_block_.empty()) {
    Status s = FlushDataBlock();
    if (!s) return s;
  }

  // Append entry into data block
  AppendEntry(data_block_, last_key_, internal_key.sv(), value.sv(),
              restarts_, opt_.restart_interval, entries_since_restart_);
  last_key_.assign(internal_key.p, internal_key.n);

  // Add bloom hash via local fnv1a over user_key
  if (opt_.build_bloom) {
    Slice uk; uint64_t tr = 0;
    if (DecodeInternalKey(internal_key, &uk, &tr)) {
      uint32_t h = 2166136261u;
      for (size_t i = 0; i < uk.n; i++) { h ^= uint8_t(uk.p[i]); h *= 16777619u; }
      bloom_hashes_.push_back(h);
    }
  }

  return Status::OK();
}

Status SSTableBuilder::FlushDataBlock() {
  // finalize current data block bytes
  FinishBlock(data_block_, restarts_);

  BlockHandle h;
  h.offset = offset_;
  h.size = data_block_.size();

  Status s = out_->Append(Slice(data_block_));
  if (!s) return s;

  offset_ += h.size;

  // record pending index entry key = last_key_ (we'll separator once we see next key)
  pending_index_key_ = last_key_;
  pending_handle_ = h;
  has_pending_index_ = true;

  // reset block
  data_block_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  entries_since_restart_ = 0;

  return Status::OK();
}

Status SSTableBuilder::Finish() {
  if (finished_) return Status::Invalid("sst builder: finished");
  // Flush final data block if non-empty
  if (!data_block_.empty()) {
    Status s = FlushDataBlock();
    if (!s) return s;
  }

  // Flush final pending index entry using its own key (no separator available).
  if (has_pending_index_) {
    std::string bh;
    EncodeBlockHandle(bh, pending_handle_);
    AppendEntry(index_block_, /*last_key=*/std::string_view(),
                pending_index_key_, bh,
                index_restarts_, opt_.restart_interval, index_entries_since_restart_);
    has_pending_index_ = false;
  }
  FinishBlock(index_block_, index_restarts_);

  // Write index block
  BlockHandle index_h{offset_, uint64_t(index_block_.size())};
  Status s = out_->Append(Slice(index_block_));
  if (!s) return s;
  offset_ += index_h.size;

  // Meta block: bloom only (encoded as: varint32 tag=1 | varint32 len | bloom_bytes)
  std::string meta;
  if (opt_.build_bloom) {
    // Build bloom from stored hashes.
    BloomBuilder bb;
    bb.k = opt_.bloom_k;
    bb.hashes = bloom_hashes_;
    Bloom bloom = bb.Finish(/*bits_per_key=*/10);
    std::string bloom_bytes = BloomBuilder::Encode(bloom);

    // tag 1 = bloom
    PutVarint32(meta, 1);
    PutVarint32(meta, uint32_t(bloom_bytes.size()));
    meta.append(bloom_bytes);
  }
  BlockHandle meta_h{offset_, uint64_t(meta.size())};
  s = out_->Append(Slice(meta));
  if (!s) return s;
  offset_ += meta_h.size;

  // Footer: 4 fixed64 + magic fixed64
  std::string footer;
  PutFixed64LE(footer, index_h.offset);
  PutFixed64LE(footer, index_h.size);
  PutFixed64LE(footer, meta_h.offset);
  PutFixed64LE(footer, meta_h.size);
  PutFixed64LE(footer, kSstMagic);

  s = out_->Append(Slice(footer));
  if (!s) return s;
  offset_ += footer.size();

  finished_ = true;
  return Status::OK();
}

void SSTableBuilder::Abandon() {
  finished_ = true;
}

uint64_t SSTableBuilder::FileSize() const { return offset_; }
