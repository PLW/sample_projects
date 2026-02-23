// tests/lsm_engine_tests.cc

// Generated (GPT 5.2) from : prompts/test_prompt.md

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// Your headers (adjust paths if needed)
#include "sstable/varint.h"
#include "sstable/builder.h"
#include "sstable/reader.h"
#include "sstable/format.h"
#include "sstable/bloom.h"

#include "wal/wal.h"
#include "memtable/memtable.h"
#include "iter/merging_iter.h"
#include "iter/internal_key.h"
#include "iter/iterator.h"

namespace {

// ------------------------------
// In-memory file mocks
// ------------------------------
class InMemoryWritableFile final : public WritableFile {
public:
  Status Append(Slice data) override {
    buf_.append(data.p, data.n);
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { synced_ = true; return Status::OK(); }
  uint64_t Size() const override { return static_cast<uint64_t>(buf_.size()); }

  std::string& Mutable() { return buf_; }
  const std::string& Data() const { return buf_; }
  bool synced() const { return synced_; }

private:
  std::string buf_;
  bool synced_{false};
};

class InMemoryRandomAccessFile final : public RandomAccessFile {
public:
  explicit InMemoryRandomAccessFile(std::shared_ptr<std::string> data)
      : data_(std::move(data)) {}

  Status Read(uint64_t offset, size_t n, std::string* dst) const override {
    if (offset > data_->size()) return Status::IOError("offset past EOF");
    size_t avail = data_->size() - static_cast<size_t>(offset);
    size_t take = std::min(n, avail);
    dst->assign(data_->data() + offset, take);
    if (take != n) return Status::IOError("short read");
    return Status::OK();
  }

  uint64_t Size() const override { return static_cast<uint64_t>(data_->size()); }

private:
  std::shared_ptr<std::string> data_;
};

static std::shared_ptr<const RandomAccessFile> MakeRAF(const std::string& bytes) {
  return std::make_shared<InMemoryRandomAccessFile>(std::make_shared<std::string>(bytes));
}

// ------------------------------
// Tiny helpers
// ------------------------------
static std::string PadNum(int x) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06d", x);
  return std::string(buf);
}

static std::string IK(std::string_view uk, uint64_t seq, ValueType t) {
  return EncodeInternalKey(uk, seq, t);
}

static void ExpectGetOK(TableReader& tr, std::string_view user_key, uint64_t seq,
                        std::string_view expected_value) {
  std::string v;
  ValueType type = ValueType::kDel;
  Status s = tr.Get(Slice(IK(user_key, seq, ValueType::kPut)), &v, &type);
  ASSERT_TRUE(s) << s.msg;
  ASSERT_EQ(type, ValueType::kPut);
  ASSERT_EQ(v, expected_value);
}

static void ExpectGetDel(TableReader& tr, std::string_view user_key, uint64_t seq) {
  std::string v;
  ValueType type = ValueType::kPut;
  Status s = tr.Get(Slice(IK(user_key, seq, ValueType::kPut)), &v, &type);
  ASSERT_TRUE(s) << s.msg;
  ASSERT_EQ(type, ValueType::kDel);
}

static void ExpectGetNotFound(TableReader& tr, std::string_view user_key, uint64_t seq) {
  std::string v;
  ValueType type = ValueType::kPut;
  Status s = tr.Get(Slice(IK(user_key, seq, ValueType::kPut)), &v, &type);
  ASSERT_EQ(s.code, Status::kNotFound) << "Expected NotFound, got: " << s.msg;
}

} // namespace

// ============================================================
// 1) Varint roundtrip: encode/decode random u64
// ============================================================
TEST(Varint, RoundTripRandomU64) {
  std::mt19937_64 rng(12345);
  for (int i = 0; i < 20000; i++) {
    uint64_t x = rng();
    std::string buf;
    PutVarint64(buf, x);

    Slice in(buf);
    uint64_t y = 0;
    ASSERT_TRUE(GetVarint64(&in, &y));
    ASSERT_EQ(y, x);
    ASSERT_EQ(in.n, 0u); // fully consumed
  }
}

