#include "memtable/memtable.h"

#include <cstring>
#include <functional>

#include "iter/internal_key.h"

static bool StartsWith(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

MemTable::MemTable(MemOptions opt)
  : opt_(opt),
    map_(
      // comparator: InternalKeyComparator over string keys
      [](const std::string& a, const std::string& b) {
        InternalKeyComparator cmp;
        return cmp(Slice(a), Slice(b)) < 0;
      }
    ) {}

Status MemTable::Put(uint64_t seq, Slice user_key, Slice value) {
  std::unique_lock lk(mu_);
  std::string ik = EncodeInternalKey(user_key.sv(), seq, ValueType::kPut);
  std::string v(value.p, value.n);
  approx_bytes_ += ik.size() + v.size();
  map_[std::move(ik)] = std::move(v);
  return Status::OK();
}

Status MemTable::Del(uint64_t seq, Slice user_key) {
  std::unique_lock lk(mu_);
  std::string ik = EncodeInternalKey(user_key.sv(), seq, ValueType::kDel);
  std::string v; // empty
  approx_bytes_ += ik.size();
  map_[std::move(ik)] = std::move(v);
  return Status::OK();
}

Status MemTable::Get(Slice user_key, uint64_t snapshot_seq,
                     std::string* value_out, ValueType* type_out) const {
  std::shared_lock lk(mu_);
  // Seek to (user_key, snapshot_seq, PUT) and find first entry with same user_key.
  std::string seek = EncodeInternalKey(user_key.sv(), snapshot_seq, ValueType::kPut);
  auto it = map_.lower_bound(seek);

  // lower_bound may land after all entries for this user_key if seek is too small/large.
  // We want the first entry whose user_key matches, scanning forward until it stops matching.
  while (it != map_.end()) {
    Slice k(it->first);
    Slice uk; uint64_t tr = 0;
    if (!DecodeInternalKey(k, &uk, &tr)) return Status::Corruption("memtable: bad internal key");
    if (uk.sv() != user_key.sv()) break;

    uint64_t seq = (tr >> 8);
    ValueType vt = ValueType(uint8_t(tr & 0xFF));
    if (seq <= snapshot_seq) {
      if (type_out) *type_out = vt;
      if (vt == ValueType::kPut) {
        if (value_out) *value_out = it->second;
      } else {
        if (value_out) value_out->clear();
      }
      return Status::OK();
    }
    ++it;
  }

  // lower_bound might start past the newest <= snapshot; in practice with our comparator,
  // EncodeInternalKey(user, snapshot_seq) places us at the right spot.
  // But if not found, try scanning from the first key of that user_key.
  std::string low = std::string(user_key.sv());
  low.append(8, '\0'); // minimal trailer (0)
  auto it2 = map_.lower_bound(low);
  while (it2 != map_.end()) {
    Slice k(it2->first);
    Slice uk; uint64_t tr = 0;
    if (!DecodeInternalKey(k, &uk, &tr)) return Status::Corruption("memtable: bad internal key");
    if (uk.sv() != user_key.sv()) break;

    uint64_t seq = (tr >> 8);
    ValueType vt = ValueType(uint8_t(tr & 0xFF));
    if (seq <= snapshot_seq) {
      if (type_out) *type_out = vt;
      if (vt == ValueType::kPut) {
        if (value_out) *value_out = it2->second;
      } else {
        if (value_out) value_out->clear();
      }
      return Status::OK();
    }
    ++it2;
  }

  return Status::NotFound("memtable: not found");
}

std::shared_ptr<const MemTable> MemTable::Freeze() const {
  auto frozen = std::make_shared<MemTable>(opt_);
  {
    std::shared_lock lk(mu_);
    frozen->map_ = map_;
    frozen->approx_bytes_ = approx_bytes_;
  }
  return frozen;
}

// Simple iterator over map
namespace {
class MapIter final : public Iterator {
public:
  using MapT = decltype(std::declval<MemTable>().map_);
  explicit MapIter(const MapT* m) : m_(m) {}

  void SeekToFirst() override { it_ = m_->begin(); }
  void Seek(Slice target) override { it_ = m_->lower_bound(std::string(target.sv())); }
  void Next() override { if (it_ != m_->end()) ++it_; }

  bool Valid() const override { return it_ != m_->end(); }
  Slice key() const override { return Valid() ? Slice(it_->first) : Slice(); }
  Slice value() const override { return Valid() ? Slice(it_->second) : Slice(); }
  Status status() const override { return Status::OK(); }

private:
  const MapT* m_;
  MapT::const_iterator it_{};
};
} // namespace

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MapIter>(&map_);
}
