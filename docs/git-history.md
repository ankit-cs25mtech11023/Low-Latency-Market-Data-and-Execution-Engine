# Git history rewrite (2026-10-04): old → new commit IDs

On 2026-10-04 the commit messages of the first 13 commits were edited (a trailer line was
removed) and `main` was force-pushed. **Only the messages changed; every commit's file tree
is byte-identical** (checked with `git diff` between the old and new branch tips). Result
files recorded before the rewrite (`env.json`, `manifest.json`, `*.meta.json`) and the
write-ups that quote them still show the **old** IDs. Use this table to find the commit.

| old | new | subject |
|---|---|---|
| `9645853` | `431ed76` | Add project blueprint: plan.md, checkbox.md, claude.md from master plan v3 |
| `4239ae0` | `a6c87f7` | Update plan.md and checkbox.md — single CLAUDE.md, revised core schedule, study-guide task |
| `d9f22ef` | `240dcf0` | Update plan.md and checkbox.md — target v1.0 (P0–P8) for the resume deadline |
| `75fdff5` | `2282d15` | feat(core): P0 skeleton — CMake presets, TSC clock, histogram, E0 drivers, bench scripts |
| `7f96642` | `d437a6a` | feat(protocol): ITCH 5.0 decoder/encoder, streaming file reader, reference book (WIP P1) |
| `7291ce5` | `083cfda` | feat(core): region-scoped perf counters, skew analysis, methodology docs — Phase 0 |
| `c05fdfe` | `c326388` | docs(exp): E0 results — timer cost, measurement floor, cross-core skew |
| `5c1a4d7` | `ec7c42d` | Update plan.md and checkbox.md — E1-driven P2 choices (sweep ladder W in E2, open-addressing ref map) |
| `e512043` | `01ab289` | feat(protocol): ITCH decoder, reference book, differential + property tests, E1 tool — Phase 1 |
| `a4102bd` | `ff265fa` | style: apply clang-format 21 to all C++ sources; CI format job and tidy config |
| `ad475bf` | `b12c1ad` | docs(exp): E1 workload characterization of the 2019-07-30 ITCH day |
| `f52141e` | `e6e6356` | Update plan.md and checkbox.md — P1 complete (E1 written), clang-format/tidy in place |
| `7db1411` | `f7e0ad1` | feat(book): optimized FastBook (pool, intrusive FIFO, level vector, open-addressing ref map), diff_books, e2_book driver — Phase 2 part 1 |

The E2 benchmark run `results/E2-book/20261004T102616Z` started before the rewrite: its
binaries were built from old `7db1411` (= new `f7e0ad1`), and its "after" environment block
records the post-rewrite HEAD.
