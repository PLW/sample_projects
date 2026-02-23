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
