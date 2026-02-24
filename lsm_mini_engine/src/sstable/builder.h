#pragma once
#include <cstdint>
#include <string>

#include "iter/iterator.h"
#include "sstable/format.h"
#include "sstable/block.h"

// Reuse your existing file abstraction.
class WritableFile {
public:
  virtual ~WritableFile() = default;
  virtual Status Append(Slice data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual uint64_t Size() const = 0;
};

struct SstBuilderOptions {
  size_t block_size = 16 * 1024;

  // kept for compatibility with earlier code/tests; unused for now
  int restart_interval = 16;
  bool build_bloom = false;
  int bloom_k = 7;
};

class SSTableBuilder {
public:
  SSTableBuilder(SstBuilderOptions opt, WritableFile* out);

  // Keys MUST be added in strictly increasing bytewise order.
  Status Add(Slice internal_key, Slice value);

  Status Finish();
  void Abandon();
  uint64_t FileSize() const { return offset_; }

private:
  Status FlushDataBlock();

  SstBuilderOptions opt_;
  WritableFile* out_{nullptr};

  sstable::BlockBuilder data_block_;
  sstable::BlockBuilder index_block_;

  std::string last_data_key_;
  std::string last_key_in_current_block_;

  uint64_t offset_{0};
  bool finished_{false};
};
