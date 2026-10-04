#pragma once
// Writes a framed ITCH stream ([len:2 BE][message] ...) to a file; gzip-compressed when the
// path ends in ".gz". Used by tools that produce ITCH files (fixtures, slices, repros).
// Not for the hot path.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lle::itch {

class ItchWriter {
public:
    explicit ItchWriter(const std::string& path);
    ~ItchWriter();
    ItchWriter(const ItchWriter&) = delete;
    ItchWriter& operator=(const ItchWriter&) = delete;

    void write_message(const std::byte* msg, std::size_t len);  // adds the length prefix
    void write_framed(const std::byte* framed, std::size_t len);  // already framed
    void close();  // flushes; throws std::runtime_error if any write failed

    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t messages_ = 0, bytes_ = 0;
};

}  // namespace lle::itch
