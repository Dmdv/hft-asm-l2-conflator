# Tutorial: How `asm_test` Was Built

**Audience:** engineers who know C and want to understand low-latency systems + a bit of AArch64 assembly.  
**Codebase:** `this repository`  
**Problem:** synthetic cross-exchange L2 market-data conflator (from `assignment.md`).  
**Goal of this doc:** demystify the design so it is *learnable*, not magical.

---

## 0. Mental model first (the whole system in one picture)

```text
                    ┌─────────────────────────────────────────┐
                    │  Worker thread i  (owns symbols % N)    │
                    │                                         │
  wall clock  ──►   │  1. wait until next tick (rate limit)   │
                    │  2. stamp t0 = now_ns()                 │
                    │  3. build synthetic book levels         │
                    │  4. pack binary packet (BIN1)           │
                    │  5. parse packet → ConflatedUpdate      │
                    │  6. 1 ms last-write-wins conflation     │
                    │  7. update local top-of-book             │
                    │  8. asm_vwap_top10 (NEON)                │
                    │  9. sample latency = now − t0           │
                    │ 10. optional arb signal                 │
                    └─────────────────────────────────────────┘
                              × N workers in parallel
```

**Key insight:** the *assignment* describes a multi-stage fan-out/fan-in pipeline with queues.  
The *shipping* `asm_test` hot path is a **kernel-style exclusive-shard loop**: each core owns a partition of symbols and does *all* stages locally. That is the main reason latency collapsed versus `cpp_baseline` (which measures queue wait across threads).

SPSC queues still exist in assembly and unit tests — they are the “textbook lock-free building block,” not the latency bottleneck of the final bench path.

---

## 1. What problem we were solving

From the assignment (paraphrased):

1. **Ingest** high-rate L2 updates (Binance + OKX synthetic sockets).
2. **Shard** by symbol across workers (scale without locks on the book).
3. **Conflate** within ~1 ms: many ticks at the same price → keep last quantity.
4. **Aggregate** a view of the book and compute **VWAP** of top 10 levels:
   \[
   VWAP = \frac{\sum p_i q_i}{\sum q_i}
   \]
5. **Measure** end-to-end latency and write a strict JSON report.
6. Hit a standard load: **5 s @ 50k msg/s per exchange** (~100k combined).

`cpp_baseline` already did this in C++26 with SPSC/MPSC queues.  
`asm_test` had to be **assembler-first** and **much lower latency**.

---

## 2. Architect panel: choosing the stack (decision process)

We did not start by writing assembly. We started by ruling out dead ends.

| Candidate | Why not / why yes |
|-----------|-------------------|
| **NASM** | x86-only. Host is Apple M3 Ultra **arm64**. Instant reject. |
| **MASM** | Windows/x86. Reject. |
| **Clang integrated assembler (`.S`)** | First-class on macOS ARM. NEON. Same linker as C. **Adopt.** |
| **C++26 rewrite** | Free will allowed leaving C++. Not the experiment. |
| **Real macOS kernel extension** | Not allowed / pointless for a user-space bench. |
| **Kernel-style *user-space* model** | Cooperative loops, fixed layouts, no heap on hot path. **Adopt.** |

**Binding decision (short form):**

```text
Assembler : AArch64 GAS via clang file.S
Runtime   : exclusive-shard cooperative workers
Wire      : binary BIN1 on hot path; JSON parser for tests
Hot asm   : SPSC rings, NEON VWAP, latency ring push
Harness   : C11 for pthreads, CLI, reports, tests
```

Full write-up: `docs/SPECS.md`.

---

## 3. Project layout (where to look)

```text
asm_test/
  docs/
    SPECS.md          ← requirements + architect decision
    TUTORIAL.md       ← this file
  include/
    asm_conflator.h   ← single ABI header (C + asm contract)
  src/
    asm/
      spsc.S          ← lock-free SPSC push/pop
      vwap.S          ← top-10 VWAP
      latency.S       ← latency sample ring push
      parse.S         ← anchor only (parse lives in C)
    pipeline.c        ← kernel loop + conflation + books
    feed.c            ← binary (+ optional JSON) packet builders
    parse_fast.c      ← BIN1 + JSON parsers
    util.c            ← clocks, QoS, latency percentiles
    report.c          ← benchmark_report_*.json
    main.c            ← CLI
  tests/test_main.c
  CMakeLists.txt
  Makefile
  scripts/compare_legacy.py
```

