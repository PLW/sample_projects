#include "db/db.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "iter/internal_key.h"
#include "iter/merging_iter.h"

Status DB::Open(DBOptions opt, std::string dbdir, std::unique_ptr<DB>* out) {
  auto db = std::unique_ptr<DB>(new DB());
  db->opt_ = opt;
  db->dir_ = std::move(dbdir);
  db->mem_ = std::make_unique<MemTable>(opt.mem);

  // TODO: open WAL file + recover:
  // - read MANIFEST to get tables
  // - read WAL and replay into memtable
  // For tests, you can construct DB without these.

  *out = std::move(db);
  return Status::OK();
}

Status DB::Put(Slice key, Slice value) {
  std::lock_guard lk(mu_);
  uint64_t seq = ++last_seq_;

  if (opt_.use_wal && wal_) {
    Status s = wal_->AppendPut(seq, key, value);
    if (!s) return s;
  }
  Status s = mem_->Put(seq, key, value);
  if (!s) return s;

  return MaybeFlush();
}

Status DB::Del(Slice key) {
  std::lock_guard lk(mu_);
  uint64_t seq = ++last_seq_;

  if (opt_.use_wal && wal_) {
    Status s = wal_->AppendDel(seq, key);
    if (!s) return s;
  }
  Status s = mem_->Del(seq, key);
  if (!s) return s;

  return MaybeFlush();
}

Status DB::Get(Slice key, std::string* value_out) {
  // Take snapshot seq
  std::shared_ptr<const Version> ver = versions_.Current();
  uint64_t snapshot = ver->last_sequence;
  {
    std::lock_guard lk(mu_);
    snapshot = std::max(snapshot, last_seq_);
  }

  // 1) memtable
  {
    std::string v;
    ValueType t = ValueType::kPut;
    Status s = mem_->Get(key, snapshot, &v, &t);
    if (s && t == ValueType::kPut) { *value_out = std::move(v); return Status::OK(); }
    if (s && t == ValueType::kDel) return Status::NotFound("deleted");
  }

  // 2) immutable memtables (if you keep them)
  for (auto& imm : immutables_) {
    std::string v;
    ValueType t = ValueType::kPut;
    Status s = imm->Get(key, snapshot, &v, &t);
    if (s && t == ValueType::kPut) { *value_out = std::move(v); return Status::OK(); }
    if (s && t == ValueType::kDel) return Status::NotFound("deleted");
  }

  // 3) SSTables newest->oldest (L0 order as stored)
  std::string ik = EncodeInternalKey(key.sv(), snapshot, ValueType::kPut);
  for (auto it = ver->l0.rbegin(); it != ver->l0.rend(); ++it) {
    if (!it->table) continue;
    std::string v;
    ValueType t = ValueType::kPut;
    Status s = it->table->Get(Slice(ik), &v, &t);
    if (!s) continue;
    if (t == ValueType::kPut) { *value_out = std::move(v); return Status::OK(); }
    return Status::NotFound("deleted");
  }

  return Status::NotFound("not found");
}

std::unique_ptr<Iterator> DB::NewIterator() {
  // Merge mem + sst iterators (no MVCC collapsing wrapper here yet).
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem_->NewIterator());
  for (auto& imm : immutables_) children.push_back(imm->NewIterator());

  auto ver = versions_.Current();
  for (auto& f : ver->l0) {
    if (f.table) children.push_back(f.table->NewIterator());
  }
  return std::make_unique<MergingIterator>(InternalKeyComparator{}, std::move(children));
}

Status DB::MaybeFlush() {
  if (mem_->ApproxBytes() < opt_.mem_flush_threshold_bytes) return Status::OK();

  // Freeze
  auto frozen = mem_->Freeze();
  immutables_.push_back(frozen);

  // New mutable memtable
  mem_ = std::make_unique<MemTable>(opt_.mem);

  // TODO: flush frozen to new SSTable file via Env, then publish new Version.
  // For now, this is a hook point.
  return Status::OK();
}

Status DB::FlushMemtable(std::shared_ptr<const MemTable> /*frozen*/) {
  return Status::Invalid("db: FlushMemtable env integration not implemented");
}
