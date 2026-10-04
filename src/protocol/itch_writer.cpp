#include "lle/protocol/itch_writer.hpp"

#include <zlib.h>

#include <array>
#include <cstdio>
#include <stdexcept>

#include "lle/protocol/endian.hpp"

namespace lle::itch {

struct ItchWriter::Impl {
    std::string path;
    gzFile gz = nullptr;
    std::FILE* f = nullptr;

    void write(const void* p, std::size_t n) {
        if (gz != nullptr) {
            if (gzwrite(gz, p, static_cast<unsigned>(n)) != static_cast<int>(n))
                throw std::runtime_error("gzwrite failed: " + path);
        } else if (std::fwrite(p, 1, n, f) != n) {
            throw std::runtime_error("write failed: " + path);
        }
    }
};

ItchWriter::ItchWriter(const std::string& path) : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    if (path.size() > 3 && path.ends_with(".gz")) {
        impl_->gz = gzopen(path.c_str(), "wb6");
        if (impl_->gz == nullptr) throw std::runtime_error("cannot open " + path);
        gzbuffer(impl_->gz, 1u << 20);
    } else {
        impl_->f = std::fopen(path.c_str(), "wb");
        if (impl_->f == nullptr) throw std::runtime_error("cannot open " + path);
    }
}

ItchWriter::~ItchWriter() {
    if (impl_->gz != nullptr) gzclose(impl_->gz);
    if (impl_->f != nullptr) std::fclose(impl_->f);
}

void ItchWriter::write_message(const std::byte* msg, std::size_t len) {
    if (len > 0xFFFF) throw std::invalid_argument("ITCH message longer than 65535 bytes");
    std::array<std::byte, 2> prefix{};
    proto::store_be<std::uint16_t>(prefix.data(), static_cast<std::uint16_t>(len));
    impl_->write(prefix.data(), 2);
    impl_->write(msg, len);
    ++messages_;
    bytes_ += 2 + len;
}

void ItchWriter::write_framed(const std::byte* framed, std::size_t len) {
    impl_->write(framed, len);
    ++messages_;
    bytes_ += len;
}

void ItchWriter::close() {
    if (impl_->gz != nullptr) {
        const int rc = gzclose(impl_->gz);
        impl_->gz = nullptr;
        if (rc != Z_OK) throw std::runtime_error("gzclose failed: " + impl_->path);
    }
    if (impl_->f != nullptr) {
        const int rc = std::fclose(impl_->f);
        impl_->f = nullptr;
        if (rc != 0) throw std::runtime_error("close failed: " + impl_->path);
    }
}

}  // namespace lle::itch