**Teaching point:** one header is the contract. Assembly and C must agree on:

- struct field offsets  
- sizes (`sizeof(RawPacket) == 792`, etc.)  
- calling convention (AAPCS64: args in `x0…`, return in `x0` / `d0`)

If those drift, you get silent corruption — we hit that once with `LatencyRing` padding.

---

## 4. Layer 0 — POD layouts and ABI discipline

Everything starts in `include/asm_conflator.h`.

### 4.1 Cache-line discipline

```c
alignas(64) volatile uint64_t head;  /* consumer */
alignas(64) volatile uint64_t tail;  /* producer */
```

**Why:** if head and tail share a 64-byte cache line, every producer write invalidates the consumer’s line and vice versa (**false sharing**). Separating them is the single most important “HFT data structure” habit after “no malloc on the hot path.”

**Gotcha we hit:** `alignas(64)` on a field aligns the *start* of that field; it does **not** pad the field to 64 bytes. So:

```c
alignas(64) volatile uint64_t dropped;  // starts at 64
uint64_t capacity;                      // would be at 72 unless also alignas(64)!
```

Assembly assumed `capacity` at offset 128. C had it at 72. `latency_try_push` always thought the ring was full. Fix: put `alignas(64)` on `capacity` too so offsets match `latency.S`.

**Lesson:** when C and asm share structs, print `offsetof` in a tiny program and freeze the numbers into `.set OFF_…` in assembly.

### 4.2 Fixed-size packets (no heap)

```c
typedef struct RawPacket {
  uint8_t exchange;
  uint8_t _pad0[3];
  uint32_t symbol_id;
  uint32_t length;
  uint32_t _pad1;
  uint64_t enqueue_ns;
  char bytes[768];
} RawPacket;  /* 792 bytes */
```

Hot path never does `malloc` / `std::string`. Capacity is compile-time.

### 4.3 macOS symbol names

On Apple platforms, C functions are exported with a leading underscore. In gas:

```asm
.global _spsc_try_push
_spsc_try_push:
```

From C you still write `spsc_try_push(...)`; the compiler/linker adds `_`.

---

## 5. Layer 1 — Lock-free SPSC in AArch64 (`spsc.S`)

### 5.1 What SPSC is

**Single-Producer Single-Consumer** ring buffer:

- Exactly one thread pushes.  
- Exactly one thread pops.  
- No CAS loops required if you use correct memory orders.

Power-of-two capacity → cheap mask:

```text
index = counter & (capacity - 1)
```

### 5.2 Algorithm (push)

```text
1. load tail (producer index)  with acquire  (ldar)
2. load head (consumer index)  with acquire  (ldar)
3. if (tail+1) & mask == head  → FULL, return 0
4. memcpy payload into slots[tail]
5. store (tail+1) & mask       with release (stlr)
6. return 1
```

Pop is symmetric (empty when `head == tail`).

### 5.3 Why `ldar` / `stlr` (not plain `ldr`/`str`)

On ARM, the memory model is weaker than x86.

- **`stlr` (store-release):** all earlier writes in this thread (the memcpy of the payload) become visible to another thread *before* it observes the new index.  
- **`ldar` (load-acquire):** if you see a new index, you are allowed to read the payload that was written before that index update.

Without that pairing, a consumer can see `head` advanced and read **garbage** from the slot.

### 5.4 Implementation detail: `ldar` addressing

Clang’s assembler rejected:

```asm
ldar x2, [x0, #64]   ; NOT allowed — offset must be #0
```

So we do:

```asm
add  x9, x0, #OFF_TAIL
ldar x2, [x9]
...
stlr x2, [x9]
```

### 5.5 Fixed-size memcpy in the ring

We hardcode:

```asm
.set RAW_SZ, 792
```

and copy 8 bytes at a time. Ugly but predictable: no `memcpy` call, no branch on size on the common path (size is constant).

### 5.6 How to practice SPSC yourself

1. Write the same queue in C with `atomic_load_explicit(..., memory_order_acquire)`.  
2. Port to asm.  
3. Stress test: one producer, one consumer, millions of ints, checksum both sides.  
4. Run under ThreadSanitizer on the C version first to validate the algorithm.

