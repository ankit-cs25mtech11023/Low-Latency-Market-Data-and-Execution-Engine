// gen_fixture: writes a synthetic ITCH 5.0 stream (see include/lle/testing/fixture_gen.hpp).
//
//   gen_fixture --mode mixed --messages 1000000 --seed 1 --out fixtures/mixed.itch
//   gen_fixture --mode realistic --messages 5000000 --out /tmp/real.itch.gz   (gzip by suffix)
//
// The same arguments always produce the same bytes, so fixtures are regenerated in CI
// instead of being committed.
#include <cstdio>
#include <exception>
#include <string>

#include "lle/core/cli.hpp"
#include "lle/protocol/itch_writer.hpp"
#include "lle/testing/fixture_gen.hpp"

int main(int argc, char** argv) try {
    const lle::Cli cli(argc, argv);
    lle::testing::FixtureConfig cfg;
    cfg.mode = lle::testing::parse_fixture_mode(cli.str("mode", "mixed"));
    cfg.messages = static_cast<std::uint64_t>(cli.i64("messages", 100'000));
    cfg.seed = static_cast<std::uint64_t>(cli.i64("seed", 1));
    cfg.symbols = static_cast<std::uint32_t>(cli.i64("symbols", 50));
    const std::string out = cli.str("out");

    lle::itch::ItchWriter w(out);
    lle::testing::generate_fixture(cfg, [&](const std::byte* p, std::size_t n) { w.write_framed(p, n); });
    w.close();
    std::printf("%s: %llu messages, %llu bytes (mode %s, seed %llu)\n", out.c_str(),
                static_cast<unsigned long long>(w.messages()), static_cast<unsigned long long>(w.bytes()),
                lle::testing::fixture_mode_name(cfg.mode), static_cast<unsigned long long>(cfg.seed));
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "gen_fixture: %s\n", e.what());
    return 1;
}
