#pragma once
// Streaming reader for ITCH 5.0 files: [len:2 big-endian][message:len] repeated.
//
// A full NASDAQ day is several GB compressed and much more uncompressed, and the dev
// machine has 7.6 GiB of RAM, so files are never loaded whole. The reader keeps one
// fixed buffer (default 8 MiB), hands out pointers to complete messages inside it, and
// refills it from a ByteSource when the next message would cross the end of the buffer.
// The pointers are valid until the next call to next().
//
// Sources: plain files, .gz via zlib, or the stdout of `pigz -dc` (faster: decompression
// runs in another process, overlapping with our parsing).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <span>
#include <stdexcept>
#include <vector>

#include "lle/protocol/endian.hpp"

namespace lle::itch {

// Calls f(message, length) for every framed message of an in-memory stream (tests, fuzzing,
// pre-loaded slices). Throws std::runtime_error if the data ends inside a frame.
template <class F>
void for_each_framed(std::span<const std::byte> data, F&& f) {
    std::size_t pos = 0;
    while (pos < data.size()) {
        if (data.size() - pos < 2) throw std::runtime_error("stream ends inside a length prefix");
        const std::size_t n = proto::load_be<std::uint16_t>(data.data() + pos);
        if (data.size() - pos - 2 < n) throw std::runtime_error("stream ends inside a message");
        f(data.data() + pos + 2, n);
        pos += 2 + n;
    }
}

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads up to n bytes; returns 0 only at end of stream. Throws std::runtime_error on error.
    virtual std::size_t read(std::byte* dst, std::size_t n) = 0;
};

enum class Decompressor : std::uint8_t { Auto, None, Zlib, Pigz };

// Opens `path`. With Auto, ".gz" files use pigz if it is installed, else zlib.
[[nodiscard]] std::unique_ptr<ByteSource> open_source(const std::string& path, Decompressor d = Decompressor::Auto);

// In-memory source (tests, benchmarks on pre-loaded slices).
[[nodiscard]] std::unique_ptr<ByteSource> memory_source(std::vector<std::byte> data);

class ItchFileReader {
public:
    explicit ItchFileReader(std::unique_ptr<ByteSource> src, std::size_t buffer_bytes = 8u << 20);

    // Sets p/len to the next message body (without the length prefix). Returns false at a
    // clean end of stream. Throws std::runtime_error if the stream ends mid-message.
    bool next(const std::byte*& p, std::size_t& len);

    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    bool fill(std::size_t need);  // ensures `need` bytes available at pos_; false at EOF

    std::unique_ptr<ByteSource> src_;
    std::vector<std::byte> buf_;
    std::size_t pos_ = 0, end_ = 0;
    bool eof_ = false;
    std::uint64_t messages_ = 0, bytes_ = 0;
};

}  // namespace lle::itch
