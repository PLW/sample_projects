#include "version/manifest.h"

// Mini-engine: no persistent manifest log yet.
// LogAndApply can be implemented later as:
//  - append VersionEdit records to MANIFEST file
//  - on Open, replay to rebuild current Version
Status VersionSet::LogAndApply(/*VersionEdit*/) {
  return Status::Invalid("manifest: not implemented");
}
