// Minimal streaming SHA-1 (used to fingerprint disc images, not for security).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gc {

class Sha1 {
public:
    Sha1();
    void update(const void* data, size_t len);
    std::array<uint8_t, 20> finish();
    static std::string hex(const std::array<uint8_t, 20>& digest);

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

} // namespace gc
