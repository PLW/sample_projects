## 1) Common types

### `Status`, `Slice`, helpers

```cpp
// iter/iterator.h
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

struct Status {
  enum Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArg } code{kOk};
  std::string msg;
  static Status OK() { return {}; }
  static Status NotFound(std::string m="") { return {kNotFound, std::move(m)}; }
  static Status Corruption(std::string m) { return {kCorruption, std::move(m)}; }
  static Status IOError(std::string m) { return {kIOError, std::move(m)}; }
  static Status Invalid(std::string m) { return {kInvalidArg, std::move(m)}; }
  explicit operator bool() const { return code == kOk; }
};

struct Slice {
  const char* p{nullptr};
  size_t n{0};
  Slice() = default;
  Slice(const char* p_, size_t n_) : p(p_), n(n_) {}
  Slice(std::string_view sv) : p(sv.data()), n(sv.size()) {}
  std::string_view sv() const { return {p, n}; }
};
```

---

## 2) Internal keys + comparator

### Spec (internal key)

`InternalKey = user_key || pack64(seq<<8 | kind)`

* `kind`: `1=PUT`, `0=DEL`
* Internal sort: `(user_key asc, seq desc, kind desc)` (seq desc achieved by packing and comparing the 8-byte trailer descending)

```cpp
// iter/internal_key.h
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
```

---

## 3) Varints + fixed32/64 encoding

### Binary encoding spec (easy + testable)

* `PutFixed32/64`: little-endian
* `PutVarint32/64`: standard 7-bit continuation
* `GetVarint...` returns bytes consumed

```cpp
// sstable/varint.h
#pragma once
#include <cstdint>
#include <string>
#include "iter/iterator.h"

inline void PutFixed32(std::string& dst, uint32_t v) {
  char b[4]; std::memcpy(b, &v, 4); dst.append(b, 4);
}
inline void PutFixed64(std::string& dst, uint64_t v) {
  char b[8]; std::memcpy(b, &v, 8); dst.append(b, 8);
}

inline void PutVarint32(std::string& dst, uint32_t v) {
  while (v >= 128) { dst.push_back(char((v & 0x7F) | 0x80)); v >>= 7; }
  dst.push_back(char(v));
}
inline void PutVarint64(std::string& dst, uint64_t v) {
  while (v >= 128) { dst.push_back(char((v & 0x7F) | 0x80)); v >>= 7; }
  dst.push_back(char(v));
}

inline bool GetVarint64(Slice* in, uint64_t* out) {
  uint64_t result = 0;
  int shift = 0;
  const char* p = in->p;
  size_t n = in->n;
  for (size_t i = 0; i < n && shift <= 63; i++) {
    uint8_t byte = uint8_t(p[i]);
    result |= uint64_t(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      in->p += i + 1; in->n -= i + 1;
      return true;
    }
    shift += 7;
  }
  return false;
}
```

---

## 4) SSTable format spec

### File layout

[DataBlock0][DataBlock1]...[IndexBlock][MetaBlock][Footer]

### DataBlock encoding

* Header-less block with entries:

  * `shared_prefix_len` (varint32)
  * `unshared_key_len` (varint32)
  * `value_len` (varint32)  // 0 allowed; tombstone is via ValueType in internal key trailer, not value_len
  * `unshared_key_bytes`
  * `value_bytes`
* Restart array at end:

  * `restart_offset[i]` fixed32 (offset within block)
  * `num_restarts` fixed32

Restart interval `R` (e.g., 16). Every R entries, `shared_prefix_len=0`, and restart_offset records that entry.

### IndexBlock encoding

Same “block encoding” as a data block, but values are:

* `BlockHandle`: `{offset varint64, size varint64}`

### MetaBlock (minimal)

* `bloom_filter`: raw bytes + k + num_bits (simple to parse)
* `properties`: optional kvs (can skip initially)

### Footer (fixed 48 bytes)

* `index_handle_offset` fixed64
* `index_handle_size` fixed64
* `meta_handle_offset` fixed64
* `meta_handle_size` fixed64
* `magic` fixed64 = `0x4C534D535354424CL` (“LSMSSTBL”)

