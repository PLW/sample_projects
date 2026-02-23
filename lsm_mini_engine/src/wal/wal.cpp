#include "wal/wal.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "sstable/varint.h"

// Tiny CRC32 (ok for tests); replace with a real one later.
static uint32_t crc32_fast(const uint8_t* data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (-(int)(c & 1)));
  }
  return ~c;
}

static void PutFixed32LE(std::string& dst, uint32_t v) { PutFixed32(dst, v); }
static bool GetFixed32LE(Slice* in, uint32_t* out) {
  if (in->n < 4) return false;
  std::memcpy(out, in->p, 4);
  in->p += 4; in->n -= 4;
  return true;
}

static void PutU8(std::string& dst, uint8_t v) { dst.push_back(char(v)); }
static bool GetU8(Slice* in, uint8_t* out) {
  if (in->n < 1) return false;
  *out = uint8_t(in->p[0]);
  in->p += 1; in->n -= 1;
  return true;
}

static void PutBytes(std::string& dst, Slice s) { dst.append(s.p, s.n); }
static bool GetBytes(Slice* in, size_t n, Slice* out) {
  if (in->n < n) return false;
  *out = Slice{in->p, n};
  in->p += n; in->n -= n;
  return true;
}

static std::string EncodeWalPayload(const WalRecord& r) {
  // payload := varint64 seq | varint32 key_len | key | varint32 val_len | val
  std::string p;
  PutVarint64(p, r.seq);
  PutVarint32(p, uint32_t(r.user_key.size()));
  PutBytes(p, Slice(r.user_key));
  PutVarint32(p, uint32_t(r.value.size()));
  PutBytes(p, Slice(r.value));
  return p;
}

static Status DecodeWalPayload(Slice* payload, WalRecord* r) {
  uint64_t seq = 0;
  if (!GetVarint64(payload, &seq)) return Status::Corruption("wal: bad seq");
  r->seq = seq;

  uint64_t klen64 = 0;
  if (!GetVarint64(payload, &klen64)) return Status::Corruption("wal: bad klen");
  if (klen64 > (1u << 30)) return Status::Corruption("wal: klen too large");
  Slice k;
  if (!GetBytes(payload, size_t(klen64), &k)) return Status::Corruption("wal: key short");
  r->user_key.assign(k.p, k.n);

  uint64_t vlen64 = 0;
  if (!GetVarint64(payload, &vlen64)) return Status::Corruption("wal: bad vlen");
  if (vlen64 > (1u << 30)) return Status::Corruption("wal: vlen too large");
  Slice v;
  if (!GetBytes(payload, size_t(vlen64), &v)) return Status::Corruption("wal: val short");
  r->value.assign(v.p, v.n);

  return Status::OK();
}

Status WalWriter::AppendPut(uint64_t seq, Slice user_key, Slice value) {
  WalRecord r;
  r.type = ValueType::kPut;
  r.seq = seq;
  r.user_key.assign(user_key.p, user_key.n);
  r.value.assign(value.p, value.n);
  return AppendRecord(r);
}

Status WalWriter::AppendDel(uint64_t seq, Slice user_key) {
  WalRecord r;
  r.type = ValueType::kDel;
  r.seq = seq;
  r.user_key.assign(user_key.p, user_key.n);
  r.value.clear();
  return AppendRecord(r);
}

Status WalWriter::AppendRecord(const WalRecord& r) {
  // record := fixed32 crc | varint32 len | u8 type | payload
  std::string payload = EncodeWalPayload(r);

  std::string rec;
  // placeholder crc
  PutFixed32LE(rec, 0);
  PutVarint32(rec, uint32_t(payload.size() + 1));
  PutU8(rec, uint8_t(r.type));
  rec.append(payload);

  uint32_t crc = crc32_fast(reinterpret_cast<const uint8_t*>(rec.data() + 4), rec.size() - 4);
  std::memcpy(rec.data(), &crc, 4);

  return f_->Append(Slice(rec));
}

Status WalReader::Replay(MemTable* mem, uint64_t* max_seq_out) {
  // Read entire file; mini-engine approach for tests.
  std::string all;
  Status s = f_->Read(0, size_t(f_->Size()), &all);
  if (!s) return s;

  Slice in(all);
  uint64_t max_seq = 0;

  while (in.n > 0) {
    uint32_t crc = 0;
    if (!GetFixed32LE(&in, &crc)) return Status::Corruption("wal: missing crc");

    // len is varint32 but we used PutVarint32; decode with GetVarint64 for convenience.
    uint64_t len64 = 0;
    if (!GetVarint64(&in, &len64)) return Status::Corruption("wal: missing len");
    if (len64 > in.n) return Status::Corruption("wal: length past EOF");

    Slice rec_body;
    if (!GetBytes(&in, size_t(len64), &rec_body)) return Status::Corruption("wal: short record");

    uint32_t crc2 = crc32_fast(reinterpret_cast<const uint8_t*>(rec_body.p), rec_body.n);
    if (crc2 != crc) return Status::Corruption("wal: bad crc");

    Slice body(rec_body);
    uint8_t type_u8 = 0;
    if (!GetU8(&body, &type_u8)) return Status::Corruption("wal: missing type");

    WalRecord r;
    r.type = (type_u8 == uint8_t(ValueType::kPut)) ? ValueType::kPut : ValueType::kDel;

    Status ds = DecodeWalPayload(&body, &r);
    if (!ds) return ds;

    if (r.type == ValueType::kPut) {
      Status ps = mem->Put(r.seq, Slice(r.user_key), Slice(r.value));
      if (!ps) return ps;
    } else {
      Status ds2 = mem->Del(r.seq, Slice(r.user_key));
      if (!ds2) return ds2;
    }
    max_seq = std::max(max_seq, r.seq);
  }

  if (max_seq_out) *max_seq_out = max_seq;
  return Status::OK();
}
