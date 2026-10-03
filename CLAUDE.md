# Claude Context — Low-Latency Market Data & Execution Engine

## What This Project Is
A Linux C++20 low-latency engine for an MTech placement portfolio. HFT roles are the priority; CPU/SoC systems roles (AMD, Intel, Qualcomm) come second. It consumes real NASDAQ TotalView-ITCH 5.0 data over UDP multicast (MoldUDP64, A/B legs, gap recovery), builds per-symbol order books, and runs a minimal strategy, risk and gateway against a local exchange emulator.

The real deliverable is 10 reproducible, counter-explained experiments (E0–E9). Releases: core checkpoint v0.5 (E0–E4) → v1.0 (+ HFT E5–E6) → v1.1 (+ silicon E7–E9).

The working blueprint is `plan.md`; progress is tracked in `checkbox.md`; full detail lives in `Low_Latency_Cpp_Systems_Engine_Master_Project_Plan_v3.md` (the "master plan").

**Current state:** planning complete, implementation starting.

## Current Phase
**Phase 0: Toolchain and Measurement Infrastructure → E0.** In progress. Blocked on the user installing the missing tools (clang/llvm, cmake, ninja, perf, …); code is written meanwhile.

## Working Agreement with the User (2026-10-04)
- **Who builds:** Claude owns the implementation of the core checkpoint (P0–P4, E0–E4, tag v0.5) end to end. The user is a beginner in HFT/low-latency systems.
- **Deadline:** the user submits a resume to an HFT firm on 2026-10-06. Interviews are expected around early December 2026.
- **Order of work:** (1) finish core v0.5 → (2) write the core resume bullets from measured numbers only → (3) the user studies every part in detail before interviews (see the study-guide task in plan.md). P5+ (v1.0) continues after that.
- **No quality cuts for the deadline.** Do not skip tests, statistics or write-ups to finish faster. Anything not measured by the deadline is described on the resume with neutral wording, never estimated.
- **No workarounds.** If something needs the user (sudo, installs, downloads, accounts, a GitHub remote, hardware) or Claude is stuck, stop and tell the user exactly what to do. Do not substitute pip/conda/vendored tools, skip a step, or fake an input.
- **Explain for a beginner.** Code comments and docs should explain *why*, not only *what*, so the user can learn from them later.

## Recent Decisions
- Adopted master plan v3: HFT-first, depth over breadth, 10 experiments; extensions never gate completion. The v1 and v2 plans were deleted.
- No Nasdaq ITCH data or derived slices are ever committed. Real data comes from `scripts/fetch_itch.sh` with checksums; CI uses synthetic ITCH-format fixtures from `tools/gen_fixture`.
- Strict C++20 (`bswap` helper, no `std::byteswap`). Release preset `-O2 -DNDEBUG` is the only baseline for performance numbers.

## Project Rules (specific to this project)
- **Never fabricate or estimate performance numbers.** README, docs and resume text contain only measured results backed by a result file + env JSON + reproduce command. Before data exists, use neutral wording.
- Every experiment follows the template in master plan §5.3 (including "where the hypothesis was wrong") and the methodology in §9: open-loop load, N ≥ 10 interleaved runs, 95% bootstrap CI.
- Feed-side book = book building only. Matching exists only in the exchange emulator.
- Hot path: zero heap allocations after warm-up, no `reinterpret_cast` of wire bytes, telemetry/logging off the hot path.
- Machine: i5-8250U, SMT siblings n/n+4, 7.6 GiB RAM (stream ITCH, never load a full day), Wi-Fi only (networking over veth + netns; no NIC/wire claims).
- There is one context file: `CLAUDE.md` (the lowercase `claude.md` symlink setup was removed at the user's request).
- Model checking is timeboxed to 2 days. Extensions (AF_XDP, DPDK, SIMD, etc.) only after v1.0.

## Protocols (always follow these)

1. **Plan-first rule**: If a new decision contradicts or changes anything in plan.md, update plan.md FIRST, then update checkbox.md to reflect any new or removed tasks, THEN implement.

2. **Checkbox discipline**: Mark items in checkbox.md as done only after they are fully implemented and working.

3. **Git commits**: Before committing, verify git is configured under the user's name (see Git Protocol below). Commit after:
   - A major feature or step is completed
   - plan.md or checkbox.md are updated
   - The user explicitly asks for a commit

4. **No silent changes**: Never change the direction of the project without updating plan.md first. If something mid-implementation reveals a flaw in the plan, pause, update the plan, then continue.

5. **CLAUDE.md updates**: Only update this file when the plan changes significantly or the user asks for it. Do not regenerate it every session.

## Git Protocol

Before every commit, run:
```bash
git config user.name
git config user.email
```

- If both return values → proceed with the commit.
- If either is empty → stop and ask the user to run:
  `git config --global user.name "Your Name"` and `git config --global user.email "your@email.com"`, then commit.

Commit with `git add .` + `git commit -m "<specific message>"`:
- Plan or checkbox update: `"Update plan.md and checkbox.md — <what changed>"`
- Feature: `"Implement <feature> — Phase <X> step complete"`
- Conventional Commits style is also fine (`feat(book): …`, `perf(memory): …`, `docs(exp): E2 …`).

Never use `--author` or override the git identity.
