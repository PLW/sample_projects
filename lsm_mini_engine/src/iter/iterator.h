#pragma once
#include <cstdint>
#include <string>
#include <string_view>

struct Status {
  enum Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArg } code{kOk};
  std::string msg;
  static Status OK() { return {}; }
  static Status NotFound(std::string m="") { return {kNotFound, std::move(m)}; }
  static Status Corruption(std::string m) { return {kCorruption, std::move(m)}; }
  static Status IOError(std::string m) { return {kIOError, std::move(m)}; }
  static Status Invalid(std::string m) { return {kInvalidArg, std::move(m)}; }
  explicit operator bool() const { return code == kOk; }
};

struct Slice {
  const char* p{nullptr};
  size_t n{0};
  Slice() = default;
  Slice(const char* p_, size_t n_) : p(p_), n(n_) {}
  Slice(std::string_view sv) : p(sv.data()), n(sv.size()) {}
  std::string_view sv() const { return {p, n}; }
};
