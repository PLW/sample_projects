#include "compaction/compaction.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "iter/merging_iter.h"
#include "iter/internal_key.h"

CompactionPlan Compactor::PickL0(const Version& v, size_t l0_trigger) {
  CompactionPlan p;
  if (v.l0.size() < l0_trigger) return p;
  // pick all for now
  p.inputs = v.l0;
  p.output_file_number = 0; // caller assigns
  return p;
}

Status Compactor::RunL0Compaction(const CompactionPlan& plan,
                                 VersionSet* versions,
                                 /*env*/ ) {
  if (plan.inputs.empty()) return Status::OK();
  // This function depends on your env/file creation; leave as scaffold.
  return Status::Invalid("compaction: env integration not implemented");
}
