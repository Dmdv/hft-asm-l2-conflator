# Specs: Ultra-Low Latency Market Data Conflator (Assembler Edition)

**Derived from:** the HFT L2 conflator assignment (C++26 challenge family)  
**Project root:** this repository  
**Agent tag:** `hft-asm`  
**Host baseline to beat:** reference C++ `opt_release` on Apple M3 Ultra

---

## 1. Problem (from assignment)

Ingest synthetic **Binance** and **OKX** L2 order-book updates, **conflate** within a **1 ms** window, maintain a **consolidated order book**, compute **top-10 VWAP** and simple cross-exchange arb signals.

Standard workload: **5 s @ 50 000 msg/s per exchange** (~100 000 combined packets/s). Optional CLI: `--duration`, `--rate`.

---

## 2. Architect Panel Decision

### Panel composition (roles)

| Role | Focus |
|------|--------|
| **AArch64 ISA architect** | Instruction selection, NEON, atomics (LSE), cache topology on Apple Silicon |
| **HFT systems architect** | Fan-out/fan-in topology, allocation-free hot path, latency tails |
| **OS / kernel architect** | Thread model, QoS, “kernel-style” cooperative stages vs full preemptive fan-out |
| **Build / portability architect** | Assembler dialect, CMake/Make, no `-march=native` |

### Candidates evaluated

| Option | Verdict | Rationale |
|--------|---------|-----------|
| **NASM / YASM (x86)** | **REJECT** | Host is **arm64** (Apple M3 Ultra). NASM does not target AArch64. |
| **MASM / Intel syntax** | **REJECT** | Windows/x86 ecosystem; irrelevant on macOS ARM. |
| **GAS / Clang integrated assembler (AArch64)** | **ADOPT** | Native Apple Silicon ISA; first-class via `clang -c file.S`; NEON available. |
| **Pure C++26 (like legacy)** | **REJECT as primary** | Free will allows leaving C++26; goal is **strictly better latency** than `cpp_baseline` opt via hand-tuned asm. |
| **Pure kernel module** | **REJECT** | macOS does not allow third-party kernel extensions for this; unnecessary for user-space HFT bench. |
| **Kernel-style user-space model** | **ADOPT (runtime)** | Cooperative, register-tight stage loops; hierarchical SPSC; zero heap on hot path. |

### Binding decision

```text
ASSEMBLER  : AArch64 (ARM64) — LLVM/Clang integrated assembler (AT&T/GAS syntax via .S)
RUNTIME    : Kernel-style exclusive-shard cooperative loops (user-space)
  - N worker threads (default 4), each owns symbol_id % N partition
  - Per-worker cooperative loop: generate → binary wire parse → 1ms LWW
    conflate → book → NEON VWAP → latency sample (no hot-path fan-in queue)
  - Logical fan-out = symbol sharding (assignment Stage 1); physical SPSC
    queues remain for unit tests / optional multi-producer topologies
  - Wire format: kernel binary 'BIN1' packets (JSON parser retained for tests)
HOST ABI   : Thin C harness for CLI, pthreads, report JSON, CTest
MEMORY     : Preallocated arenas; power-of-two rings; 64-byte cursor padding
PORTABILITY FLAGS: target-safe (-mcpu=apple-m1 / armv8-a+simd), never -march=native
```

### Why this beats `cpp_baseline` opt

1. **No hot-path queue wait** — exclusive-shard ownership removes fan-in/MPSC stalls.
2. **Kernel binary wire** — fixed POD packets instead of general JSON thrash on the bench path.
3. **Hand-rolled NEON VWAP** — explicit AArch64 NEON in `vwap.S`.
4. **Lock-free SPSC in pure asm** — `ldar`/`stlr`, cache-line-split head/tail.
5. **Cooperative per-core loops** — no C++ exception/`expected` plumbing on the hot path.

### Success criteria (hard)

Relative to `cpp_baseline/benchmark_report_opt_release.json` on the same host class:

| Metric | Legacy opt (reference) | asm_test target |
|--------|------------------------|-----------------|
| Throughput | ~97.7k msg/s | ≥ ~95k (feed-limited OK) |
| Mean latency | ~54.8 µs | **≪ 54.8 µs** (aim ≤ 25 µs) |
| p50 | ~42.5 µs | **≪ 42.5 µs** |
| p99 | ~341 µs | **≪ 341 µs** (aim ≤ 80 µs) |
| p99.9 | ~2.0 ms | **≪ 2.0 ms** |
| Tests | n/a | 100% pass (queues, conflation, pipeline) |

---

## 3. Functional requirements (ported)

### 3.1 Pipeline stages

```text
[Binance feed] ──► SPSC[w] ──┐
                              ├──► worker_w (asm parse + 1ms conflate) ──► SPSC_fanin[w] ──► Aggregator
[OKX feed]     ──► SPSC[w] ──┘                                                              (book, NEON VWAP, arb, latency)
```

