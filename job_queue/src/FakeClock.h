// Deterministic clock for unit tests.

#pragma once
#include "LeaseQueue.h"

namespace leaseq {

struct FakeClock final : IClock {
  TimePoint t = TimePoint{};  // starts at epoch of steady_clock domain
  TimePoint now() const override { return t; }
  void advance(Duration d) { t += d; }
};

} // namespace leaseq
