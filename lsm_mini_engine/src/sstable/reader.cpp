#include "sstable/reader.h"

#include <algorithm>
#include <cstring>

#include "sstable/block.h"
#include "sstable/format.h"
#include "iter/internal_key.h"

namespace {

static inline int BytewiseCompare(Slice a, Slice b) {
  auto sa = a.sv();
  auto sb = b.sv();
  if (sa < sb) return -1;
  if (sa > sb) return 1;
  return 0;
}

static inline bool DecodeBlockHandleFromValue(Slice v, sstable::BlockHandle* out) {
  // varint64 off | varint64 size
  auto p = v.p;
  size_t n = v.n;

  auto get_varint64 = [&](uint64_t* dst) -> bool {
    uint64_t result = 0;
    int shift = 0;
    size_t i = 0;
    while (i < n && shift <= 63) {
      uint8_t byte = uint8_t(p[i]);
      result |= uint64_t(byte & 0x7F) << shift;
      i++;
      if ((byte & 0x80) == 0) {
        *dst = result;
        p += i; n -= i;
        return true;
      }
      shift += 7;
    }
    return false;
  };

  uint64_t off=0, sz=0;
  if (!get_varint64(&off)) return false;
  if (!get_varint64(&sz)) return false;
  out->offset = off;
  out->size = sz;
  return true;
}

class BlockIter final : public Iterator {
public:
  explicit BlockIter(std::string storage)
    : storage_(std::move(storage)) {
    status_ = sstable::ParseBlock(Slice(storage_), &view_);
    if (!status_) valid_ = false;
  }

  void SeekToFirst() override {
    if (!status_) { valid_ = false; return; }
    idx_ = 0;
    LoadCurrent();
  }

  void Seek(Slice target) override {
    if (!status_) { valid_ = false; return; }
    // binary search over entries
    uint32_t lo = 0, hi = view_.num_entries;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      Slice k, v;
      Status s = sstable::EntryAt(view_, mid, &k, &v);
      if (!s) { status_ = s; valid_ = false; return; }
      int c = BytewiseCompare(k, target);
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
    idx_ = lo;
    LoadCurrent();
  }

  void Next() override {
    if (!valid_) return;
    idx_++;
    LoadCurrent();
  }

  bool Valid() const override { return valid_; }
  Slice key() const override { return key_; }
  Slice value() const override { return val_; }
  Status status() const override { return status_; }

private:
  void LoadCurrent() {
    if (!status_) { valid_ = false; return; }
    if (idx_ >= view_.num_entries) { valid_ = false; return; }
    Status s = sstable::EntryAt(view_, idx_, &key_, &val_);
    if (!s) { status_ = s; valid_ = false; return; }
    valid_ = true;
  }

  std::string storage_;
  sstable::BlockView view_{};

  uint32_t idx_{0};
  bool valid_{false};
  Status status_{Status::OK()};
  Slice key_{}, val_{};
};

} // namespace

TableReader::TableReader(std::shared_ptr<const RandomAccessFile> f, sstable::Footer footer)
  : file_(std::move(f)), footer_(footer) {}

Status TableReader::ReadBlock(const sstable::BlockHandle& h, std::string* out) const {
  return file_->Read(h.offset, size_t(h.size), out);
}

Status TableReader::Open(TableReaderOptions opt,
                         std::shared_ptr<const RandomAccessFile> file,
                         std::unique_ptr<TableReader>* out) {
  if (file->Size() < 40) return Status::Corruption("sst: too small");

  std::string foot;
  Status s = file->Read(file->Size() - 40, 40, &foot);
  if (!s) return s;

  uint64_t ioff=0, isz=0, moff=0, msz=0, magic=0;
  std::memcpy(&ioff, foot.data() + 0, 8);
  std::memcpy(&isz,  foot.data() + 8, 8);
  std::memcpy(&moff, foot.data() + 16, 8);
  std::memcpy(&msz,  foot.data() + 24, 8);
  std::memcpy(&magic,foot.data() + 32, 8);

  if (opt.verify_magic && magic != sstable::kSstMagic) return Status::Corruption("sst: bad magic");

  sstable::Footer f;
  f.index = {ioff, isz};
  f.meta  = {moff, msz};

  auto tr = std::unique_ptr<TableReader>(new TableReader(file, f));

  // Load + parse index block
  s = tr->ReadBlock(tr->footer_.index, &tr->index_storage_);
  if (!s) return s;

  s = sstable::ParseBlock(Slice(tr->index_storage_), &tr->index_view_);
  if (!s) return s;

  *out = std::move(tr);
  return Status::OK();
}

bool TableReader::FindDataBlockForKey(Slice target, sstable::BlockHandle* h_out) const {
  // Find first index entry with key >= target
  uint32_t lo = 0, hi = index_view_.num_entries;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    Slice k, v;
    if (!sstable::EntryAt(index_view_, mid, &k, &v)) return false;
    int c = BytewiseCompare(k, target);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= index_view_.num_entries) return false;

  Slice k, v;
  if (!sstable::EntryAt(index_view_, lo, &k, &v)) return false;
  return DecodeBlockHandleFromValue(v, h_out);
}

