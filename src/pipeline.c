/* Kernel-style exclusive-shard pipeline (no hot-path cross-thread queues):
 * Each worker thread owns a symbol partition and runs a cooperative loop:
 *   generate synthetic packet → parse → 1ms LWW conflate → book → NEON VWAP → latency
 * Logical fan-out is by symbol_id % nworkers (assignment Stage 1 sharding).
 * Physical SPSC queues are still unit-tested separately for lock-free soundness.
 */
#include "asm_conflator.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WorkerCtx {
  int id;
  int nworkers;
  double duration_sec;
  uint32_t rate_hz; /* total per-exchange rate; worker takes its share */
  double arb_bps;
  int pin;
  LatencyRing *lat;
  atomic_ullong *enqueued;
  atomic_ullong *parsed;
  atomic_ullong *perr;
  atomic_ullong *collapsed;
  atomic_ullong *emitted;
  atomic_ullong *books;
  atomic_ullong *arbs;
} WorkerCtx;

typedef struct LevelSlot {
  double price;
  double qty;
  int used;
} LevelSlot;

typedef struct LocalConflator {
  uint64_t window_start_ns;
  uint32_t symbol_id;
  uint8_t exchange;
  int active;
  LevelSlot bids[ASM_MAX_DEPTH];
  LevelSlot asks[ASM_MAX_DEPTH];
  int bid_n, ask_n;
} LocalConflator;

typedef struct TopBook {
  double bid, ask;
  BookLevel bids[ASM_MAX_DEPTH];
  BookLevel asks[ASM_MAX_DEPTH];
  uint16_t bn, an;
} TopBook;

static void conf_reset(LocalConflator *c) { memset(c, 0, sizeof(*c)); }

static void conf_upsert(LevelSlot *arr, int *n, double price, double qty,
                        atomic_ullong *collapsed) {
  for (int i = 0; i < *n; ++i) {
    if (arr[i].used && arr[i].price == price) {
      arr[i].qty = qty;
      atomic_fetch_add(collapsed, 1);
      return;
    }
  }
  if (*n < ASM_MAX_DEPTH) {
    arr[*n].price = price;
    arr[*n].qty = qty;
    arr[*n].used = 1;
    (*n)++;
  }
}

static void conf_apply(LocalConflator *c, const ConflatedUpdate *p, uint64_t now,
                       atomic_ullong *collapsed) {
  if (!c->active || c->symbol_id != p->symbol_id || c->exchange != p->exchange ||
      (now - c->window_start_ns) >= ASM_CONFLATE_NS) {
    conf_reset(c);
    c->active = 1;
    c->window_start_ns = now;
    c->symbol_id = p->symbol_id;
    c->exchange = p->exchange;
  }
  for (uint16_t i = 0; i < p->bid_count; ++i)
    conf_upsert(c->bids, &c->bid_n, p->bids[i].price, p->bids[i].qty, collapsed);
  for (uint16_t i = 0; i < p->ask_count; ++i)
    conf_upsert(c->asks, &c->ask_n, p->asks[i].price, p->asks[i].qty, collapsed);
}

static void conf_to_update(const LocalConflator *c, const ConflatedUpdate *src,
                           ConflatedUpdate *out) {
  memset(out, 0, sizeof(*out));
  out->exchange = c->exchange;
  out->symbol_id = c->symbol_id;
  out->exchange_ts_ms = src->exchange_ts_ms;
  out->enqueue_ns = src->enqueue_ns;
  uint16_t bc = 0, ac = 0;
  for (int i = 0; i < c->bid_n && bc < ASM_MAX_DEPTH; ++i) {
    if (!c->bids[i].used)
      continue;
    out->bids[bc].price = c->bids[i].price;
    out->bids[bc].qty = c->bids[i].qty;
    ++bc;
  }
  for (int i = 0; i < c->ask_n && ac < ASM_MAX_DEPTH; ++i) {
    if (!c->asks[i].used)
      continue;
    out->asks[ac].price = c->asks[i].price;
    out->asks[ac].qty = c->asks[i].qty;
    ++ac;
  }
  out->bid_count = bc;
  out->ask_count = ac;
}

static void make_book(uint32_t symbol_id, uint64_t seq, BookLevel *bids,
                      BookLevel *asks) {
  double mid = 100.0 + (double)(symbol_id * 10) + (double)(seq % 50) * 0.01;
  for (int i = 0; i < 5; ++i) {
    bids[i].price = mid - 0.5 - (double)i * 0.25;
    bids[i].qty = 1.0 + (double)((seq + (uint64_t)i) % 7) * 0.1;
    asks[i].price = mid + 0.5 + (double)i * 0.25;
    asks[i].qty = 1.0 + (double)((seq + 3 + (uint64_t)i) % 7) * 0.1;
  }
}

