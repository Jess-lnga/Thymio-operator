#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
class Preferences {
 public:
  static std::vector<uint8_t> bytes;
  static bool failWrite;
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  size_t getBytesLength(const char*) { return bytes.size(); }
  size_t getBytes(const char*, void* dest, size_t size) {
    if (size != bytes.size()) return 0;
    std::memcpy(dest, bytes.data(), size);
    return size;
  }
  size_t putBytes(const char*, const void* src, size_t size) {
    if (failWrite) return 0;
    const auto* p = static_cast<const uint8_t*>(src);
    bytes.assign(p, p + size);
    return size;
  }
};
