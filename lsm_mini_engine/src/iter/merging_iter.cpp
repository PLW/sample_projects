// src/iter/merging_iter.cpp
#include "iter/merging_iter.h"

#include <utility>

MergingIterator::MergingIterator(InternalKeyComparator cmp,
                                 std::vector<std::unique_ptr<Iterator>> children)
  : cmp_(cmp),
    children_(std::move(children)),
    heap_(HeapCmp{cmp_}) {}

void MergingIterator::SeekToFirst() {
  while (!heap_.empty()) heap_.pop();
  status_ = Status::OK();

  for (size_t i = 0; i < children_.size(); i++) {
    children_[i]->SeekToFirst();
    Status s = children_[i]->status();
    if (!s) status_ = s;
    if (children_[i]->Valid()) {
      heap_.push(HeapItem{i, children_[i]->key()});
    }
  }
  RebuildTop();
}

void MergingIterator::Seek(Slice target) {
  while (!heap_.empty()) heap_.pop();
  status_ = Status::OK();

  for (size_t i = 0; i < children_.size(); i++) {
    children_[i]->Seek(target);
    Status s = children_[i]->status();
    if (!s) status_ = s;
    if (children_[i]->Valid()) {
      heap_.push(HeapItem{i, children_[i]->key()});
    }
  }
  RebuildTop();
}

void MergingIterator::Next() {
  if (!valid_) return;
  AdvanceTop();
  RebuildTop();
}

void MergingIterator::AdvanceTop() {
  if (heap_.empty()) { valid_ = false; return; }

  const size_t idx = heap_.top().idx;
  heap_.pop();

  children_[idx]->Next();
  Status s = children_[idx]->status();
  if (!s) status_ = s;

  if (children_[idx]->Valid()) {
    heap_.push(HeapItem{idx, children_[idx]->key()});
  }
}

void MergingIterator::RebuildTop() {
  if (!status_) { valid_ = false; return; }
  if (heap_.empty()) { valid_ = false; return; }

  auto top = heap_.top();
  auto& it = children_[top.idx];
  if (!it->Valid()) { valid_ = false; return; }

  key_ = it->key();
  value_ = it->value();
  valid_ = true;
}
