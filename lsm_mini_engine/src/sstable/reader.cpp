#include "sstable/reader.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sstable/varint.h"
#include "sstable/format.h"
#include "sstable/bloom.h"
#include "iter/internal_key.h"

// ---------- Block iterator (prefix-compressed w/ restart array) ----------
namespace {

struct BlockView {
  Slice data;               // whole block bytes
  const char* restarts{nullptr};
  uint32_t num_restarts{0};
  uint32_t restart_offset0{0};
};

static Status ParseBlock(Slice block, BlockView* out) {
  if (block.n < 4) return Status::Corruption("block: too small");
  uint32_t nr = 0;
  std::memcpy(&nr, block.p + (block.n - 4), 4);
  size_t restarts_bytes = size_t(nr) * 4 + 4;
  if (block.n < restarts_bytes) return Status::Corruption("block: bad restarts");
  out->data = block;
  out->num_restarts = nr;
  out->restarts = block.p + (block.n - restarts_bytes);
  if (nr > 0) std::memcpy(&out->restart_offset0, out->restarts, 4);
  return Status::OK();
}

static uint32_t RestartAt(const BlockView& b, uint32_t i) {
  uint32_t off = 0;
  std::memcpy(&off, b.restarts + i * 4, 4);
  return off;
}

static bool ReadEntryAt(const BlockView& b, uint32_t offset,
                        std::string* last_key_io,
                        Slice* key_out, Slice* val_out,
                        uint32_t* next_offset_out) {
  Slice in(b.data.p + offset, b.data.n - offset);

  uint64_t shared=0, unshared=0, vlen=0;
  if (!GetVarint64(&in, &shared)) return false;
  if (!GetVarint64(&in, &unshared)) return false;
  if (!GetVarint64(&in, &vlen)) return false;
  if (shared > last_key_io->size()) return false;
  if (in.n < unshared + vlen) return false;

  std::string key = last_key_io->substr(0, size_t(shared));
  key.append(in.p, size_t(unshared));
  *last_key_io = key;

  const char* vptr = in.p + unshared;
  *key_out = Slice(last_key_io->data(), last_key_io->size());
  *val_out = Slice(vptr, size_t(vlen));

  // Compute next offset:
  size_t consumed = (b.data.n - offset) - in.n + size_t(unshared + vlen);
  *next_offset_out = offset + uint32_t(consumed);
  return true;
}

class BlockIter final : public Iterator {
public:
  BlockIter(BlockView bv, InternalKeyComparator cmp)
    : b_(bv), cmp_(cmp) {}

  void SeekToFirst() override {
    status_ = Status::OK();
    last_key_.clear();
    off_ = (b_.num_restarts ? RestartAt(b_, 0) : 0);
    ParseAtOrInvalidate();
  }

  void Seek(Slice target) override {
    status_ = Status::OK();
    // binary search restart points by key
    uint32_t lo = 0, hi = b_.num_restarts;
    while (lo + 1 < hi) {
      uint32_t mid = (lo + hi) / 2;
      uint32_t off = RestartAt(b_, mid);
      Slice k, v; uint32_t next = 0;
      std::string last;
      if (!ReadEntryAt(b_, off, &last, &k, &v, &next)) { status_ = Status::Corruption("block seek"); valid_ = false; return; }
      int c = cmp_(k, target);
      if (c < 0) lo = mid;
      else hi = mid;
    }
    last_key_.clear();
    off_ = RestartAt(b_, lo);
    ParseAtOrInvalidate();
    // linear scan within restart interval
    while (valid_ && cmp_(key_, target) < 0) Next();
  }

  void Next() override {
    if (!valid_) return;
    off_ = next_off_;
    ParseAtOrInvalidate();
  }