```cpp
// sstable/format.h
#pragma once
#include <cstdint>

constexpr uint64_t kSstMagic = 0x4C534D535354424CL; // "LSMSSTBL"

struct BlockHandle { uint64_t offset{0}, size{0}; };

struct Footer {
  BlockHandle index;
  BlockHandle meta;
};
```

---

## 5) SSTable builder API

```cpp
// sstable/builder.h
#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include "iter/iterator.h"
#include "sstable/format.h"
#include "sstable/varint.h"
#include "iter/internal_key.h"

struct SstBuilderOptions {
  size_t block_size = 16 * 1024;
  int restart_interval = 16;
  bool build_bloom = true;
  int bloom_k = 7;
};

class WritableFile {
public:
  virtual ~WritableFile() = default;
  virtual Status Append(Slice data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual uint64_t Size() const = 0;
};

class SSTableBuilder {
public:
  SSTableBuilder(SstBuilderOptions opt, WritableFile* out);

  // Keys MUST be added in strictly increasing InternalKeyComparator order.
  Status Add(Slice internal_key, Slice value);

  Status Finish();   // writes remaining data block, index, meta, footer
  void Abandon();    // best-effort cleanup

  uint64_t FileSize() const;

private:
  Status FlushDataBlock();

  SstBuilderOptions opt_;
  WritableFile* out_;
  InternalKeyComparator cmp_;

  std::string last_key_;
  std::string data_block_;
  std::vector<uint32_t> restarts_;
  int entries_since_restart_{0};

  // index entries: separator_key -> BlockHandle
  std::string index_block_;        // encoded as a block too
  std::vector<uint32_t> index_restarts_;
  int index_entries_since_restart_{0};
  std::string pending_index_key_;
  BlockHandle pending_handle_;
  bool has_pending_index_{false};

  // bloom building: add user_key or internal_key (pick user_key)
  // (implementation in bloom.h)
  std::vector<uint32_t> bloom_hashes_;

  uint64_t offset_{0};
  bool finished_{false};
};
```

**Key behaviors**

* `Add()` appends entry to current data block (prefix-compressed). If block would exceed target, `FlushDataBlock()`.
* When flushing a data block, record its `BlockHandle`, and create one **index entry** (use last key in block or a short separator).
* `Finish()` writes index block + meta + footer.

---

## 6) SSTable reader API

```cpp
// sstable/reader.h
#pragma once
#include <memory>
#include <vector>
#include "iter/iterator.h"
#include "sstable/format.h"
#include "iter/internal_key.h"

class RandomAccessFile {
public:
  virtual ~RandomAccessFile() = default;
  virtual Status Read(uint64_t offset, size_t n, std::string* dst) const = 0;
  virtual uint64_t Size() const = 0;
};

struct TableReaderOptions {
  bool use_mmap = true;    // optional
  bool verify_magic = true;
};

class Iterator {
public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void Seek(Slice target) = 0;
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;
  virtual Status status() const = 0;
};

class TableReader {
public:
  static Status Open(TableReaderOptions opt,
                     std::shared_ptr<const RandomAccessFile> file,
                     std::unique_ptr<TableReader>* out);

  std::unique_ptr<Iterator> NewIterator() const;

  // Point lookup (fast path: bloom -> index -> data block)
  Status Get(Slice internal_key, std::string* value_out, ValueType* type_out) const;

  Slice SmallestKey() const;
  Slice LargestKey() const;

private:
  TableReader(std::shared_ptr<const RandomAccessFile> f, Footer footer);

  Status ReadFooter();
  Status ReadBlock(const BlockHandle& h, std::string* block) const;

  std::shared_ptr<const RandomAccessFile> file_;
  Footer footer_;
  InternalKeyComparator cmp_;

  // cached blocks (simple version: keep index/meta in memory)
  std::string index_block_;
  std::string meta_block_;

  // bloom filter decoded
  // (implementation-defined: store bits + k)
};
```

**Iterator behavior**

* The table iterator uses:

  * an **index iterator** over index_block
  * a **data-block iterator** for the currently selected data block
  * `Seek(target)` does: `index.Seek(target)`, load that block, `data.Seek(target)`.

---

## 7) Bloom filter API (simple)

Use a standard Bloom:

* store `num_bits`, `k`, then bitset bytes
* hash: `h1=xxhash32(key)`, `h2=rotl(h1, 15) * 0x9e3779b1` (or any stable second hash)
* i-th probe: `(h1 + i*h2) % num_bits`

