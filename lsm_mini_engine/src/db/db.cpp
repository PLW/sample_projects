#include "db/db.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "sstable/builder.h"
#include "sstable/reader.h"
#include "wal/wal.h"
#include "iter/internal_key.h"

Status DB::Open(DBOptions opt, std::string dbdir, std::unique_ptr<DB>* out) {
  auto db = std::unique_ptr<DB>(new DB());
  db->opt_ = opt;
  db->dir_ = std::move(dbdir);

  db->owned_env_ = std::make_unique<PosixEnv>();
  db->env_ = db->owned_env_.get();

  Status s = db->env_->CreateDirIfMissing(db->dir_);
  if (!s) return s;

  // Recover MANIFEST (or create new)
  s = db->versions_.Recover(db->env_, db->dir_);
  if (!s) return s;

  // Open existing SSTables referenced by Version (lazy-open is fine; do eager here)
  {
    auto cur = std::make_shared<Version>(*db->versions_.Current());
    for (auto& fm : cur->l0) {
      std::shared_ptr<const RandomAccessFile> raf;
      s = db->env_->NewRandomAccessFile(db->TablePath(fm.file_number), &raf);
      if (!s) return s;
      std::unique_ptr<TableReader> tr;
      s = TableReader::Open(TableReaderOptions{}, raf, &tr);
      if (!s) return s;
      fm.table = std::shared_ptr<TableReader>(tr.release());
    }
    db->versions_.Publish(cur);
  }

  // Open WAL (new file if missing)
  db->mem_ = std::make_unique<MemTable>(opt.mem);

  // Replay WAL if exists
  s = db->RecoverWAL();
  if (!s) return s;

  // Open WAL writer (truncate WAL after replay for mini-engine)
  std::unique_ptr<WritableFile> wwf;
  s = db->env_->NewWritableFile(db->WalPath(), &wwf);
  if (!s) return s;
  db->wal_ = std::make_unique<WalWriter>(wwf.release()); // WalWriter stores raw pointer per earlier sketch

  *out = std::move(db);
  return Status::OK();
}

Status DB::RecoverWAL() {
  bool ex=false;
  Status s = env_->FileExists(WalPath(), &ex);
  if (!s) return s;
  if (!ex) return Status::OK();

  std::shared_ptr<const RandomAccessFile> rf;
  s = env_->NewRandomAccessFile(WalPath(), &rf);
  if (!s) return s;

  WalReader wr(rf);
  uint64_t max_seq = 0;
  s = wr.Replay(mem_.get(), &max_seq);
  if (!s) return s;

  last_seq_ = std::max(last_seq_, max_seq);

  // Update manifest last_sequence so snapshots reflect recovered seq
  VersionEdit e;
  e.set_last_sequence = last_seq_;
  auto cur = versions_.Current();
  e.set_next_file_number = cur->next_file_number;
  return versions_.LogAndApply(env_, dir_, e);
}

Status DB::MaybeFlush() {
  if (mem_->ApproxBytes() < opt_.mem_flush_threshold_bytes) return Status::OK();

  auto frozen = mem_->Freeze();
  immutables_.push_back(frozen);
  mem_ = std::make_unique<MemTable>(opt_.mem);

  // Flush immediately (single-thread mini-engine)
  Status s = FlushFrozenToSST(frozen);
  if (!s) return s;

  // remove from immutables_
  immutables_.erase(std::remove(immutables_.begin(), immutables_.end(), frozen), immutables_.end());
  return Status::OK();
}

Status DB::FlushFrozenToSST(std::shared_ptr<const MemTable> frozen) {
  auto cur = versions_.Current();
  uint64_t file_no = cur->next_file_number;
  uint64_t next_file_no = file_no + 1;

  std::unique_ptr<WritableFile> wf;
  Status s = env_->NewWritableFile(TablePath(file_no), &wf);
  if (!s) return s;

  SSTableBuilder sb(opt_.sst, wf.get());

  auto it = frozen->NewIterator();
  it->SeekToFirst();

  std::string smallest, largest;
  bool first = true;

  for (; it->Valid(); it->Next()) {
    if (first) { smallest.assign(it->key().p, it->key().n); first = false; }
    largest.assign(it->key().p, it->key().n);
    s = sb.Add(it->key(), it->value());
    if (!s) return s;
  }
  s = sb.Finish();
  if (!s) return s;

  uint64_t fsz = wf->Size();
  s = wf->Sync();
  if (!s) return s;

  // Open reader for new table
  std::shared_ptr<const RandomAccessFile> raf;
  s = env_->NewRandomAccessFile(TablePath(file_no), &raf);
  if (!s) return s;
  std::unique_ptr<TableReader> tr;
  s = TableReader::Open(TableReaderOptions{}, raf, &tr);
  if (!s) return s;

  FileMeta fm;
  fm.file_number = file_no;
  fm.file_size = fsz;
  fm.smallest = std::move(smallest);
  fm.largest = std::move(largest);
  fm.table = std::shared_ptr<TableReader>(tr.release());

  VersionEdit e;
  e.add_l0.push_back(fm);
  e.set_next_file_number = next_file_no;
  e.set_last_sequence = last_seq_;

  return versions_.LogAndApply(env_, dir_, e);
}