  bool Valid() const override { return valid_; }
  Slice key() const override { return valid_ ? key_ : Slice(); }
  Slice value() const override { return valid_ ? val_ : Slice(); }
  Status status() const override { return status_; }

private:
  void ParseAtOrInvalidate() {
    // stop before restart array region
    size_t restarts_bytes = size_t(b_.num_restarts) * 4 + 4;
    size_t limit = b_.data.n - restarts_bytes;
    if (off_ >= limit) { valid_ = false; return; }

    Slice k, v; uint32_t next = 0;
    if (!ReadEntryAt(b_, off_, &last_key_, &k, &v, &next)) {
      status_ = Status::Corruption("block: read entry");
      valid_ = false;
      return;
    }
    key_ = k;
    val_ = v;
    next_off_ = next;
    valid_ = true;
  }

  BlockView b_;
  InternalKeyComparator cmp_;
  bool valid_{false};
  Status status_{Status::OK()};

  uint32_t off_{0};
  uint32_t next_off_{0};
  std::string last_key_;
  Slice key_, val_;
};

static bool DecodeBlockHandle(Slice* in, BlockHandle* h) {
  uint64_t off=0, sz=0;
  if (!GetVarint64(in, &off)) return false;
  if (!GetVarint64(in, &sz)) return false;
  h->offset = off; h->size = sz;
  return true;
}

} // namespace

// ---------- TableReader ----------
TableReader::TableReader(std::shared_ptr<const RandomAccessFile> f, Footer footer)
  : file_(std::move(f)), footer_(footer) {}

Status TableReader::ReadBlock(const BlockHandle& h, std::string* block) const {
  return file_->Read(h.offset, size_t(h.size), block);
}

Status TableReader::Open(TableReaderOptions opt,
                         std::shared_ptr<const RandomAccessFile> file,
                         std::unique_ptr<TableReader>* out) {
  if (file->Size() < 40) return Status::Corruption("sst: too small");

  // Footer is 5*8 = 40 bytes
  std::string foot;
  Status s = file->Read(file->Size() - 40, 40, &foot);
  if (!s) return s;

  uint64_t ioff=0, isz=0, moff=0, msz=0, magic=0;
  std::memcpy(&ioff, foot.data() + 0, 8);
  std::memcpy(&isz,  foot.data() + 8, 8);
  std::memcpy(&moff, foot.data() + 16, 8);
  std::memcpy(&msz,  foot.data() + 24, 8);
  std::memcpy(&magic,foot.data() + 32, 8);

  if (opt.verify_magic && magic != kSstMagic) return Status::Corruption("sst: bad magic");

  Footer f;
  f.index = {ioff, isz};
  f.meta  = {moff, msz};

  auto tr = std::unique_ptr<TableReader>(new TableReader(file, f));

  // Load index/meta blocks
  s = tr->ReadBlock(tr->footer_.index, &tr->index_block_);
  if (!s) return s;
  s = tr->ReadBlock(tr->footer_.meta, &tr->meta_block_);
  if (!s) return s;

  // Decode bloom (meta tag 1)
  if (!tr->meta_block_.empty()) {
    Slice m(tr->meta_block_);
    uint64_t tag=0, len=0;
    if (GetVarint64(&m, &tag) && GetVarint64(&m, &len)) {
      if (tag == 1 && len <= m.n) {
        Slice bloom_bytes{m.p, size_t(len)};
        BloomBuilder::Decode(bloom_bytes, &tr->bloom_);
      }
    }
  }

  *out = std::move(tr);
  return Status::OK();
}

