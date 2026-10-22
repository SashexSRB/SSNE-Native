// SHA-256 (used to check downloaded files).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gc {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> finish();
    static std::string hex(const std::array<uint8_t, 32>& digest);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

// Hashes a whole file; returns an empty string if it can't be read.
std::string sha256_file(const std::string& path);

} // namespace gc
