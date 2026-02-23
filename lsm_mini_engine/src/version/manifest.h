#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "iter/iterator.h"
#include "sstable/reader.h"

struct FileMeta {
  uint64_t file_number;
  uint64_t file_size;
  std::string smallest; // internal key
  std::string largest;  // internal key
  std::shared_ptr<TableReader> table; // lazily opened ok too
};

struct Version {
  uint64_t generation{0};
  std::vector<FileMeta> l0;     // simplest: only one level
  // If you add levels later: std::vector<std::vector<FileMeta>> levels;

  uint64_t last_sequence{0};
};

class VersionSet {
public:
  VersionSet() = default;

  std::shared_ptr<const Version> Current() const { return current_.load(); }

  // publish a new immutable version (copy-on-write)
  void Publish(std::shared_ptr<const Version> v) { current_.store(std::move(v)); }

  // Append-only manifest (optional initially): record edits then rebuild at open.
  Status LogAndApply(/*VersionEdit*/);

private:
  std::atomic<std::shared_ptr<const Version>> current_{std::make_shared<Version>()};
};