```cpp
// sstable/bloom.h
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
```

---

## 8) WAL append + recovery

WAL record format (minimal, robust):

[crc32 fixed32][len varint32][type u8][payload bytes]

Types: PUT/DEL (payload includes key/value and sequence).

```cpp
// wal/wal.h
#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include "iter/iterator.h"
#include "iter/internal_key.h"

struct WalRecord {
  ValueType type;
  uint64_t seq;
  std::string user_key;
  std::string value; // empty for DEL
};

class WalWriter {
public:
  explicit WalWriter(WritableFile* f) : f_(f) {}
  Status AppendPut(uint64_t seq, Slice user_key, Slice value);
  Status AppendDel(uint64_t seq, Slice user_key);
  Status Sync() { return f_->Sync(); }
private:
  Status AppendRecord(const WalRecord& r);
  WritableFile* f_;
};

class WalReader {
public:
  explicit WalReader(std::shared_ptr<const RandomAccessFile> f) : f_(std::move(f)) {}
  Status Replay(class MemTable* mem, uint64_t* max_seq_out);
private:
  std::shared_ptr<const RandomAccessFile> f_;
};
```

Recovery:

* scan WAL sequentially; validate crc; apply to memtable; track max seq; then continue normal operation.

---

## 9) Memtable (mutable + frozen)

Use a skiplist or `std::map` first.

```cpp
// memtable/memtable.h
#pragma once
#include <map>
#include <memory>
#include <shared_mutex>
#include "iter/iterator.h"
#include "iter/internal_key.h"

struct MemOptions {
  size_t max_bytes = 64 * 1024 * 1024;
};

class MemTable {
public:
  explicit MemTable(MemOptions opt) : opt_(opt) {}

  Status Put(uint64_t seq, Slice user_key, Slice value);
  Status Del(uint64_t seq, Slice user_key);

  // Lookup by *user key*; return newest visible entry in this memtable.
  Status Get(Slice user_key, uint64_t snapshot_seq,
             std::string* value_out, ValueType* type_out) const;

  size_t ApproxBytes() const { return approx_bytes_; }

  // Turn into an immutable view for readers and flushing.
  std::shared_ptr<const MemTable> Freeze() const;

  std::unique_ptr<Iterator> NewIterator() const;

private:
  MemOptions opt_;
  mutable std::shared_mutex mu_;
  std::map<std::string, std::string, std::function<bool(const std::string&, const std::string&)>> map_;
  size_t approx_bytes_{0};
};
```

Implementation notes:

* Store **internal key** as the map key, and value bytes as map value.
* `Get(user_key, snapshot_seq)` constructs a lookup key: `EncodeInternalKey(user_key, snapshot_seq, kPut)` and seeks lower_bound to find newest `<= snapshot_seq` for that user_key.

---

## 10) Version / Manifest (atomic publish)

**Goal:** reads take a `Version` snapshot that lists current SSTables (and optional frozen memtables).

```cpp
// version/manifest.h
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "iter/iterator.h"
#include "sstable/reader.h"

struct FileMeta {
  uint64_t file_number;
  uint64_t file_size;
  std::string smallest; // internal key
  std::string largest;  // internal key
  std::shared_ptr<TableReader> table; // lazily opened ok too
};

struct Version {
  uint64_t generation{0};
  std::vector<FileMeta> l0;     // simplest: only one level
  // If you add levels later: std::vector<std::vector<FileMeta>> levels;

  uint64_t last_sequence{0};
};

class VersionSet {
public:
  VersionSet() = default;

  std::shared_ptr<const Version> Current() const { return current_.load(); }

  // publish a new immutable version (copy-on-write)
  void Publish(std::shared_ptr<const Version> v) { current_.store(std::move(v)); }

  // Append-only manifest (optional initially): record edits then rebuild at open.
  Status LogAndApply(/*VersionEdit*/);

private:
  std::atomic<std::shared_ptr<const Version>> current_{std::make_shared<Version>()};
};
```

For the “atomic publish” requirement, `atomic<shared_ptr>` snapshotting is enough. If you later want crash-safe: implement manifest logging + fsync.

---

## 11) Merging iterator (k-way merge)

