#pragma once
#include <vector>
#include <memory>
#include "version/manifest.h"
#include "sstable/builder.h"
#include "iter/merging_iter.h"

struct CompactionPlan {
  std::vector<FileMeta> inputs;     // pick N newest or overlapping
  uint64_t output_file_number;
  size_t target_file_size = 64 * 1024 * 1024;
};

class Env;

class Compactor {
public:
  Status RunL0Compaction(const CompactionPlan& plan,
                         VersionSet* versions,
                         Env* env);

  // selection policy
  static CompactionPlan PickL0(const Version& v, size_t l0_trigger = 8);

private:
  // build output SSTables from merged iter, dropping shadowed keys, tombstones if safe
};
