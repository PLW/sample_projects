#include "sstable/builder.h"

#include <cstring>

#include "sstable/format.h"
#include "sstable/block.h"

namespace {

static inline void PutFixed64(std::string& dst, uint64_t v) {
  char b[8];
  std::memcpy(b, &v, 8);
  dst.append(b, 8);
}

static inline void PutVarint64(std::string& dst, uint64_t v) {
  while (v >= 128) {
    dst.push_back(char((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst.push_back(char(v));
}

static inline void EncodeBlockHandle(std::string& dst, const sstable::BlockHandle& h) {
  PutVarint64(dst, h.offset);
  PutVarint64(dst, h.size);
}

static inline bool BytewiseLess(std::string_view a, std::string_view b) {
  return a < b; // lexicographic bytes
}

} // namespace

SSTableBuilder::SSTableBuilder(SstBuilderOptions opt, WritableFile* out)
  : opt_(opt),
    out_(out),
    data_block_(opt.block_size),
    index_block_(/*target*/ 16 * 1024) {}

Status SSTableBuilder::Add(Slice internal_key, Slice value) {
  if (finished_) return Status::Invalid("sst builder: finished");

  // enforce table-level strict increasing order (bytewise)
  if (!last_data_key_.empty()) {
    if (!BytewiseLess(last_data_key_, internal_key.sv())) {
      return Status::Invalid("sst builder: keys not increasing");
    }
  }

  // If adding this entry would exceed block target, flush current block first.
  // We use builder's size estimate rather than guessing.
  // Ensure at least one entry per block.
  if (!data_block_.Empty() &&
      data_block_.CurrentSizeEstimate() + internal_key.n + value.n + 16 >= opt_.block_size) {
    Status s = FlushDataBlock();
    if (!s) return s;
  }

  Status s = data_block_.Add(internal_key, value);
  if (!s) return s;

  last_data_key_.assign(internal_key.p, internal_key.n);
  last_key_in_current_block_.assign(internal_key.p, internal_key.n);
  return Status::OK();
}

Status SSTableBuilder::FlushDataBlock() {
  // finalize and write data block
  std::string block_bytes = data_block_.Finish();
  sstable::BlockHandle h;
  h.offset = offset_;
  h.size = block_bytes.size();

  Status s = out_->Append(Slice(block_bytes));
  if (!s) return s;
  offset_ += h.size;

  // index entry: key = last key in that data block (must be a real internal key)
  std::string bh;
  EncodeBlockHandle(bh, h);

  // Index block is uncompressed key/value entries too.
  s = index_block_.Add(Slice(last_key_in_current_block_), Slice(bh));
  if (!s) return s;

  last_key_in_current_block_.clear();
  return Status::OK();
}

Status SSTableBuilder::Finish() {
  if (finished_) return Status::Invalid("sst builder: finished");

  if (!data_block_.Empty()) {
    Status s = FlushDataBlock();
    if (!s) return s;
  }

  // Write index block
  std::string index_bytes = index_block_.Finish();
  sstable::BlockHandle index_h{offset_, index_bytes.size()};
  Status s = out_->Append(Slice(index_bytes));
  if (!s) return s;
  offset_ += index_h.size;

  // Meta block: empty for now
  std::string meta_bytes;
  sstable::BlockHandle meta_h{offset_, meta_bytes.size()};
  if (!meta_bytes.empty()) {
    s = out_->Append(Slice(meta_bytes));
    if (!s) return s;
    offset_ += meta_h.size;
  }

  // Footer: fixed 5*8 = 40 bytes
  std::string footer;
  PutFixed64(footer, index_h.offset);
  PutFixed64(footer, index_h.size);
  PutFixed64(footer, meta_h.offset);
  PutFixed64(footer, meta_h.size);
  PutFixed64(footer, sstable::kSstMagic);

  s = out_->Append(Slice(footer));
  if (!s) return s;
  offset_ += footer.size();

  finished_ = true;
  return Status::OK();
}

void SSTableBuilder::Abandon() {
  finished_ = true;
}