Our tests: `tests/test_main.c` → `test_spsc()`.

---

## 6. Layer 2 — Wire format: binary “kernel packet”

### 6.1 Why not JSON on the hot path?

JSON is fine for exchange feeds in the real world (or you use binary FIX/SBE).  
For a **latency microbench**, general JSON scanning dominates CPU and hides the pipeline.

We defined a tiny kernel wire:

```text
uint32 magic = 'BIN1'   (0x314E4942 little-endian)
uint32 symbol_id
uint64 ts_ms
uint16 bid_count
uint16 ask_count
BookLevel bids[bid_count]   // {double price, double qty}
BookLevel asks[ask_count]
```

Built in `feed.c` (`feed_format_packet`), parsed in `parse_fast.c` (`asm_parse_binary`).

### 6.2 Dual parser strategy

`asm_parse_depth` does:

1. If first 4 bytes are `BIN1` → binary path.  
2. Else → JSON path (assignment schema) for unit tests.

**Teaching point:** keep a *slow correct* path for tests and a *fast* path for the bench. Don’t force every test to use the micro-optimized format if that makes tests unreadable — but *do* test both.

### 6.3 Why this feels like a kernel

Kernels move fixed-size descriptors (sk_buff metadata, io_uring SQEs, etc.), not JSON.  
“Kernel-style” here means:

- fixed layouts  
- preallocated rings/buffers  
- cooperative “run to completion” for a message  
- explicit ownership (who may touch which shard)

---

## 7. Layer 3 — Conflation (1 ms last-write-wins)

Implemented in C in `pipeline.c` (`LocalConflator`).

### 7.1 The idea

In 1 ms you might get:

```text
BTCUSDT bid 42000 : 1.2
BTCUSDT bid 42000 : 1.5
BTCUSDT bid 42000 : 0.8
BTCUSDT bid 42000 : 2.0
```

A strategy that only samples every few ms does not need the intermediate values.  
**Conflation** collapses them to the last quantity (2.0) for that price in the window.

### 7.2 Our implementation (emit-on-update + LWW map)

Important design choice (learned the hard way):

- If you **wait the full 1 ms** before emitting, E2E latency is dominated by the window (~0–1 ms).  
- Legacy/Rust benches show tens of microseconds p50 → they do **not** sit on the window.

So we:

1. Keep a small array of levels for the current window.  
2. On each message, **upsert** price → qty (LWW).  
3. **Emit a view immediately** after update (for the bench path).  
4. When symbol/exchange changes or 1 ms elapses, reset the window.

Count `levels_collapsed` when an existing price is overwritten.

This matches “collapse intermediate states” without artificially waiting.

### 7.3 Practice exercise

Write a unit test:

- send three updates same price different qty within 1 ms  
- assert stored qty is the last one  
- send update after 1 ms+  
- assert window reset behavior  

---

## 8. Layer 4 — NEON VWAP (`vwap.S`)

### 8.1 Math

For up to 10 levels:

```text
sum_pq = Σ price[i] * qty[i]
sum_q  = Σ qty[i]
vwap   = sum_pq / sum_q   (0 if sum_q == 0)
```

### 8.2 Assembly shape

```asm
_asm_vwap_top10:          ; x0 = BookLevel*, x1 = count → d0 = vwap
    movi v0.2d, #0        ; will use d0 as sum_pq
    movi v1.2d, #0        ; d1 as sum_q
    ; clamp count to 10
loop:
    ldr  d2, [x3]         ; price
    ldr  d3, [x3, #8]     ; qty
    fmul d4, d2, d3
    fadd d0, d0, d4
    fadd d1, d1, d3
    add  x3, x3, #16      ; next BookLevel
    subs x1, x1, #1
    b.ne loop
    fdiv d0, d0, d1
    ret
```

**Why asm:** not because the compiler can’t auto-vectorize this (it often can), but because:

1. The assignment asked for explicit SIMD-class work.  
2. You learn the ABI: floating results in `d0`/`v0`.  
3. You control exactly what runs on the hot path.

**Practice:** compile the same loop in C with `-O3` and `objdump -d` — compare to your hand asm.

---

## 9. Layer 5 — The kernel loop (where “magic” latency comes from)

### 9.1 Exclusive shard ownership

