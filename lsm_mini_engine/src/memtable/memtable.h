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
  explicit MemTable(MemOptions opt);

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
