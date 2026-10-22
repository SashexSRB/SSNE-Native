// Small self-contained decompressors for compressed disc images (GCZ, WIA, RVZ). No external libraries, so the
// launcher builds the same way on every platform.
//
// Every function decodes `src` into exactly `dst_len` bytes. It returns false (and sets *error when given) on
// corrupt input or if the stream ends before `dst_len` bytes were produced.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace gc::decomp {

// zlib stream (RFC 1950 header + deflate) or raw deflate (RFC 1951) when `zlib_header` is false.
bool inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, bool zlib_header = true,
             std::string* error = nullptr);

// Zstandard frames (RFC 8878). Skippable frames are ignored; dictionaries are not supported.
bool zstd(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error = nullptr);

// Raw LZMA1 stream (no .lzma header). `props` is the 5-byte properties block: lc/lp/pb byte + dict size (LE).
// Stops at dst_len or at an end-of-payload marker.
bool lzma(const uint8_t props[5], const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len,
          std::string* error = nullptr);

// Raw LZMA2 stream (no .xz container). `dict_prop` is the 1-byte dictionary size property.
bool lzma2(uint8_t dict_prop, const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len,
           std::string* error = nullptr);

// bzip2 stream ("BZh" header), possibly several concatenated streams.
bool bzip2(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, std::string* error = nullptr);

} // namespace gc::decomp
