// diff_books: runs the reference book and the optimized book (FastBook) side by side over an
// ITCH file and stops at the first message after which they disagree.
//
//   diff_books --input data/07302019.NASDAQ_ITCH50.gz [--book fast|fast-fib] [--full-every 100000]
//              [--out result.json]
//
// Comparison (see lle/book/differential.hpp): top of book after every book message, full depth
// of every touched symbol every --full-every messages, counters and live orders at the end.
// On a mismatch it prints the message index and locate; a reproduction is cut with
//   slice_itch --symbols <SYM> ... (one symbol's messages rebuild its book on their own).
// Exit code: 0 = books agree on the whole input, 1 = mismatch, 2 = error.

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

#include "lle/book/differential.hpp"
#include "lle/book/fast_book.hpp"
#include "lle/book/reference_book.hpp"
#include "lle/core/cli.hpp"
#include "lle/protocol/itch.hpp"
#include "lle/protocol/itch_file.hpp"
#include "lle/telemetry/result_io.hpp"

namespace {

template <class Fast>
int run(const std::string& in, const std::string& book_name, std::uint64_t full_every, const std::string& out) {
    using namespace lle;
    auto ref = std::make_unique<book::ReferenceBook>();
    auto fast = std::make_unique<Fast>();
    auto t = std::make_unique<book::DifferentialTester<book::ReferenceBook, Fast>>(*ref, *fast, full_every);

    const auto t0 = std::chrono::steady_clock::now();
    itch::ItchFileReader reader(itch::open_source(in));
    const std::byte* p = nullptr;
    std::size_t len = 0;
    while (!t->failed() && reader.next(p, len)) {
        const auto st = itch::decode(p, len, *t);
        if (st == itch::DecodeStatus::Truncated || st == itch::DecodeStatus::Empty) t->on_undecodable();
    }
    t->finish();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const book::BookCounters& c = ref->counters();
    std::string json = R"({"tool":"diff_books","book":")" + book_name + R"(","input":")" + json_escape(in) +
                       R"(","build":)" + build_info_json() + R"(,"messages":)" + std::to_string(t->messages()) +
                       R"(,"top_checks":)" + std::to_string(t->top_checks()) + R"(,"full_checks":)" +
                       std::to_string(t->full_checks()) + R"(,"full_every":)" + std::to_string(full_every) +
                       R"(,"agree":)" + (t->failed() ? "false" : "true") + R"(,"reference_counters":{"adds":)" +
                       std::to_string(c.adds) + R"(,"executes":)" + std::to_string(c.executes) + R"(,"cancels":)" +
                       std::to_string(c.cancels) + R"(,"deletes":)" + std::to_string(c.deletes) + R"(,"replaces":)" +
                       std::to_string(c.replaces) + R"(,"unknown_ref":)" + std::to_string(c.unknown_ref) +
                       R"(},"live_orders_end":)" + std::to_string(fast->live_orders()) +
                       R"(,"wall_seconds_informational":)" + std::to_string(secs);
    if (t->failed()) {
        const auto& m = t->mismatch();
        json += R"(,"mismatch":{"message_index":)" + std::to_string(m.message_index) + R"(,"locate":)" +
                std::to_string(m.locate) + R"(,"type":")" + std::string(1, m.message_type ? m.message_type : '?') +
                R"(","what":")" + json_escape(m.what) + R"("})";
    }
    json += "}\n";
    std::fputs(json.c_str(), stdout);
    if (!out.empty()) {
        std::FILE* f = std::fopen(out.c_str(), "w");
        if (f == nullptr) throw std::runtime_error("cannot write " + out);
        std::fputs(json.c_str(), f);
        std::fclose(f);
    }
    return t->failed() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    const std::string in = cli.str("input");
    const std::string book_name = cli.str("book", "fast");
    const auto full_every = static_cast<std::uint64_t>(cli.i64("full-every", 100'000));
    const std::string out = cli.str("out", "");
    if (book_name == "fast") return run<lle::book::FastBook>(in, book_name, full_every, out);
    if (book_name == "fast-fib") return run<lle::book::FastBookFib>(in, book_name, full_every, out);
    throw std::invalid_argument("--book must be fast or fast-fib");
} catch (const std::exception& e) {
    std::fprintf(stderr, "diff_books: %s\n", e.what());
    return 2;
}
