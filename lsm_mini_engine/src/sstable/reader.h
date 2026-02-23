#pragma once

#include <memory>
#include <vector>

#include "iter/iterator.h"  // Status, Slice, Iterator
#include "iter/internal_key.h"
#include "sstable/format.h"
#include "sstable/bloom.h"

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
  Bloom bloom_;
};
