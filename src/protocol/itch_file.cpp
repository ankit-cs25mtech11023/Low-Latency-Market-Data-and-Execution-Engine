#include "lle/protocol/itch_file.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "lle/protocol/endian.hpp"

namespace lle::itch {
namespace {

class FdSource final : public ByteSource {
public:
    explicit FdSource(const std::string& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        if (fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    }
    ~FdSource() override { ::close(fd_); }
    FdSource(const FdSource&) = delete;
    FdSource& operator=(const FdSource&) = delete;

    std::size_t read(std::byte* dst, std::size_t n) override {
        for (;;) {
            const ssize_t r = ::read(fd_, dst, n);
            if (r >= 0) return static_cast<std::size_t>(r);
            if (errno != EINTR) throw std::runtime_error(std::string("read: ") + std::strerror(errno));
        }
    }

private:
    int fd_;
};

class ZlibSource final : public ByteSource {
public:
    explicit ZlibSource(const std::string& path) : gz_(gzopen(path.c_str(), "rb")) {
        if (gz_ == nullptr) throw std::runtime_error("gzopen " + path + " failed");
        gzbuffer(gz_, 1u << 20);
    }
    ~ZlibSource() override { gzclose(gz_); }
    ZlibSource(const ZlibSource&) = delete;
    ZlibSource& operator=(const ZlibSource&) = delete;

    std::size_t read(std::byte* dst, std::size_t n) override {
        const unsigned chunk = n > (1u << 30) ? (1u << 30) : static_cast<unsigned>(n);
        const int r = gzread(gz_, dst, chunk);
        if (r < 0) {
            int err = 0;
            throw std::runtime_error(std::string("gzread: ") + gzerror(gz_, &err));
        }
        return static_cast<std::size_t>(r);
    }

private:
    gzFile gz_;
};

class PipeSource final : public ByteSource {
public:
    explicit PipeSource(const std::string& cmd) : f_(::popen(cmd.c_str(), "r")) {
        if (f_ == nullptr) throw std::runtime_error("popen failed: " + cmd);
    }
    ~PipeSource() override { ::pclose(f_); }
    PipeSource(const PipeSource&) = delete;
    PipeSource& operator=(const PipeSource&) = delete;

    std::size_t read(std::byte* dst, std::size_t n) override {
        const std::size_t r = std::fread(dst, 1, n, f_);
        if (r == 0 && std::ferror(f_)) throw std::runtime_error("pipe read failed");
        return r;
    }

private:
    std::FILE* f_;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::vector<std::byte> d) : data_(std::move(d)) {}
    std::size_t read(std::byte* dst, std::size_t n) override {
        const std::size_t k = std::min(n, data_.size() - pos_);
        std::memcpy(dst, data_.data() + pos_, k);
        pos_ += k;
        return k;
    }

private:
    std::vector<std::byte> data_;
    std::size_t pos_ = 0;
};

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

}  // namespace

std::unique_ptr<ByteSource> open_source(const std::string& path, Decompressor d) {
    if (d == Decompressor::Auto) {
        if (!ends_with(path, ".gz")) d = Decompressor::None;
        else d = (std::system("command -v pigz >/dev/null 2>&1") == 0) ? Decompressor::Pigz : Decompressor::Zlib;
    }
    switch (d) {
        case Decompressor::None: return std::make_unique<FdSource>(path);
        case Decompressor::Zlib: return std::make_unique<ZlibSource>(path);
        case Decompressor::Pigz:
            if (::access(path.c_str(), R_OK) != 0) throw std::runtime_error("cannot read " + path);
            return std::make_unique<PipeSource>("pigz -dc " + shell_quote(path));
        case Decompressor::Auto: break;
    }
    throw std::logic_error("unreachable");
}

std::unique_ptr<ByteSource> memory_source(std::vector<std::byte> data) {
    return std::make_unique<MemorySource>(std::move(data));
}

ItchFileReader::ItchFileReader(std::unique_ptr<ByteSource> src, std::size_t buffer_bytes)
    : src_(std::move(src)), buf_(buffer_bytes < (1u << 17) ? (1u << 17) : buffer_bytes) {}

bool ItchFileReader::fill(std::size_t need) {
    if (end_ - pos_ >= need) return true;
    // Move the partial message to the front, then top the buffer up.
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
    end_ -= pos_;
    pos_ = 0;
    while (end_ < need && !eof_) {
        const std::size_t r = src_->read(buf_.data() + end_, buf_.size() - end_);
        if (r == 0) eof_ = true;
        end_ += r;
    }
    return end_ - pos_ >= need;
}

bool ItchFileReader::next(const std::byte*& p, std::size_t& len) {
    if (!fill(2)) {
        if (end_ != pos_) throw std::runtime_error("ITCH stream ends inside a length prefix");
        return false;
    }
    const std::size_t n = proto::load_be<std::uint16_t>(buf_.data() + pos_);
    if (!fill(2 + n)) throw std::runtime_error("ITCH stream ends inside a message (truncated file?)");
    p = buf_.data() + pos_ + 2;
    len = n;
    pos_ += 2 + n;
    ++messages_;
    bytes_ += 2 + n;
    return true;
}

}  // namespace lle::itch
