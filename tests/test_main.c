#include "asm_conflator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define EXPECT(cond, msg)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

static void test_spsc(void) {
  RawPacket *slots = aligned_alloc(64, ASM_SPSC_CAP * sizeof(RawPacket));
  SpscQueue q;
  memset(&q, 0, sizeof(q));
  q.capacity = ASM_SPSC_CAP;
  q.mask = ASM_SPSC_CAP - 1;
  q.slots = slots;
  q.head = 0;
  q.tail = 0;

  RawPacket in, out;
  memset(&in, 0, sizeof(in));
  in.exchange = 1;
  in.symbol_id = 3;
  in.length = 5;
  in.enqueue_ns = 42;
  memcpy(in.bytes, "hello", 5);

  EXPECT(spsc_try_push(&q, &in) == 1, "push");
  EXPECT(spsc_try_pop(&q, &out) == 1, "pop");
  EXPECT(out.exchange == 1, "ex");
  EXPECT(out.symbol_id == 3, "sym");
  EXPECT(out.enqueue_ns == 42, "ts");
  EXPECT(memcmp(out.bytes, "hello", 5) == 0, "payload");
  EXPECT(spsc_try_pop(&q, &out) == 0, "empty");

  /* fill almost full */
  uint64_t n = 0;
  while (spsc_try_push(&q, &in))
    ++n;
  EXPECT(n == ASM_SPSC_CAP - 1, "capacity-1");
  free(slots);
  printf("  spsc ok\n");
}

static void test_parse_and_vwap(void) {
  const char *json =
      "{\"s\":\"BTCUSDT\",\"E\":1710000000123,\"e\":\"depth\","
      "\"b\":[[\"42000.5\",\"1.2\"],[\"41999.0\",\"2.0\"]],"
      "\"a\":[[\"42001.0\",\"0.8\"],[\"42002.0\",\"1.5\"]]}";
  ConflatedUpdate u;
  uint64_t rc =
      asm_parse_depth(json, (uint32_t)strlen(json), ASM_EX_BINANCE, 100, &u);
  EXPECT(rc == ASM_PARSE_OK, "parse ok");
  EXPECT(u.symbol_id == 0, "btc");
  EXPECT(u.exchange_ts_ms == 1710000000123ull, "E");
  EXPECT(u.bid_count >= 2, "bids");
  EXPECT(fabs(u.bids[0].price - 42000.5) < 1e-6, "bid0p");
  EXPECT(fabs(u.bids[0].qty - 1.2) < 1e-6, "bid0q");

  BookLevel lv[2] = {{100.0, 1.0}, {200.0, 3.0}};
  double v = asm_vwap_top10(lv, 2);
  /* (100*1 + 200*3)/4 = 175 */
  EXPECT(fabs(v - 175.0) < 1e-9, "vwap");
  printf("  parse+vwap ok\n");
}

static void test_conflation_lww(void) {
  /* Binary wire + JSON both parse; LWW qty on second binary packet */
  BookLevel b1[1] = {{100.0, 1.0}};
  BookLevel b2[1] = {{100.0, 5.0}};
  BookLevel a[1] = {{101.0, 1.0}};
  RawPacket p1, p2;
  feed_format_packet(&p1, ASM_EX_BINANCE, 0, 1, b1, 1, a, 1, 10);
  feed_format_packet(&p2, ASM_EX_BINANCE, 0, 2, b2, 1, a, 1, 20);
  ConflatedUpdate u1, u2;
  EXPECT(asm_parse_depth(p1.bytes, p1.length, 0, 10, &u1) == 0, "p1");
  EXPECT(asm_parse_depth(p2.bytes, p2.length, 0, 20, &u2) == 0, "p2");
  EXPECT(fabs(u2.bids[0].qty - 5.0) < 1e-9, "lww qty");
  printf("  conflation input ok\n");
}

static void test_pipeline_smoke(void) {
  BenchConfig cfg = {.duration_sec = 0.25,
                     .feed_rate_hz = 20000,
                     .pin_threads = 0,
                     .arb_threshold_bps = 0.0,
                     .worker_count = 2};
  PipelineStats st;
  LatencyStats lat;
  double ms = 0;
  EXPECT(pipeline_run(&cfg, &st, &lat, &ms) == 0, "run");
  EXPECT(st.packets_parsed > 1000, "parsed volume");
  EXPECT(st.parse_errors == 0, "no parse errors");
  EXPECT(lat.sample_count > 0, "latency samples");
  printf("  pipeline smoke ok (parsed=%llu lat_n=%llu mean=%.0f ns)\n",
         (unsigned long long)st.packets_parsed,
         (unsigned long long)lat.sample_count, lat.mean_ns);
}

int main(void) {
  printf("asm_test unit tests\n");
  test_spsc();
  test_parse_and_vwap();
  test_conflation_lww();
  test_pipeline_smoke();
  if (g_fail) {
    fprintf(stderr, "SOME TESTS FAILED\n");
    return 1;
  }
  printf("ALL TESTS PASSED\n");
  return 0;
}
