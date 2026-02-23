#include "version/manifest.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sstable/varint.h"
#include "sstable/reader.h"

// small crc32 (same as wal.cc)
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

std::string VersionEdit::Encode() const {
  std::string p;

  if (set_last_sequence) {
    PutU8(p, 1);
    PutVarint64(p, *set_last_sequence);
  }
  if (set_next_file_number) {
    PutU8(p, 2);
    PutVarint64(p, *set_next_file_number);
  }
  for (const auto& f : add_l0) {
    PutU8(p, 3);
    PutVarint64(p, f.file_number);
    PutVarint64(p, f.file_size);
    PutVarint32(p, uint32_t(f.smallest.size()));
    PutBytes(p, Slice(f.smallest));
    PutVarint32(p, uint32_t(f.largest.size()));
    PutBytes(p, Slice(f.largest));
  }
  for (uint64_t x : del_l0) {
    PutU8(p, 4);
    PutVarint64(p, x);
  }
  return p;
}

Status VersionEdit::Decode(Slice rec, VersionEdit* out) {
  *out = VersionEdit{};
  Slice in = rec;
  while (in.n > 0) {
    uint8_t tag = 0;
    if (!GetU8(&in, &tag)) return Status::Corruption("manifest: tag");
    if (tag == 1) {
      uint64_t v=0; if (!GetVarint64(&in, &v)) return Status::Corruption("manifest: lastseq");
      out->set_last_sequence = v;
    } else if (tag == 2) {
      uint64_t v=0; if (!GetVarint64(&in, &v)) return Status::Corruption("manifest: nextfile");
      out->set_next_file_number = v;
    } else if (tag == 3) {
      FileMeta f;
      if (!GetVarint64(&in, &f.file_number)) return Status::Corruption("manifest: add file_no");
      if (!GetVarint64(&in, &f.file_size)) return Status::Corruption("manifest: add file_size");
      uint64_t sl=0, ll=0;
      if (!GetVarint64(&in, &sl)) return Status::Corruption("manifest: add sl");
      Slice ssmall; if (!GetBytes(&in, size_t(sl), &ssmall)) return Status::Corruption("manifest: add smallest");
      if (!GetVarint64(&in, &ll)) return Status::Corruption("manifest: add ll");
      Slice slarge; if (!GetBytes(&in, size_t(ll), &slarge)) return Status::Corruption("manifest: add largest");
      f.smallest.assign(ssmall.p, ssmall.n);
      f.largest.assign(slarge.p, slarge.n);
      out->add_l0.push_back(std::move(f));
    } else if (tag == 4) {
      uint64_t v=0; if (!GetVarint64(&in, &v)) return Status::Corruption("manifest: del file_no");
      out->del_l0.push_back(v);
    } else {
      return Status::Corruption("manifest: unknown tag");
    }
  }
  return Status::OK();
}

static std::string ManifestPath(const std::string& dbdir) { return dbdir + "/MANIFEST"; }

static Status AppendManifestRecord(WritableFile& wf, Slice payload) {
  std::string rec;
  PutFixed32LE(rec, 0);
  PutVarint32(rec, uint32_t(payload.n));
  PutBytes(rec, payload);

  uint32_t crc = crc32_fast(reinterpret_cast<const uint8_t*>(rec.data() + 4), rec.size() - 4);
  std::memcpy(rec.data(), &crc, 4);

  Status s = wf.Append(Slice(rec));
  if (!s) return s;
  return wf.Sync();
}

static Status ReadAll(const RandomAccessFile& f, std::string* out) {
  out->clear();
  out->resize(size_t(f.Size()));
  // cheap: use f.Read in one go (requires exact size support)
  return f.Read(0, size_t(f.Size()), out);
}