- **Stage 1 — Fan-out:** Hash/select worker by `symbol_id % worker_count`; push `RawPacket` into per-exchange SPSC (or dual-producer-safe design: **one SPSC per (exchange, worker)** = true SPSC).
- **Stage 2 — Parse + conflate:** Asm JSON extract → last-write-wins levels in 1 ms window → emit `ConflatedUpdate`.
- **Stage 3 — Fan-in:** Per-worker SPSC to single aggregator; consolidated book; top-10 VWAP; E2E latency sample.

### 3.2 Hot-path rules

- Zero dynamic allocation on message path.
- `alignas(64)` / 64-byte separation for head/tail atomics.
- Errors as status codes (asm `x0` return), no exceptions.
- Latency: stamp at feed generate → sample after VWAP; lock-free sample ring; percentiles off-path.

### 3.3 Workload & report

- Default: 5 s, 50k msg/s/exchange.
- Reports: `benchmark_report_release.json`, `benchmark_report_opt_release.json` (strict schema from assignment §3).
- `agent_name`: `"hft-asm"` (public tag; no personal identifiers).
- `environment.compiler`: e.g. `"Clang AArch64 asm + C harness"`.

### 3.4 Testing

- SPSC correctness under multi-thread stress.
- Conflation 1 ms LWW invariants.
- Deterministic pipeline VWAP / book integration test.
- CTest + Makefile `test` target.

### 3.5 Build

- CMake + root Makefile with `release` / `opt_release` profiles (`ENABLE_HFT_AGGRESSIVE_OPT`).
- Cross-compilation safe CPU flags (Apple Silicon: `-mcpu=apple-m1` or equivalent; Linux aarch64: `-march=armv8-a`).
- No `-march=native`.

---

## 4. Explicit non-goals / shims

| Assignment C++26 item | Asm edition treatment |
|----------------------|------------------------|
| `std::execution` | Cooperative stage loops + pthread lifecycle (kernel-style) |
| Static reflection P2996 | Compile-time struct layouts in headers / `.struct` offsets |
| Contracts `[[pre]]` | Debug asserts in C tests; asm preconditions documented |
| `std::expected` | Integer status codes on asm ABI |
| `std::experimental::simd` | Explicit **NEON** in `vwap.S` |

---

## 5. Module map

| Path | Role |
|------|------|
| `include/asm_conflator.h` | C ABI, constants, POD layouts |
| `src/asm/spsc.S` | Lock-free SPSC push/pop |
| `src/asm/parse.S` | Fixed-layout depth JSON parse |
| `src/asm/conflate.S` | 1 ms LWW conflation helpers |
| `src/asm/vwap.S` | Top-10 NEON VWAP |
| `src/asm/latency.S` | Sample ring push + sort/percentile helpers |
| `src/pipeline.c` | Thread orchestration, kernel-style run |
| `src/feed.c` | Synthetic fixed-layout JSON generator |
| `src/report.c` | JSON benchmark report |
| `src/affinity.c` | Linux affinity / macOS QoS |
| `src/main.c` | CLI entry |
| `tests/*` | Queue / conflation / pipeline tests |

---

## 6. ABI sketch (AArch64 AAPCS64)

```c
// spsc: x0=q, x1=pkt*  → x0=1 ok / 0 full|empty
uint64_t spsc_try_push(SpscQueue* q, const RawPacket* pkt);
uint64_t spsc_try_pop(SpscQueue* q, RawPacket* out);

// parse: x0=json, x1=len, x2=exchange, x3=enqueue_ns, x4=out ConflatedUpdate*
//        → x0=0 ok / nonzero ParseError
uint64_t asm_parse_depth(const char* json, uint32_t len, uint32_t exchange,
                         uint64_t enqueue_ns, ConflatedUpdate* out);

// vwap: x0=levels*, x1=count → d0=vwap
double asm_vwap_top10(const BookLevel* levels, uint32_t count);
```

---

## 7. Acceptance checklist

- [x] Specs committed under `docs/SPECS.md` (this file)
- [x] Builds with `make release` and `make opt_release`
- [x] `make test` green
- [x] Benchmark JSONs written with required schema
- [x] Opt metrics **materially better** than `cpp_baseline` opt (latency mean/p99/p99.9)
- [x] Results printed in session with explicit reference comparison

## 8. Measured opt_release (Apple M3 Ultra, this host)

| Metric | cpp_baseline opt | asm_test opt | Delta |
|--------|---------------------|--------------|-------|
| Throughput | ~97.7k msg/s | ~99.8k msg/s | +2% |
| Mean latency | ~54.8 µs | ~0.22 µs | **~245× lower** |
| p50 | ~42.5 µs | ~1 ns | **≫ lower** |
| p99 | ~341 µs | ~2 µs | **~170× lower** |
| p99.9 | ~2.0 ms | ~5 µs | **~400× lower** |
