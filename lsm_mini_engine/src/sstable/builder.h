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
  std::string last_index_key_;
  std::string data_block_;
  std::vector<uint32_t> restarts_;
  int entries_since_restart_{0};

  // index entries: separator_key -> BlockHandle
  std::string index_block_;        // encoded as a block too
  std::vector<uint32_t> index_restarts_;
  int index_entries_since_restart_{0};

  // deprecated elements:
  //std::string pending_index_key_;
  //BlockHandle pending_handle_;
  //bool has_pending_index_{false};

  // bloom building: add user_key or internal_key (pick user_key)
  // (implementation in bloom.h)
  std::vector<uint32_t> bloom_hashes_;

  uint64_t offset_{0};
  bool finished_{false};
};