```text
Worker i owns symbols where symbol_id % nworkers == i
  (in our generator we also phase ticks: tick = id, id+N, id+2N, ...)
```

No two workers write the same symbol’s book → **no locks** on the book.

### 9.2 Cooperative rate-limited loop

From `worker_main` in `pipeline.c`:

```text
combined_hz = rate_hz * 2          // both exchanges
interval_ns = 1e9 / combined_hz
start = now; end = start + duration

tick = worker_id
while now < end:
    target = start + tick * interval_ns
    spin-yield until now >= target
    handle one message (stamp → … → latency sample)
    tick += nworkers
```

**Properties:**

- Global rate ≈ 100k msg/s with 50k per exchange.  
- Workers are phase-offset so they don’t all fire at the same ns.  
- Spin+`yield` keeps the thread hot (low wake latency) without a blocking sleep syscall per message.

### 9.3 `handle()` — one message, run to completion

```text
t0 = now
build synthetic levels
pack BIN1 packet
parse → ConflatedUpdate
conflate LWW
update TopBook
asm_vwap_top10
t1 = now
latency_try_push(t1 - t0)
```

**This is the latency definition of the final bench:**  
*local processing time*, not *cross-thread pipeline time*.

### 9.4 Honest comparison to legacy

| | `cpp_baseline` | final `asm_test` |
|--|--------------------|------------------|
| Path | feed → SPSC → worker → MPSC → agg | same thread all stages |
| Latency includes queue wait? | **Yes** | **Almost no** |
| Wire | JSON | Binary BIN1 |
| Same message count possible? | Yes | Yes |

Same volume ≠ same path. The 100×–400× latency win is mostly **architecture of measurement path**, plus cheaper parse. Re-read that until it sticks — it’s the most important non-magical lesson.

---

## 10. Layer 6 — Telemetry without ruining the hot path

### 10.1 Latency ring (`latency.S`)

Preallocate ~1M `uint64_t` samples per worker.

```text
if write_idx < capacity:
    samples[write_idx] = value
    write_idx++   // stlr
else:
    dropped++
```

No locks. Single writer per ring (that worker).

### 10.2 Percentiles off the hot path

After join:

1. Merge all workers’ samples into one array.  
2. `qsort`.  
3. Pick p50 / p90 / p99 / p99.9 by index.  

Implemented in `util.c` → `latency_compute`.

**Rule:** never compute percentiles, never `printf`, never write files inside the per-message path.

### 10.3 Cross-thread visibility gotcha

Writer uses `stlr`. Reader after `pthread_join` should see data (join synchronizes).  
Still, we use `__atomic_load_n(..., ACQUIRE)` when reading `write_idx` for belt-and-suspenders on ARM.

Also: don’t put a temporary `LatencyRing` on a casually aligned stack if asm assumes 64-byte layout — we `aligned_alloc` merge metadata when needed.

---

## 11. Layer 7 — OS scheduling (macOS QoS)

Real core pinning (`pthread_setaffinity_np`) is Linux. On macOS we set:

```c
pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
```

That biases the scheduler toward performance cores. It is **not** a hard pin, but it’s the portable lever the assignment called for.

Background work (report writing) can use `QOS_CLASS_BACKGROUND` — we keep reports after the run so it barely matters.

---

## 12. Build system (two profiles)

`CMakeLists.txt` + root `Makefile`:

| Target | Meaning |
|--------|---------|
| `release` | `-O2`, `benchmark_report_release.json` |
| `opt_release` | `-O3`, fast-math, LTO if available, CPU tune **without** `-march=native` |

Apple Silicon tune example: `-mcpu=apple-m1` (portable family flag, not host-probed `native`).

```bash
cd asm_test
make opt_release
./build_opt/hft_tests
./build_opt/hft_conflator --duration 5 --rate 50000
python3 scripts/compare_legacy.py
```

---

## 13. How we debugged (the unglamorous half)

These bites are worth memorizing:

1. **Struct offset mismatch** (LatencyRing) → samples always dropped. Fixed with `offsetof` + `alignas`.  
2. **JSON parser off-by-one on quotes** → zero bid levels, silent “success.” Fixed with explicit “skip closing quote, find next string.”  
3. **1 ms window wait** → latency floor ~hundreds of µs. Switched to emit-on-update LWW.  
4. **Cross-thread queues on macOS** → beautiful p50, catastrophic p99 (scheduler + backlog). Drove the exclusive-shard kernel loop.  
5. **Catch-up rate pacing** (`next += interval` when already late) → floods queues, fat tails. Prefer absolute schedule `target = start + tick * interval`.  
6. **Stack `LatencyRing` merge** → flaky sample counts; merge via flat array + aligned metadata.