std::unique_ptr<Iterator> TableReader::NewIterator() const {
  // Table iterator: uses index block + loads data blocks on demand.
  class TableIter final : public Iterator {
  public:
    explicit TableIter(const TableReader* tr) : tr_(tr) {}

    void SeekToFirst() override {
      status_ = Status::OK();
      idx_ = 0;
      LoadDataBlockAtIndex(idx_);
      if (data_) data_->SeekToFirst();
      Sync();
    }

    void Seek(Slice target) override {
      status_ = Status::OK();

      // binary search index for first key >= target
      uint32_t lo = 0, hi = tr_->index_view_.num_entries;
      while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        Slice k, v;
        Status s = sstable::EntryAt(tr_->index_view_, mid, &k, &v);
        if (!s) { status_ = s; valid_ = false; return; }
        int c = BytewiseCompare(k, target);
        if (c < 0) lo = mid + 1;
        else hi = mid;
      }
      idx_ = lo;
      LoadDataBlockAtIndex(idx_);
      if (data_) data_->Seek(target);
      Sync();
    }

    void Next() override {
      if (!valid_) return;
      data_->Next();
      if (!data_->Valid()) {
        idx_++;
        LoadDataBlockAtIndex(idx_);
        if (data_) data_->SeekToFirst();
      }
      Sync();
    }

    bool Valid() const override { return valid_; }
    Slice key() const override { return key_; }
    Slice value() const override { return val_; }
    Status status() const override { return status_; }

  private:
    void LoadDataBlockAtIndex(uint32_t i) {
      data_.reset();
      data_storage_.clear();
      valid_ = false;

      if (i >= tr_->index_view_.num_entries) return;

      Slice k, v;
      Status s = sstable::EntryAt(tr_->index_view_, i, &k, &v);
      if (!s) { status_ = s; return; }

      sstable::BlockHandle h;
      if (!DecodeBlockHandleFromValue(v, &h)) { status_ = Status::Corruption("sst: bad blockhandle"); return; }

      s = tr_->ReadBlock(h, &data_storage_);
      if (!s) { status_ = s; return; }

      data_ = std::make_unique<BlockIter>(data_storage_);
      if (!data_->status()) { status_ = data_->status(); data_.reset(); }
    }

    void Sync() {
      if (!status_) { valid_ = false; return; }
      if (!data_ || !data_->Valid()) { valid_ = false; return; }
      key_ = data_->key();
      val_ = data_->value();
      valid_ = true;
    }

    const TableReader* tr_;
    uint32_t idx_{0};

    std::string data_storage_;
    std::unique_ptr<BlockIter> data_;

    bool valid_{false};
    Status status_{Status::OK()};
    Slice key_{}, val_{};
  };

  return std::make_unique<TableIter>(this);
}

Status TableReader::Get(Slice internal_key, std::string* value_out, ValueType* type_out) const {
  // Find candidate block using index
  sstable::BlockHandle h;
  if (!FindDataBlockForKey(internal_key, &h)) return Status::NotFound("sst: not found");

  std::string block;
  Status s = ReadBlock(h, &block);
  if (!s) return s;

  BlockIter it(block);
  it.Seek(internal_key);
  if (!it.status()) return it.status();
  if (!it.Valid()) return Status::NotFound("sst: not found");

  // Snapshot semantics: caller passes (user_key, snapshot_seq, PUT)
  Slice target_user; uint64_t target_tr = 0;
  if (!DecodeInternalKey(internal_key, &target_user, &target_tr)) {
    // If caller didn't pass an internal key, require exact match.
    if (it.key().sv() != internal_key.sv()) return Status::NotFound("sst: not found");
    if (type_out) *type_out = ValueType::kPut;
    if (value_out) value_out->assign(it.value().p, it.value().n);
    return Status::OK();
  }
  const uint64_t snapshot_seq = (target_tr >> 8);

  // Iterate within this user key until we find seq <= snapshot.
  while (it.Valid()) {
    Slice k = it.key();
    Slice uk; uint64_t tr = 0;
    if (!DecodeInternalKey(k, &uk, &tr)) break;
    if (uk.sv() != target_user.sv()) break;

    uint64_t seq = (tr >> 8);
    if (seq <= snapshot_seq) {
      ValueType vt = ValueType(uint8_t(tr & 0xFF));
      if (type_out) *type_out = vt;
      if (vt == ValueType::kPut) {
        if (value_out) value_out->assign(it.value().p, it.value().n);
      } else {
        if (value_out) value_out->clear();
      }
      return Status::OK();
    }
    it.Next();
  }

  return Status::NotFound("sst: not found");
}
