# Fuzzing

libFuzzer targets, built only with the `fuzz` preset (clang; coverage instrumentation +
ASan + UBSan on all code).

```bash
cmake --preset fuzz && cmake --build --preset fuzz
# Seed corpus: synthetic ITCH streams (never Nasdaq data), small so mutations stay effective.
mkdir -p build/fuzz/corpus/itch
for m in realistic deep-queue wide-prices replace-chains crossing error-paths; do
  build/fuzz/tools/gen_fixture --mode $m --messages 200 --seed 1 --out build/fuzz/corpus/itch/$m.itch
done
build/fuzz/fuzz/fuzz_itch_decoder -max_total_time=60 -max_len=4096 build/fuzz/corpus/itch
```

| target | input | checks |
|---|---|---|
| `fuzz_itch_decoder` | bytes as one message, then as a framed stream | no crash / out-of-bounds / UB in the decoder or the reference book; two identical books never disagree |

A crash writes `crash-<hash>` in the working directory; reproduce with
`build/fuzz/fuzz/fuzz_itch_decoder crash-<hash>` and turn it into a unit test.