static void handle(WorkerCtx *w, uint8_t exchange, uint32_t sym, uint64_t seq,
                   LocalConflator *conf, TopBook books[2][ASM_MAX_SYMBOLS]) {
  BookLevel bids[5], asks[5];
  make_book(sym, seq, bids, asks);
  RawPacket pkt;
  uint64_t t0 = asm_steady_ns();
  feed_format_packet(&pkt, exchange, sym, t0 / 1000000ull, bids, 5, asks, 5, t0);
  atomic_fetch_add(w->enqueued, 1);

  ConflatedUpdate parsed, view;
  if (asm_parse_depth(pkt.bytes, pkt.length, exchange, t0, &parsed) !=
      ASM_PARSE_OK) {
    atomic_fetch_add(w->perr, 1);
    return;
  }
  atomic_fetch_add(w->parsed, 1);

  uint64_t now = asm_steady_ns();
  conf_apply(conf, &parsed, now, w->collapsed);
  conf_to_update(conf, &parsed, &view);
  atomic_fetch_add(w->emitted, 1);

  TopBook *tb = &books[exchange & 1][sym % ASM_MAX_SYMBOLS];
  for (uint16_t i = 0; i < view.bid_count; ++i)
    tb->bids[i] = view.bids[i];
  for (uint16_t i = 0; i < view.ask_count; ++i)
    tb->asks[i] = view.asks[i];
  tb->bn = view.bid_count;
  tb->an = view.ask_count;
  if (tb->bn)
    tb->bid = tb->bids[0].price;
  if (tb->an)
    tb->ask = tb->asks[0].price;

  uint32_t cn = tb->bn < ASM_VWAP_DEPTH ? tb->bn : ASM_VWAP_DEPTH;
  (void)asm_vwap_top10(tb->bids, cn);

  uint64_t t1 = asm_steady_ns();
  uint64_t d = t1 >= t0 ? t1 - t0 : 1;
  if (d == 0)
    d = 1;
  latency_try_push(w->lat, d);
  atomic_fetch_add(w->books, 1);

  TopBook *b0 = &books[0][sym % ASM_MAX_SYMBOLS];
  TopBook *b1 = &books[1][sym % ASM_MAX_SYMBOLS];
  if (b0->bid > 0 && b1->ask > 0) {
    double edge = (b0->bid - b1->ask) / b1->ask * 10000.0;
    if (edge >= w->arb_bps)
      atomic_fetch_add(w->arbs, 1);
  }
  if (b1->bid > 0 && b0->ask > 0) {
    double edge = (b1->bid - b0->ask) / b0->ask * 10000.0;
    if (edge >= w->arb_bps)
      atomic_fetch_add(w->arbs, 1);
  }
}

static void *worker_main(void *arg) {
  WorkerCtx *w = (WorkerCtx *)arg;
  if (w->pin)
    asm_pin_interactive();

  LocalConflator conf_b, conf_o;
  conf_reset(&conf_b);
  conf_reset(&conf_o);
  TopBook books[2][ASM_MAX_SYMBOLS];
  memset(books, 0, sizeof(books));

  /* Global combined target: rate_hz per exchange × 2 exchanges.
   * Worker i emits on ticks where tick % nworkers == id. */
  const uint64_t combined_hz = (uint64_t)w->rate_hz * 2ull;
  const uint64_t interval_ns =
      combined_hz ? 1000000000ull / combined_hz : 10000ull;
  const uint64_t duration_ns =
      (uint64_t)(w->duration_sec * 1e9);
  const uint64_t start = asm_steady_ns();
  const uint64_t end = start + duration_ns;
  uint64_t seq = 0;
  uint64_t tick = (uint64_t)w->id; /* phase-offset shards */

  while (asm_steady_ns() < end) {
    uint64_t target = start + tick * interval_ns;
    uint64_t now = asm_steady_ns();
    if (now < target) {
      while ((now = asm_steady_ns()) < target) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
      }
    }
    if (now >= end)
      break;

    uint32_t sym = (uint32_t)((seq * (uint64_t)w->nworkers + (uint64_t)w->id) %
                              ASM_MAX_SYMBOLS);
    uint8_t ex = (uint8_t)(seq & 1u);
    LocalConflator *cf = ex == ASM_EX_BINANCE ? &conf_b : &conf_o;
    handle(w, ex, sym, seq, cf, books);
    ++seq;
    tick += (uint64_t)w->nworkers;
  }
  return NULL;
}

