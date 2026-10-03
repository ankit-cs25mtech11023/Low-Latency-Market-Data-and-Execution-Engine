# Low-Latency Market Data & Execution Engine

A Linux C++20 engine that consumes NASDAQ TotalView-ITCH 5.0 market data, builds per-symbol
limit order books, and runs a minimal strategy, risk and order-gateway path against a local
exchange emulator. It is a testbed for reproducible, hardware-counter-explained latency
experiments (E0–E9).

**Status:** under active development. Results will appear here only once measured, each backed
by a result file, the captured environment, and a reproduce command.

## Build

Requirements: Linux x86-64 with an invariant TSC, GCC ≥ 13 or Clang ≥ 18, CMake ≥ 3.25, Ninja.

```bash
cmake --preset release            # -O2 -DNDEBUG: the only build used for performance numbers
cmake --build --preset release
ctest --preset release
```

Other presets: `debug`, `relwithdebinfo` (profiling), `asan`, `ubsan`, `tsan`, and `clang-*` variants.

## Data

No Nasdaq data is stored in this repository. See [`data/README.md`](data/README.md) for how to
obtain the ITCH 5.0 files.

## Documentation

- [`plan.md`](plan.md): project blueprint and phases
- [`docs/methodology.md`](docs/methodology.md): how every number is measured
- [`docs/experiments/`](docs/experiments/): one write-up per experiment

## License

MIT, see [LICENSE](LICENSE).
