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