std::unique_ptr<Iterator> TableReader::NewIterator() const {
  // Table iterator: index iter (block) + current data block iter.
  class TableIter final : public Iterator {
  public:
    TableIter(const TableReader* tr, InternalKeyComparator cmp)
      : tr_(tr), cmp_(cmp) {
      // parse index block
      BlockView bv;
      Status s = ParseBlock(Slice(tr_->index_block_), &bv);
      status_ = s;
      if (!s) return;
      index_ = std::make_unique<BlockIter>(bv, cmp_);
    }

    void SeekToFirst() override {
      if (!status_) return;
      index_->SeekToFirst();
      LoadDataBlock();
      if (data_) data_->SeekToFirst();
      Sync();
    }

    void Seek(Slice target) override {
      if (!status_) return;
      index_->Seek(target);
      LoadDataBlock();
      if (data_) data_->Seek(target);
      Sync();
    }

    void Next() override {
      if (!Valid()) return;
      data_->Next();
      if (!data_->Valid()) {
        index_->Next();
        LoadDataBlock();
        if (data_) data_->SeekToFirst();
      }
      Sync();
    }

    bool Valid() const override { return valid_; }
    Slice key() const override { return valid_ ? key_ : Slice(); }
    Slice value() const override { return valid_ ? val_ : Slice(); }
    Status status() const override { return status_; }

  private:
    void LoadDataBlock() {
      data_.reset();
      if (!index_->Valid()) return;

      // index value is BlockHandle varint64+varint64
      Slice v = index_->value();
      Slice tmp = v;
      BlockHandle h;
      if (!DecodeBlockHandle(&tmp, &h)) { status_ = Status::Corruption("sst: bad index bh"); return; }

      std::string block;
      Status s = tr_->ReadBlock(h, &block);
      if (!s) { status_ = s; return; }
      data_storage_ = std::move(block);

      BlockView bv;
      s = ParseBlock(Slice(data_storage_), &bv);
      if (!s) { status_ = s; return; }
      data_ = std::make_unique<BlockIter>(bv, cmp_);
    }

    void Sync() {
      if (!status_) { valid_ = false; return; }
      if (!data_ || !data_->Valid()) { valid_ = false; return; }
      key_ = data_->key();
      val_ = data_->value();
      valid_ = true;
    }

    const TableReader* tr_;
    InternalKeyComparator cmp_;
    Status status_{Status::OK()};
    bool valid_{false};
    Slice key_, val_;

    std::unique_ptr<BlockIter> index_;
    std::unique_ptr<BlockIter> data_;
    std::string data_storage_;
  };

  return std::make_unique<TableIter>(this, cmp_);
}

Status TableReader::Get(Slice internal_key, std::string* value_out, ValueType* type_out) const {
  // Bloom on user key
  Slice uk; uint64_t tr = 0;
  if (DecodeInternalKey(internal_key, &uk, &tr)) {
    if (!bloom_.bits.empty() && !bloom_.MayContain(uk)) {
      return Status::NotFound("sst: bloom miss");
    }
  }

  // Use iterator seek
  auto it = NewIterator();
  it->Seek(internal_key);
  if (!it->status()) return it->status();
  if (!it->Valid()) return Status::NotFound("sst: not found");

  // Must match user key and snapshot semantics:
  Slice found = it->key();
  Slice fuk; uint64_t ftr = 0;
  if (!DecodeInternalKey(found, &fuk, &ftr)) return Status::Corruption("sst: bad key");
  if (DecodeInternalKey(internal_key, &uk, &tr)) {
    if (fuk.sv() != uk.sv()) return Status::NotFound("sst: not found");
    uint64_t snapshot = (tr >> 8);
    uint64_t fseq = (ftr >> 8);
    if (fseq > snapshot) {
      // seek landed on a newer version; advance until <= snapshot or user key changes
      while (it->Valid()) {
        found = it->key();
        if (!DecodeInternalKey(found, &fuk, &ftr)) return Status::Corruption("sst: bad key2");
        if (fuk.sv() != uk.sv()) return Status::NotFound("sst: not found");
        fseq = (ftr >> 8);
        if (fseq <= snapshot) break;
        it->Next();
      }
      if (!it->Valid()) return Status::NotFound("sst: not found");
    }
  }

  ValueType vt = ValueType(uint8_t(ftr & 0xFF));
  if (type_out) *type_out = vt;
  if (vt == ValueType::kPut) {
    if (value_out) value_out->assign(it->value().p, it->value().n);
  } else {
    if (value_out) value_out->clear();
  }
  return Status::OK();
}

Slice TableReader::SmallestKey() const { return Slice(); }
Slice TableReader::LargestKey() const { return Slice(); }