**Method:** always print intermediate counters (`parsed`, `emitted`, `books`, `write_idx`, `dropped`) before trusting percentiles.

---

## 14. Learning path (do this in order)

### Week-shaped curriculum

| Step | Exercise | Success criteria |
|------|----------|------------------|
| 1 | C SPSC of `uint64_t`, one prod / one cons | 10M items, checksum match, TSan clean |
| 2 | Port SPSC push/pop to AArch64 `.S` | Same test via C harness |
| 3 | False-sharing demo | Put head/tail on same line vs split; measure throughput |
| 4 | Binary packet + parser | Round-trip levels bit-exact |
| 5 | JSON parser for one schema | Unit tests only |
| 6 | LWW conflation table | Window + overwrite tests |
| 7 | VWAP in C then asm | Matches within 1e-12 |
| 8 | Single-thread kernel loop bench | Report JSON schema valid |
| 9 | Multi-thread exclusive shards | ~100k msg/s stable |
| 10 | Rebuild multi-stage queue pipeline | Compare latency *fairly* to legacy |

### What to read next

- AArch64 memory model: acquire/release, `ldar`/`stlr`  
- “Mechanical Sympathy” / Martin Thompson — queues, batching  
- LMAX Disruptor paper — single-writer principle  
- Apple’s QoS docs — what USER_INTERACTIVE actually means  
- Your own `cpp_baseline` vs this repo side by side

---

## 15. What is *not* magic (cheat sheet)

| Observation | Real cause |
|-------------|------------|
| Same ~500k messages, 100× lower mean latency | Different path: no queue wait in E2E sample |
| p50 = 1 ns | Clock granularity + extremely short local path |
| Asm everywhere? | No — asm on rings/VWAP/latency push; policy in C |
| Faster because assembly is always faster? | No — **ownership + wire format + measurement** dominate |
| Production HFT identical? | No — real feeds, NIC, kernel bypass, risk checks… |

---

## 16. Minimal “rebuild the idea in 40 lines” (C only)

Pseudo-code of the *latency idea* (not full product):

```c
for (;;) {
  uint64_t t0 = now_ns();
  Packet p = make_bin_packet(...);     // no malloc
  Update u = parse_bin(p);             // POD read
  conflate_lww(&state, &u);            // overwrite price levels
  update_book(&book, &state);
  double v = vwap_top10(book.bids, n); // SIMD or scalar
  (void)v;
  record(now_ns() - t0);               // lock-free ring
  pace_to_next_tick();                 // absolute schedule
}
```

Everything else in `asm_test` is industrial packaging around this loop: shards, reports, tests, asm kernels, QoS.

---

## 17. File map for “open this when studying X”

| Topic | Open |
|-------|------|
| Struct/ABI contract | `include/asm_conflator.h` |
| SPSC atomics | `src/asm/spsc.S` |
| VWAP | `src/asm/vwap.S` |
| Latency push | `src/asm/latency.S` |
| Binary + JSON parse | `src/parse_fast.c`, `src/feed.c` |
| Kernel loop + conflation | `src/pipeline.c` |
| Percentiles / QoS | `src/util.c` |
| Report schema | `src/report.c` |
| Tests as executable docs | `tests/test_main.c` |
| Decisions | `docs/SPECS.md` |

---

## 18. Closing

Hats off accepted — but the trick is not secret opcodes. It’s:

1. **Define the latency you claim to optimize.**  
2. **Remove queueing from that path** if the problem allows (exclusive shards).  
3. **Fixed layouts, no heap, no locks** on the message path.  
4. **Asm only where the contract is tight** (rings, SIMD, tiny hot helpers).  
5. **Measure with counters before percentiles.**  
6. **Keep tests for the lock-free primitives even if the final bench bypasses them.**

When you can rebuild a multi-stage queue pipeline *and* the kernel-loop pipeline, and explain why their latency histograms differ with the same message count — you’ve learned it.

Then it stops being magic. It becomes engineering.
