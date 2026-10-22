// Disc image containers: plain ISO/GCM plus the compressed formats Dolphin produces (CISO, GCZ, WIA, RVZ).
// A BlobReader presents any of them as the plain disc image it stands for.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace gc {

enum class ImageFormat { Unknown, RawGcm, Ciso, Gcz, Wia, Rvz, Wbfs };

const char* to_string(ImageFormat f);

class BlobReader {
public:
    virtual ~BlobReader() = default;
    virtual ImageFormat format() const = 0;
    // Size of the plain disc image this container represents.
    virtual uint64_t size() const = 0;
    // Reads `len` bytes of the plain image. On failure returns false and sets error().
    virtual bool read(uint64_t offset, void* dst, size_t len) = 0;
    // Human-readable detail about the container (e.g. "RVZ, zstd level 5, 128 KiB chunks").
    virtual std::string describe() const = 0;
    const std::string& error() const { return error_; }

protected:
    std::string error_;
};

// Detects the container format from the file header.
ImageFormat sniff_image(const std::string& path);

// Opens any supported container. On failure returns nullptr and sets `error`.
std::unique_ptr<BlobReader> open_blob(const std::string& path, std::string& error);

} // namespace gc