Status VersionSet::Recover(Env* env, const std::string& dbdir) {
  bool ex = false;
  Status s = env->FileExists(ManifestPath(dbdir), &ex);
  if (!s) return s;

  auto v = std::make_shared<Version>();
  if (!ex) {
    // brand new DB: create empty manifest with initial edit
    v->generation = 1;
    v->last_sequence = 0;
    v->next_file_number = 1;

    std::unique_ptr<WritableFile> wf;
    s = env->NewWritableFile(ManifestPath(dbdir), &wf);
    if (!s) return s;

    VersionEdit init;
    init.set_last_sequence = v->last_sequence;
    init.set_next_file_number = v->next_file_number;
    std::string payload = init.Encode();
    s = AppendManifestRecord(*wf, Slice(payload));
    if (!s) return s;

    Publish(v);
    return Status::OK();
  }

  // Read and replay manifest
  std::shared_ptr<const RandomAccessFile> rf;
  s = env->NewRandomAccessFile(ManifestPath(dbdir), &rf);
  if (!s) return s;

  std::string all;
  s = ReadAll(*rf, &all);
  if (!s) return s;

  Slice in(all);
  while (in.n > 0) {
    uint32_t crc = 0;
    if (!GetFixed32LE(&in, &crc)) return Status::Corruption("manifest: crc");
    uint64_t len64 = 0;
    if (!GetVarint64(&in, &len64)) return Status::Corruption("manifest: len");
    if (len64 > in.n) return Status::Corruption("manifest: short");

    Slice payload;
    if (!GetBytes(&in, size_t(len64), &payload)) return Status::Corruption("manifest: payload short");

    uint32_t crc2 = crc32_fast(reinterpret_cast<const uint8_t*>(payload.p), payload.n);
    if (crc2 != crc) return Status::Corruption("manifest: bad crc");

    VersionEdit e;
    s = VersionEdit::Decode(payload, &e);
    if (!s) return s;

    // Apply edit
    if (e.set_last_sequence) v->last_sequence = *e.set_last_sequence;
    if (e.set_next_file_number) v->next_file_number = *e.set_next_file_number;

    for (uint64_t del : e.del_l0) {
      v->l0.erase(std::remove_if(v->l0.begin(), v->l0.end(),
                                [&](const FileMeta& fm){ return fm.file_number == del; }),
                  v->l0.end());
    }
    for (auto& add : e.add_l0) {
      v->l0.push_back(std::move(add));
    }
  }

  // lazily open tables later; or open now:
  // for (auto& f : v->l0) { ... TableReader::Open ... }

  Publish(v);
  return Status::OK();
}

Status VersionSet::LogAndApply(Env* env, const std::string& dbdir, const VersionEdit& edit) {
  // Read current
  auto cur = Current();
  auto next = std::make_shared<Version>(*cur);
  next->generation = cur->generation + 1;

  // Apply to next in-memory
  if (edit.set_last_sequence) next->last_sequence = *edit.set_last_sequence;
  if (edit.set_next_file_number) next->next_file_number = *edit.set_next_file_number;

  for (uint64_t del : edit.del_l0) {
    next->l0.erase(std::remove_if(next->l0.begin(), next->l0.end(),
                                  [&](const FileMeta& fm){ return fm.file_number == del; }),
                   next->l0.end());
  }
  for (auto add : edit.add_l0) { // copy
    next->l0.push_back(std::move(add));
  }

  // Append to MANIFEST
  // NOTE: for performance you’d keep MANIFEST open; for mini-engine reopen/append is fine.
  std::unique_ptr<WritableFile> wf;
  Status s = env->NewWritableFile(ManifestPath(dbdir) + ".tmp_append", &wf);
  if (!s) return s;

  // We need append, not truncate. Simplest: implement NewAppendableFile later.
  // For now: emulate append by reading old MANIFEST and writing new one atomically.
  bool ex=false; env->FileExists(ManifestPath(dbdir), &ex);
  std::string old;
  if (ex) {
    std::shared_ptr<const RandomAccessFile> rf;
    s = env->NewRandomAccessFile(ManifestPath(dbdir), &rf);
    if (!s) return s;
    s = ReadAll(*rf, &old);
    if (!s) return s;
    s = wf->Append(Slice(old));
    if (!s) return s;
  }
  std::string payload = edit.Encode();
  s = AppendManifestRecord(*wf, Slice(payload));
  if (!s) return s;

  // atomic replace
  s = env->RenameFile(ManifestPath(dbdir) + ".tmp_append", ManifestPath(dbdir));
  if (!s) return s;

  Publish(next);
  return Status::OK();
}

