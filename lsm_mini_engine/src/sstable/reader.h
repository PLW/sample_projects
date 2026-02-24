#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "iter/iterator.h"       // Iterator, Status, Slice
#include "iter/internal_key.h"   // ValueType, DecodeInternalKey
#include "sstable/format.h"
#include "sstable/block.h"

// Reuse your existing file abstraction.
class RandomAccessFile {
public:
  virtual ~RandomAccessFile() = default;
  virtual Status Read(uint64_t offset, size_t n, std::string* dst) const = 0;
  virtual uint64_t Size() const = 0;
};

struct TableReaderOptions {
  bool verify_magic = true;
};

class TableReader {
public:
  static Status Open(TableReaderOptions opt,
                     std::shared_ptr<const RandomAccessFile> file,
                     std::unique_ptr<TableReader>* out);

  std::unique_ptr<Iterator> NewIterator() const;

  // internal_key is (user_key, snapshot_seq, PUT) style.
  Status Get(Slice internal_key, std::string* value_out, ValueType* type_out) const;

private:
  TableReader(std::shared_ptr<const RandomAccessFile> f, sstable::Footer footer);

  Status ReadBlock(const sstable::BlockHandle& h, std::string* out) const;

  // Find the first data block whose index key >= target. Returns false if none.
  bool FindDataBlockForKey(Slice target, sstable::BlockHandle* h_out) const;

  std::shared_ptr<const RandomAccessFile> file_;
  sstable::Footer footer_;

  // cached decoded index block
  std::string index_storage_;
  sstable::BlockView index_view_;
};