// ============================================================
// 2) Data block: write N entries → parse block → iterator seek correctness
//
// This test expects you to have a "block iterator" reachable via
// TableReader::NewIterator() correctness, so we drive it through SSTable:
// one SSTable with small blocks so multiple data blocks exist.
// ============================================================
TEST(SSTable, IteratorSeekCorrectnessAcrossManyKeys) {
  InMemoryWritableFile wf;
  SstBuilderOptions opt;
  opt.block_size = 512;          // force many blocks
  opt.restart_interval = 8;
  opt.build_bloom = true;

  SSTableBuilder b(opt, &wf);

  constexpr int N = 200;
  // Internal keys must be strictly increasing:
  // use user_key increasing, seq constant.
  for (int i = 0; i < N; i++) {
    std::string uk = "k" + PadNum(i);
    std::string ik = IK(uk, /*seq=*/1, ValueType::kPut);
    std::string val = "v" + PadNum(i);
    ASSERT_TRUE(b.Add(Slice(ik), Slice(val)));
  }
  ASSERT_TRUE(b.Finish());

  std::unique_ptr<TableReader> tr;
  TableReaderOptions ropt;
  ASSERT_TRUE(TableReader::Open(ropt, MakeRAF(wf.Data()), &tr));

  auto it = tr->NewIterator();

  // Seek exact hits
  for (int i = 0; i < N; i += 7) {
    std::string uk = "k" + PadNum(i);
    std::string target = IK(uk, 1, ValueType::kPut);
    it->Seek(Slice(target));
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(it->key().sv(), target);
    ASSERT_EQ(it->value().sv(), std::string_view("v" + PadNum(i)));
  }

  // Seek to a missing key should land on next key
  {
    std::string uk = "k" + PadNum(50);
    std::string missing = IK(std::string(uk) + "x", 1, ValueType::kPut);
    it->Seek(Slice(missing));
    ASSERT_TRUE(it->Valid());
    // Should land on k000051 (since k000050x is between k000050 and k000051)
    std::string expect_uk = "k" + PadNum(51);
    std::string expect_ik = IK(expect_uk, 1, ValueType::kPut);
    ASSERT_EQ(it->key().sv(), expect_ik);
  }

  // Seek past end => invalid
  {
    std::string past = IK("k999999", 1, ValueType::kPut);
    it->Seek(Slice(past));
    ASSERT_FALSE(it->Valid());
  }

  ASSERT_TRUE(it->status());
}

// ============================================================
// 3) Footer: corrupt magic → reader rejects.
// ============================================================
TEST(SSTable, FooterMagicCorruptionRejected) {
  InMemoryWritableFile wf;
  SstBuilderOptions opt;
  SSTableBuilder b(opt, &wf);

  ASSERT_TRUE(b.Add(Slice(IK("a", 1, ValueType::kPut)), Slice("va")));
  ASSERT_TRUE(b.Finish());

  // Corrupt last 8 bytes (magic)
  std::string corrupted = wf.Data();
  ASSERT_GE(corrupted.size(), 8u);
  for (size_t i = 0; i < 8; i++) corrupted[corrupted.size() - 1 - i] ^= char(0xFF);

  std::unique_ptr<TableReader> tr;
  TableReaderOptions ropt;
  ropt.verify_magic = true;

  Status s = TableReader::Open(ropt, MakeRAF(corrupted), &tr);
  ASSERT_EQ(s.code, Status::kCorruption) << "Expected Corruption, got: " << s.msg;
}

// ============================================================
// 4) SSTable Get(): build table with 1–3 blocks → verify lookups
//    with seeks across block boundaries.
// ============================================================
TEST(SSTable, GetAcrossBlockBoundaries) {
  InMemoryWritableFile wf;
  SstBuilderOptions opt;
  opt.block_size = 256;          // force multiple blocks with modest entries
  opt.restart_interval = 4;
  opt.build_bloom = true;

  SSTableBuilder b(opt, &wf);

  // Carefully choose values to exceed a block.
  std::vector<std::pair<std::string,std::string>> kvs;
  for (int i = 0; i < 60; i++) {
    std::string uk = "key" + PadNum(i);
    std::string val = std::string(40, char('A' + (i % 26))) + PadNum(i);
    kvs.push_back({uk, val});
  }
  std::sort(kvs.begin(), kvs.end());

  for (auto& [uk, val] : kvs) {
    ASSERT_TRUE(b.Add(Slice(IK(uk, 7, ValueType::kPut)), Slice(val)));
  }
  ASSERT_TRUE(b.Finish());

  std::unique_ptr<TableReader> tr;
  ASSERT_TRUE(TableReader::Open(TableReaderOptions{}, MakeRAF(wf.Data()), &tr));

  // Hit some keys near likely block edges.
  ExpectGetOK(*tr, "key000000", 7, kvs[0].second);
  ExpectGetOK(*tr, "key000017", 7, kvs[17].second);
  ExpectGetOK(*tr, "key000031", 7, kvs[31].second);
  ExpectGetOK(*tr, "key000059", 7, kvs[59].second);

  // Missing key
  ExpectGetNotFound(*tr, "key999999", 7);
}

