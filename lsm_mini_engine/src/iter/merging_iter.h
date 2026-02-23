#pragma once
#include <queue>
#include <vector>
#include <memory>
#include "iter/iterator.h"
#include "iter/internal_key.h"

class MergingIterator : public Iterator {
public:
  MergingIterator(InternalKeyComparator cmp,
                  std::vector<std::unique_ptr<Iterator>> children);

  void Seek(Slice target) override;
  void SeekToFirst() override;
  void Next() override;

  bool Valid() const override { return valid_; }
  Slice key() const override { return key_; }
  Slice value() const override { return value_; }
  Status status() const override { return status_; }

private:
  struct HeapItem {
    size_t idx;
    Slice key;
  };
  struct HeapCmp {
    InternalKeyComparator cmp;
    bool operator()(const HeapItem& a, const HeapItem& b) const {
      return cmp(a.key, b.key) > 0; // min-heap via priority_queue invert
    }
  };

  void RebuildTop();
  void AdvanceTop();

  InternalKeyComparator cmp_;
  std::vector<std::unique_ptr<Iterator>> children_;
  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCmp> heap_;

  bool valid_{false};
  Slice key_, value_;
  Status status_{Status::OK()};
};