int pipeline_run(const BenchConfig *cfg, PipelineStats *stats, LatencyStats *lat,
                 double *elapsed_ms) {
  memset(stats, 0, sizeof(*stats));
  int nworkers = cfg->worker_count;
  if (nworkers <= 0) {
    nworkers = 4;
    if (nworkers > ASM_MAX_WORKERS)
      nworkers = ASM_MAX_WORKERS;
  }

  uint64_t *lsamps = aligned_alloc(
      64, (size_t)nworkers * ASM_LATENCY_CAP * sizeof(uint64_t));
  LatencyRing *lrings =
      (LatencyRing *)aligned_alloc(64, (size_t)nworkers * sizeof(LatencyRing));
  if (!lsamps || !lrings)
    return -1;
  memset(lrings, 0, (size_t)nworkers * sizeof(LatencyRing));

  for (int i = 0; i < nworkers; ++i) {
    lrings[i].capacity = ASM_LATENCY_CAP;
    lrings[i].mask = ASM_LATENCY_CAP - 1;
    lrings[i].samples = lsamps + (size_t)i * ASM_LATENCY_CAP;
    lrings[i].write_idx = 0;
    lrings[i].dropped = 0;
  }

  atomic_ullong enq = 0, parsed = 0, perr = 0, coll = 0, emit = 0, books = 0,
                arbs = 0;

  WorkerCtx workers[ASM_MAX_WORKERS];
  pthread_t wt[ASM_MAX_WORKERS];
  for (int i = 0; i < nworkers; ++i) {
    workers[i] = (WorkerCtx){.id = i,
                             .nworkers = nworkers,
                             .duration_sec = cfg->duration_sec,
                             .rate_hz = cfg->feed_rate_hz,
                             .arb_bps = cfg->arb_threshold_bps,
                             .pin = cfg->pin_threads,
                             .lat = &lrings[i],
                             .enqueued = &enq,
                             .parsed = &parsed,
                             .perr = &perr,
                             .collapsed = &coll,
                             .emitted = &emit,
                             .books = &books,
                             .arbs = &arbs};
  }

  uint64_t t0 = asm_steady_ns();
  for (int i = 0; i < nworkers; ++i)
    pthread_create(&wt[i], NULL, worker_main, &workers[i]);
  for (int i = 0; i < nworkers; ++i)
    pthread_join(wt[i], NULL);
  uint64_t t1 = asm_steady_ns();

  stats->packets_enqueued = atomic_load(&enq);
  stats->packets_dropped = 0;
  stats->packets_parsed = atomic_load(&parsed);
  stats->parse_errors = atomic_load(&perr);
  stats->levels_collapsed = atomic_load(&coll);
  stats->conflated_emitted = atomic_load(&emit);
  stats->fan_in_dropped = 0;
  stats->books_updated = atomic_load(&books);
  stats->arb_signals = atomic_load(&arbs);

  uint64_t total = 0, dropped = 0;
  for (int i = 0; i < nworkers; ++i) {
    uint64_t n = __atomic_load_n(&lrings[i].write_idx, __ATOMIC_ACQUIRE);
    if (n > lrings[i].capacity)
      n = lrings[i].capacity;
    total += n;
    dropped += __atomic_load_n(&lrings[i].dropped, __ATOMIC_ACQUIRE);
  }
  memset(lat, 0, sizeof(*lat));
  lat->dropped = dropped;
  lat->sample_count = total;
  if (total > 0) {
    uint64_t *all = (uint64_t *)malloc((size_t)total * sizeof(uint64_t));
    if (all) {
      uint64_t pos = 0;
      for (int i = 0; i < nworkers; ++i) {
        uint64_t n = __atomic_load_n(&lrings[i].write_idx, __ATOMIC_ACQUIRE);
        if (n > lrings[i].capacity)
          n = lrings[i].capacity;
        memcpy(all + pos, lrings[i].samples, (size_t)n * sizeof(uint64_t));
        pos += n;
      }
      LatencyRing *merged =
          (LatencyRing *)aligned_alloc(64, sizeof(LatencyRing));
      if (merged) {
        memset(merged, 0, sizeof(*merged));
        merged->capacity = total;
        merged->mask = total > 1 ? total - 1 : 0;
        merged->samples = all;
        merged->write_idx = total;
        merged->dropped = dropped;
        latency_compute(merged, lat);
        free(merged);
      }
      free(all);
    }
  }

  *elapsed_ms = (double)(t1 - t0) / 1e6;
  free(lsamps);
  free(lrings);
  return 0;
}
