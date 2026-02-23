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