// ============================================================
// 5) MergingIterator: merge 3 child iterators; verify global order.
// ============================================================
namespace {

class VecIter final : public Iterator {
public:
  explicit VecIter(std::vector<std::pair<std::string,std::string>> kv)
      : kv_(std::move(kv)) {}

  void SeekToFirst() override { i_ = 0; }
  void Seek(Slice target) override {
    auto t = std::string(target.sv());
    i_ = std::lower_bound(kv_.begin(), kv_.end(), t,
      [](auto& a, const std::string& key){ return a.first < key; }) - kv_.begin();
  }
  void Next() override { if (i_ < kv_.size()) i_++; }

  bool Valid() const override { return i_ < kv_.size(); }
  Slice key() const override { return Valid() ? Slice(kv_[i_].first) : Slice(); }
  Slice value() const override { return Valid() ? Slice(kv_[i_].second) : Slice(); }
  Status status() const override { return Status::OK(); }

private:
  std::vector<std::pair<std::string,std::string>> kv_;
  size_t i_{0};
};

} // namespace

TEST(Iter, MergingIteratorGlobalOrder) {
  InternalKeyComparator cmp;

  auto a = std::make_unique<VecIter>(std::vector<std::pair<std::string,std::string>>{
    {IK("a", 3, ValueType::kPut), "va3"},
    {IK("c", 1, ValueType::kPut), "vc1"},
    {IK("e", 9, ValueType::kPut), "ve9"},
  });

  auto b = std::make_unique<VecIter>(std::vector<std::pair<std::string,std::string>>{
    {IK("b", 2, ValueType::kPut), "vb2"},
    {IK("d", 4, ValueType::kPut), "vd4"},
  });

  auto c = std::make_unique<VecIter>(std::vector<std::pair<std::string,std::string>>{
    {IK("a", 1, ValueType::kPut), "va1"},
    {IK("f", 7, ValueType::kPut), "vf7"},
  });

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::move(a));
  children.push_back(std::move(b));
  children.push_back(std::move(c));

  MergingIterator mit(cmp, std::move(children));
  mit.SeekToFirst();

  // Expect order by (user asc, seq desc):
  // for "a": seq 3 then seq 1
  std::vector<std::string> got;
  while (mit.Valid()) {
    got.push_back(std::string(mit.key().sv()));
    mit.Next();
  }

  std::vector<std::string> expect = {
    IK("a",3,ValueType::kPut),
    IK("a",1,ValueType::kPut),
    IK("b",2,ValueType::kPut),
    IK("c",1,ValueType::kPut),
    IK("d",4,ValueType::kPut),
    IK("e",9,ValueType::kPut),
    IK("f",7,ValueType::kPut),
  };
  ASSERT_EQ(got, expect);
}

