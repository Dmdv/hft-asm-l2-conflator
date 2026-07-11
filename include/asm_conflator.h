#pragma once

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants (64-byte cache line discipline) ---- */
enum {
  ASM_CACHELINE = 64,
  ASM_SPSC_CAP = 4096, /* power of two */
  ASM_MAX_PAYLOAD = 768,
  ASM_MAX_SYMBOLS = 8,
  ASM_MAX_DEPTH = 10,
  ASM_VWAP_DEPTH = 10,
  ASM_MAX_WORKERS = 8,
  ASM_LATENCY_CAP = 1 << 20, /* ~1M samples */
  ASM_CONFLATE_NS = 1000000ULL /* 1 ms */
};

enum AsmExchange { ASM_EX_BINANCE = 0, ASM_EX_OKX = 1 };

enum AsmSide { ASM_SIDE_BID = 0, ASM_SIDE_ASK = 1 };

enum AsmParseError {
  ASM_PARSE_OK = 0,
  ASM_PARSE_EMPTY = 1,
  ASM_PARSE_TRUNC = 2,
  ASM_PARSE_MISSING = 3,
  ASM_PARSE_NUMBER = 4,
  ASM_PARSE_SYMBOL = 5
};

/* ---- POD layouts (must match assembly offsets) ---- */
typedef struct BookLevel {
  double price;
  double qty;
} BookLevel;

typedef struct RawPacket {
  uint8_t exchange;
  uint8_t _pad0[3];
  uint32_t symbol_id;
  uint32_t length;
  uint32_t _pad1;
  uint64_t enqueue_ns;
  char bytes[ASM_MAX_PAYLOAD];
} RawPacket;

typedef struct ConflatedUpdate {
  uint8_t exchange;
  uint8_t _pad0[3];
  uint32_t symbol_id;
  uint64_t exchange_ts_ms;
  uint64_t enqueue_ns;
  uint16_t bid_count;
  uint16_t ask_count;
  uint32_t _pad1;
  BookLevel bids[ASM_MAX_DEPTH];
  BookLevel asks[ASM_MAX_DEPTH];
} ConflatedUpdate;

/* SPSC: power-of-two ring, head/tail on separate cache lines */
typedef struct SpscQueue {
  alignas(64) volatile uint64_t head; /* consumer */
  alignas(64) volatile uint64_t tail; /* producer */
  alignas(64) uint64_t capacity;      /* power of two */
  uint64_t mask;
  RawPacket *slots; /* capacity elements, externally allocated */
} SpscQueue;

typedef struct SpscUpdateQueue {
  alignas(64) volatile uint64_t head;
  alignas(64) volatile uint64_t tail;
  alignas(64) uint64_t capacity;
  uint64_t mask;
  ConflatedUpdate *slots;
} SpscUpdateQueue;

typedef struct LatencyRing {
  alignas(64) volatile uint64_t write_idx;
  alignas(64) volatile uint64_t dropped;
  alignas(64) uint64_t capacity; /* pad to own cache line — matches latency.S */
  uint64_t mask;
  uint64_t *samples; /* capacity */
} LatencyRing;

typedef struct LatencyStats {
  uint64_t sample_count;
  uint64_t dropped;
  uint64_t min_ns;
  uint64_t max_ns;
  double mean_ns;
  double p50;
  double p90;
  double p99;
  double p99_9;
} LatencyStats;

typedef struct PipelineStats {
  uint64_t packets_enqueued;
  uint64_t packets_dropped;
  uint64_t packets_parsed;
  uint64_t parse_errors;
  uint64_t levels_collapsed;
  uint64_t conflated_emitted;
  uint64_t fan_in_dropped;
  uint64_t books_updated;
  uint64_t arb_signals;
} PipelineStats;

typedef struct BenchConfig {
  double duration_sec;
  uint32_t feed_rate_hz;
  int pin_threads;
  double arb_threshold_bps;
  int worker_count; /* 0 = auto */
} BenchConfig;

/* ---- Assembly exports ---- */
uint64_t spsc_try_push(SpscQueue *q, const RawPacket *pkt);
uint64_t spsc_try_pop(SpscQueue *q, RawPacket *out);
uint64_t spsc_update_try_push(SpscUpdateQueue *q, const ConflatedUpdate *u);
uint64_t spsc_update_try_pop(SpscUpdateQueue *q, ConflatedUpdate *out);

uint64_t asm_parse_depth(const char *json, uint32_t len, uint32_t exchange,
                         uint64_t enqueue_ns, ConflatedUpdate *out);

double asm_vwap_top10(const BookLevel *levels, uint32_t count);

uint64_t latency_try_push(LatencyRing *r, uint64_t sample_ns);
void latency_compute(LatencyRing *r, LatencyStats *out);

/* Symbol helpers (C) */
int asm_symbol_id(const char *s, size_t n);
const char *asm_symbol_name(uint32_t id);
uint64_t asm_steady_ns(void);
uint64_t asm_hw_concurrency(void);

/* Pipeline */
int pipeline_run(const BenchConfig *cfg, PipelineStats *stats,
                 LatencyStats *lat, double *elapsed_ms);

/* Report */
int write_benchmark_report(const char *path, const char *build_type,
                           const PipelineStats *stats, const LatencyStats *lat,
                           double elapsed_ms);

/* Affinity / QoS */
void asm_pin_interactive(void);
void asm_pin_background(void);
const char *asm_affinity_backend(void);

/* Feed helpers */
void feed_format_packet(RawPacket *pkt, uint8_t exchange, uint32_t symbol_id,
                        uint64_t ts_ms, const BookLevel *bids, uint32_t nb,
                        const BookLevel *asks, uint32_t na, uint64_t enqueue_ns);

#ifdef __cplusplus
}
#endif
