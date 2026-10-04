// slice_itch: cuts a smaller ITCH file out of a full day, for development and debugging.
//
//   slice_itch --in data/07302019.NASDAQ_ITCH50.gz --out data/first10m.itch --first 10000000
//   slice_itch --in DAY.gz --out data/aapl_msft.itch.gz --symbols AAPL,MSFT
//   slice_itch --in DAY.gz --out data/top20.itch.gz --top-k 20      (two passes over the input)
//
// What is kept:
//   * every S (system event) message, so the slice has the day's start/end markers;
//   * with --symbols / --top-k: only messages whose stock locate belongs to a chosen symbol
//     (including that symbol's R directory message); an order never changes symbol, so each
//     chosen symbol's book is rebuilt exactly as in the full day;
//   * with --first N: only the first N messages of the input (applied before the filter).
// Slices are derived from Nasdaq data and are never committed (data/ is git-ignored).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

#include "lle/core/cli.hpp"
#include "lle/protocol/endian.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/protocol/itch_writer.hpp"

namespace {

using lle::itch::ItchFileReader;

struct DirectoryEntry {
    std::string symbol;
    std::uint64_t book_messages = 0;  // A F E C X D U for this locate
};

bool is_book_message(char t) {
    return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

// Pass 1 for --top-k: count book messages per locate and learn locate -> symbol.
std::vector<DirectoryEntry> count_per_locate(const std::string& in, std::uint64_t first) {
    std::vector<DirectoryEntry> dir(65536);
    ItchFileReader r(lle::itch::open_source(in));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    while (r.messages() < first && r.next(p, len)) {
        if (len < 3) continue;
        const char t = static_cast<char>(p[0]);
        const auto loc = lle::proto::load_be<std::uint16_t>(p + 1);
        if (t == 'R' && len >= lle::itch::kMessageLength['R']) {
            std::array<char, 8> s{};
            std::memcpy(s.data(), p + 11, 8);
            dir[loc].symbol = std::string(lle::itch::trim_symbol(s));
        } else if (is_book_message(t)) {
            ++dir[loc].book_messages;
        }
    }
    return dir;
}

}  // namespace

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    const std::string in = cli.str("in");
    const std::string out = cli.str("out");
    const auto first = static_cast<std::uint64_t>(cli.i64("first", INT64_MAX));
    const auto top_k = static_cast<std::size_t>(cli.i64("top-k", 0));
    std::unordered_set<std::string> want_symbols;
    if (cli.has("symbols"))
        for (const auto& s : cli.list("symbols")) want_symbols.insert(s);

    std::vector<bool> keep_locate(65536, false);
    const bool filter = top_k > 0 || !want_symbols.empty();
    if (top_k > 0) {
        auto dir = count_per_locate(in, first);
        std::vector<std::uint16_t> order(65536);
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::uint16_t>(i);
        std::partial_sort(order.begin(), order.begin() + static_cast<long>(std::min<std::size_t>(top_k, order.size())),
                          order.end(), [&](auto a, auto b) { return dir[a].book_messages > dir[b].book_messages; });
        std::printf("top %zu symbols by book messages:\n", top_k);
        for (std::size_t i = 0; i < top_k && dir[order[i]].book_messages > 0; ++i) {
            keep_locate[order[i]] = true;
            std::printf("  %-8s locate %5u  %llu\n", dir[order[i]].symbol.c_str(), order[i],
                        static_cast<unsigned long long>(dir[order[i]].book_messages));
        }
    }

    ItchFileReader r(lle::itch::open_source(in));
    lle::itch::ItchWriter w(out);
    const std::byte* p = nullptr;
    std::size_t len = 0;
    while (r.messages() < first && r.next(p, len)) {
        bool keep = true;
        if (filter && len >= 3) {
            const char t = static_cast<char>(p[0]);
            const auto loc = lle::proto::load_be<std::uint16_t>(p + 1);
            if (t == 'R' && len >= lle::itch::kMessageLength['R'] && !want_symbols.empty()) {
                std::array<char, 8> s{};
                std::memcpy(s.data(), p + 11, 8);
                if (want_symbols.count(std::string(lle::itch::trim_symbol(s))) != 0) keep_locate[loc] = true;
            }
            keep = t == 'S' || keep_locate[loc];
        }
        if (keep) w.write_message(p, len);
    }
    w.close();
    std::printf("%s: kept %llu of %llu messages (%llu bytes)\n", out.c_str(),
                static_cast<unsigned long long>(w.messages()), static_cast<unsigned long long>(r.messages()),
                static_cast<unsigned long long>(w.bytes()));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "slice_itch: %s\n", e.what());
    return 1;
}
