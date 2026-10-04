// Streaming ITCH file reader tests: framing, buffer refills, compressed sources, and every
// way a stream can end badly. A full day is streamed through this reader, so a silent
// truncation would silently drop the end of the trading day from every experiment.
#include <gtest/gtest.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_encode.hpp"
#include "lle/protocol/itch_file.hpp"

namespace {

using namespace lle::itch;
namespace fs = std::filesystem;

// n framed messages of varying type/size; message i is a Delete with ref i or an Add.
std::vector<std::byte> make_stream(std::size_t n) {
    std::vector<std::byte> out;
    std::array<std::byte, 64> msg{};
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t len = (i % 3 == 0) ? encode_add(msg.data(), 1, i, i, Side::Buy, 100, make_symbol("T"), 1000)
                                             : encode_delete(msg.data(), 1, i, i);
        const std::size_t at = out.size();
        out.resize(at + 2 + len);
        frame(out.data() + at, msg.data(), len);
    }
    return out;
}

// Reads everything; checks message i carries ref i.
std::size_t read_all(ItchFileReader& r) {
    const std::byte* p = nullptr;
    std::size_t len = 0, n = 0;
    while (r.next(p, len)) {
        OrderRef ref = 0;
        struct H : NullHandler {
            OrderRef* out;
            void on_add(const AddOrder& m) { *out = m.ref; }
            void on_delete(const OrderDelete& m) { *out = m.ref; }
        } h;
        h.out = &ref;
        EXPECT_EQ(decode(p, len, h), DecodeStatus::Ok);
        EXPECT_EQ(ref, n) << "message order or content changed";
        ++n;
    }
    return n;
}

class TempDir : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("lle_itch_file_test_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    std::string write_gz(const std::string& name, const std::vector<std::byte>& data) {
        const auto path = (dir_ / name).string();
        gzFile g = gzopen(path.c_str(), "wb");
        EXPECT_NE(g, nullptr);
        EXPECT_EQ(gzwrite(g, data.data(), static_cast<unsigned>(data.size())), static_cast<int>(data.size()));
        gzclose(g);
        return path;
    }
    std::string write_raw(const std::string& name, const std::vector<std::byte>& data) {
        const auto path = (dir_ / name).string();
        std::FILE* f = std::fopen(path.c_str(), "wb");
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        return path;
    }
    fs::path dir_;
};

TEST(ItchFileReader, EmptyStreamIsCleanEnd) {
    ItchFileReader r(memory_source({}));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    EXPECT_FALSE(r.next(p, len));
    EXPECT_EQ(r.messages(), 0u);
}

TEST(ItchFileReader, ReadsAllMessagesAcrossManyRefills) {
    // ~1.6 MB of messages through the minimum 128 KiB buffer: many messages straddle a
    // refill boundary and must be moved to the front of the buffer intact.
    const auto data = make_stream(60'000);
    ItchFileReader r(memory_source(data), 1);  // 1 = request the smallest buffer
    EXPECT_EQ(read_all(r), 60'000u);
    EXPECT_EQ(r.bytes(), data.size());
}

TEST(ItchFileReader, ZeroLengthMessageIsReturned) {
    std::vector<std::byte> data{std::byte{0}, std::byte{0}};
    ItchFileReader r(memory_source(data));
    const std::byte* p = nullptr;
    std::size_t len = 99;
    ASSERT_TRUE(r.next(p, len));
    EXPECT_EQ(len, 0u);
    EXPECT_FALSE(r.next(p, len));
}

TEST(ItchFileReader, TruncatedInsideLengthPrefixThrows) {
    auto data = make_stream(3);
    data.push_back(std::byte{0});  // half a length prefix
    ItchFileReader r(memory_source(data));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(r.next(p, len));
    EXPECT_THROW(r.next(p, len), std::runtime_error);
}

TEST(ItchFileReader, TruncatedInsideMessageThrows) {
    auto data = make_stream(3);
    data.resize(data.size() - 5);
    ItchFileReader r(memory_source(data));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(r.next(p, len));
    EXPECT_THROW(r.next(p, len), std::runtime_error);
}

TEST_F(TempDir, PlainFileSource) {
    const auto path = write_raw("plain.itch", make_stream(5000));
    ItchFileReader r(open_source(path));
    EXPECT_EQ(read_all(r), 5000u);
}

TEST_F(TempDir, GzipViaZlibAndPigzGiveSameMessages) {
    const auto path = write_gz("day.gz", make_stream(50'000));
    ItchFileReader rz(open_source(path, Decompressor::Zlib));
    EXPECT_EQ(read_all(rz), 50'000u);
    if (std::system("command -v pigz >/dev/null 2>&1") != 0)  // NOLINT(concurrency-mt-unsafe): single-threaded test
        GTEST_SKIP() << "pigz not installed";
    ItchFileReader rp(open_source(path, Decompressor::Pigz));
    EXPECT_EQ(read_all(rp), 50'000u);
}

// A .gz cut at an arbitrary byte: the decompressed data may end exactly at a message boundary,
// so the framing check alone cannot be relied on; the decompressor's error must surface.
std::vector<std::byte> read_file(const std::string& path) {
    std::vector<std::byte> d(fs::file_size(path));
    std::FILE* f = std::fopen(path.c_str(), "rb");
    EXPECT_EQ(std::fread(d.data(), 1, d.size(), f), d.size());
    std::fclose(f);
    return d;
}

TEST_F(TempDir, TruncatedGzipIsAnErrorWithZlib) {
    const auto full = read_file(write_gz("full.gz", make_stream(50'000)));
    const auto cut =
        write_raw("cut.gz", std::vector<std::byte>(full.begin(), full.begin() + static_cast<long>(full.size() / 2)));
    ItchFileReader r(open_source(cut, Decompressor::Zlib));
    EXPECT_THROW(read_all(r), std::runtime_error);
}

TEST_F(TempDir, TruncatedGzipIsAnErrorWithPigz) {
    if (std::system("command -v pigz >/dev/null 2>&1") != 0)  // NOLINT(concurrency-mt-unsafe): single-threaded test
        GTEST_SKIP() << "pigz not installed";
    const auto full = read_file(write_gz("full.gz", make_stream(50'000)));
    const auto cut =
        write_raw("cut.gz", std::vector<std::byte>(full.begin(), full.begin() + static_cast<long>(full.size() / 2)));
    ItchFileReader r(open_source(cut, Decompressor::Pigz));
    EXPECT_THROW(read_all(r), std::runtime_error);
}

TEST_F(TempDir, MissingFileThrows) {
    EXPECT_THROW((void)open_source((dir_ / "nope.itch").string()), std::runtime_error);
    EXPECT_THROW((void)open_source((dir_ / "nope.gz").string(), Decompressor::Zlib), std::runtime_error);
    EXPECT_THROW((void)open_source((dir_ / "nope.gz").string(), Decompressor::Pigz), std::runtime_error);
}

}  // namespace
