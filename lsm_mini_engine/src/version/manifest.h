#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iter/iterator.h"
#include "sstable/reader.h"
#include "env/posix_env.h" // Env

struct FileMeta {
  uint64_t file_number{0};
  uint64_t file_size{0};
  std::string smallest; // internal key
  std::string largest;  // internal key
  std::shared_ptr<TableReader> table; // optional cache
};

struct Version {
  uint64_t generation{0};
  std::vector<FileMeta> l0;
  uint64_t last_sequence{0};
  uint64_t next_file_number{1};
};

struct VersionEdit {
  // fields are optional; if set they update state
  std::optional<uint64_t> set_last_sequence;
  std::optional<uint64_t> set_next_file_number;

  std::vector<FileMeta> add_l0;
  std::vector<uint64_t> del_l0;

  // Serialize as a single record to MANIFEST.
  std::string Encode() const;
  static Status Decode(Slice rec, VersionEdit* out);
};

class VersionSet {
public:
  VersionSet() = default;

  std::shared_ptr<const Version> Current() const { return current_.load(); }
  void Publish(std::shared_ptr<const Version> v) { current_.store(std::move(v)); }

  // DB open: rebuild version by replaying MANIFEST records.
  Status Recover(Env* env, const std::string& dbdir);

  // Append edit to MANIFEST and atomically publish new version.
  Status LogAndApply(Env* env, const std::string& dbdir, const VersionEdit& edit);

private:
  std::atomic<std::shared_ptr<const Version>> current_{std::make_shared<Version>()};
};