// ============================================================
// 6) MVCC visibility: put(k,v1) seq1; put(k,v2) seq2; del(k) seq3
//    check reads at snapshots seq1/seq2/seq3.
//
// This test targets MemTable::Get(user_key, snapshot_seq,...)
// ============================================================
TEST(MVCC, MemTableVisibilityAcrossSnapshots) {
  MemTable mem(MemOptions{});
  ASSERT_TRUE(mem.Put(1, Slice("k"), Slice("v1")));
  ASSERT_TRUE(mem.Put(2, Slice("k"), Slice("v2")));
  ASSERT_TRUE(mem.Del(3, Slice("k")));

  std::string v;
  ValueType t = ValueType::kPut;

  // snapshot at seq1 -> v1
  ASSERT_TRUE(mem.Get(Slice("k"), 1, &v, &t));
  ASSERT_EQ(t, ValueType::kPut);
  ASSERT_EQ(v, "v1");

  // snapshot at seq2 -> v2
  ASSERT_TRUE(mem.Get(Slice("k"), 2, &v, &t));
  ASSERT_EQ(t, ValueType::kPut);
  ASSERT_EQ(v, "v2");

  // snapshot at seq3 -> deleted
  ASSERT_TRUE(mem.Get(Slice("k"), 3, &v, &t));
  ASSERT_EQ(t, ValueType::kDel);

  // snapshot after delete -> deleted
  ASSERT_TRUE(mem.Get(Slice("k"), 100, &v, &t));
  ASSERT_EQ(t, ValueType::kDel);
}

// ============================================================
// 7) Recovery: write WAL + crash simulation; replay into memtable; flush; validate.
//
// This uses WalWriter/WalReader and then flushes the recovered memtable into an SSTable.
// ============================================================
TEST(WAL, RecoveryReplayThenFlushAndRead) {
  // "Crash" simulation: keep WAL bytes, throw away memtable.
  auto wal_file = std::make_shared<std::string>();
  InMemoryWritableFile wal_wf;
  WalWriter ww(&wal_wf);

  // Write a few ops
  ASSERT_TRUE(ww.AppendPut(1, Slice("a"), Slice("va1")));
  ASSERT_TRUE(ww.AppendPut(2, Slice("b"), Slice("vb2")));
  ASSERT_TRUE(ww.AppendPut(3, Slice("a"), Slice("va3"))); // overwrite
  ASSERT_TRUE(ww.AppendDel(4, Slice("b")));               // delete
  ASSERT_TRUE(ww.Sync());

  *wal_file = wal_wf.Data();

  // Recovery: new memtable, replay WAL
  MemTable recovered(MemOptions{});
  WalReader wr(std::make_shared<InMemoryRandomAccessFile>(wal_file));
  uint64_t max_seq = 0;
  ASSERT_TRUE(wr.Replay(&recovered, &max_seq));
  ASSERT_EQ(max_seq, 4u);

  // Validate recovered state via memtable MVCC reads
  std::string v;
  ValueType t = ValueType::kPut;

  ASSERT_TRUE(recovered.Get(Slice("a"), 2, &v, &t));  // before overwrite
  ASSERT_EQ(t, ValueType::kPut);
  ASSERT_EQ(v, "va1");

  ASSERT_TRUE(recovered.Get(Slice("a"), 3, &v, &t));  // after overwrite
  ASSERT_EQ(t, ValueType::kPut);
  ASSERT_EQ(v, "va3");

  ASSERT_TRUE(recovered.Get(Slice("b"), 3, &v, &t));  // before delete
  ASSERT_EQ(t, ValueType::kPut);
  ASSERT_EQ(v, "vb2");

  ASSERT_TRUE(recovered.Get(Slice("b"), 4, &v, &t));  // deleted
  ASSERT_EQ(t, ValueType::kDel);

  // Flush recovered memtable into an SSTable (in-memory file)
  // We need an iterator over internal keys in sorted order.
  auto it = recovered.NewIterator();
  it->SeekToFirst();

  InMemoryWritableFile sst_wf;
  SstBuilderOptions opt;
  opt.block_size = 512;
  opt.restart_interval = 8;
  opt.build_bloom = true;

  SSTableBuilder sb(opt, &sst_wf);
  for (; it->Valid(); it->Next()) {
    ASSERT_TRUE(sb.Add(it->key(), it->value()));
  }
  ASSERT_TRUE(sb.Finish());

  // Open table and read at snapshot seqs using internal-key lookups
  std::unique_ptr<TableReader> tr;
  ASSERT_TRUE(TableReader::Open(TableReaderOptions{}, MakeRAF(sst_wf.Data()), &tr));

  ExpectGetOK(*tr, "a", 2, "va1");
  ExpectGetOK(*tr, "a", 3, "va3");
  ExpectGetOK(*tr, "b", 3, "vb2");
  ExpectGetDel(*tr, "b", 4);
}