```cpp
// iter/merging_iter.h
#pragma once
#include <queue>
#include <vector>
#include <memory>
#include "iter/iterator.h"
#include "iter/internal_key.h"

class MergingIterator : public Iterator {
public:
  MergingIterator(InternalKeyComparator cmp,
                  std::vector<std::unique_ptr<Iterator>> children);

  void Seek(Slice target) override;
  void SeekToFirst() override;
  void Next() override;

  bool Valid() const override { return valid_; }
  Slice key() const override { return key_; }
  Slice value() const override { return value_; }
  Status status() const override { return status_; }

private:
  struct HeapItem {
    size_t idx;
    Slice key;
  };
  struct HeapCmp {
    InternalKeyComparator cmp;
    bool operator()(const HeapItem& a, const HeapItem& b) const {
      return cmp(a.key, b.key) > 0; // min-heap via priority_queue invert
    }
  };

  void RebuildTop();
  void AdvanceTop();

  InternalKeyComparator cmp_;
  std::vector<std::unique_ptr<Iterator>> children_;
  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCmp> heap_;

  bool valid_{false};
  Slice key_, value_;
  Status status_{Status::OK()};
};
```

**Higher-level “user-visible iterator”** usually wraps `MergingIterator` and **collapses versions**:

* compare user keys
* pick highest-seq entry ≤ snapshot_seq
* skip tombstones

That wrapper is where you enforce MVCC visibility and dedup.

---

## 12) Compaction API (minimal L0 merge)

```cpp
// compaction/compaction.h
#pragma once
#include <vector>
#include <memory>
#include "version/manifest.h"
#include "sstable/builder.h"
#include "iter/merging_iter.h"

struct CompactionPlan {
  std::vector<FileMeta> inputs;     // pick N newest or overlapping
  uint64_t output_file_number;
  size_t target_file_size = 64 * 1024 * 1024;
};

class Compactor {
public:
  Status RunL0Compaction(const CompactionPlan& plan,
                         VersionSet* versions,
                         /*file env*/);

  // selection policy
  static CompactionPlan PickL0(const Version& v, size_t l0_trigger = 8);

private:
  // build output SSTables from merged iter, dropping shadowed keys, tombstones if safe
};
```

---

## 13) Top-level DB API (tying it together)

```cpp
// db/db.h
#pragma once
#include <memory>
#include <mutex>
#include "wal/wal.h"
#include "memtable/memtable.h"
#include "version/manifest.h"

struct DBOptions {
  MemOptions mem;
  SstBuilderOptions sst;
  size_t mem_flush_threshold_bytes = 64 * 1024 * 1024;
  bool use_wal = true;
};

class DB {
public:
  static Status Open(DBOptions opt, std::string dbdir, std::unique_ptr<DB>* out);

  Status Put(Slice key, Slice value);
  Status Del(Slice key);
  Status Get(Slice key, std::string* value_out);

  // Range scan
  std::unique_ptr<Iterator> NewIterator();

private:
  Status MaybeFlush();
  Status FlushMemtable(std::shared_ptr<const MemTable> frozen);

  DBOptions opt_;
  std::string dir_;

  std::mutex mu_;
  uint64_t next_file_number_{1};
  uint64_t last_seq_{0};

  std::unique_ptr<WalWriter> wal_;
  std::unique_ptr<MemTable> mem_;
  std::vector<std::shared_ptr<const MemTable>> immutables_; // flushing

  VersionSet versions_;
};
```

---

## 14) Testing checklist (fast confidence)

1. **Varint roundtrip**: encode/decode random u64.
2. **Data block**: write N entries → parse block → iterator seek correctness.
3. **Footer**: corrupt magic → reader rejects.
4. **SSTable Get()**: build table with 1–3 blocks → verify point lookups with seeks across block boundaries.
5. **MergingIterator**: merge 3 child iterators; verify global order.
6. **MVCC visibility**: put(k,v1) seq1; put(k,v2) seq2; del(k) seq3; check reads at snapshots seq1/seq2/seq3.
7. **Recovery**: write WAL + crash simulation; replay into memtable; flush; validate.

---

If you want the next step, I can provide:

* a concrete **BlockIterator** implementation (restart array seek)
* a minimal **SSTableBuilder::Add / Finish** and **TableReader::Get / NewIterator**
* a tiny “Env” abstraction for files (posix-backed) so the sketches compile and run end-to-end.
