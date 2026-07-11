#include "asm_conflator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BUILD_TYPE_STR
#define BUILD_TYPE_STR "release"
#endif

static void usage(const char *a0) {
  printf("Usage: %s [--duration SEC] [--rate HZ] [--no-pin] [--arb-bps X] "
         "[--report PATH] [--workers N]\n",
         a0);
}

int main(int argc, char **argv) {
  BenchConfig cfg = {.duration_sec = 5.0,
                     .feed_rate_hz = 50000,
                     .pin_threads = 1,
                     .arb_threshold_bps = 1.0,
                     .worker_count = 0};
  char report_path[256];
  snprintf(report_path, sizeof(report_path), "benchmark_report_%s.json",
           BUILD_TYPE_STR);

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage(argv[0]);
      return 0;
    } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
      cfg.duration_sec = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
      cfg.feed_rate_hz = (uint32_t)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--no-pin")) {
      cfg.pin_threads = 0;
    } else if (!strcmp(argv[i], "--arb-bps") && i + 1 < argc) {
      cfg.arb_threshold_bps = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--report") && i + 1 < argc) {
      snprintf(report_path, sizeof(report_path), "%s", argv[++i]);
    } else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
      cfg.worker_count = atoi(argv[++i]);
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  printf("=== HFT Market Data Conflator (AArch64 Assembler / Kernel-style) ===\n");
  printf("Affinity backend : %s\n", asm_affinity_backend());
  printf("Build profile    : %s\n", BUILD_TYPE_STR);
  printf("Assembler        : AArch64 GAS (Clang integrated) — SPSC + NEON VWAP\n");
  printf("Runtime model    : Kernel-style exclusive-shard cooperative loops\n");
  printf("HW concurrency   : %llu\n",
         (unsigned long long)asm_hw_concurrency());
  printf("Running for %.1f s @ %u msg/s per exchange ...\n\n", cfg.duration_sec,
         cfg.feed_rate_hz);

  PipelineStats stats;
  LatencyStats lat;
  double elapsed_ms = 0.0;
  if (pipeline_run(&cfg, &stats, &lat, &elapsed_ms) != 0) {
    fprintf(stderr, "pipeline_run failed\n");
    return 1;
  }

  double thr = elapsed_ms > 0 ? (double)stats.packets_parsed / (elapsed_ms / 1000.0) : 0;

  printf("---------- Results ----------\n");
  printf("Wall time            : %.3f s\n", elapsed_ms / 1000.0);
  printf("Packets enqueued     : %llu\n",
         (unsigned long long)stats.packets_enqueued);
  printf("Packets dropped      : %llu\n",
         (unsigned long long)stats.packets_dropped);
  printf("Packets parsed       : %llu\n",
         (unsigned long long)stats.packets_parsed);
  printf("Parse errors         : %llu\n",
         (unsigned long long)stats.parse_errors);
  printf("Levels collapsed     : %llu\n",
         (unsigned long long)stats.levels_collapsed);
  printf("Conflated emitted    : %llu\n",
         (unsigned long long)stats.conflated_emitted);
  printf("Fan-in dropped       : %llu\n",
         (unsigned long long)stats.fan_in_dropped);
  printf("Book updates         : %llu\n",
         (unsigned long long)stats.books_updated);
  printf("Arb signals          : %llu\n",
         (unsigned long long)stats.arb_signals);
  printf("Throughput           : %.0f msg/s\n", thr);
  if (lat.sample_count) {
    printf("E2E latency samples  : %llu (dropped %llu)\n",
           (unsigned long long)lat.sample_count,
           (unsigned long long)lat.dropped);
    printf("E2E latency min      : %.2f us\n", lat.min_ns / 1e3);
    printf("E2E latency mean     : %.2f us\n", lat.mean_ns / 1e3);
    printf("E2E latency p50      : %.2f us\n", lat.p50 / 1e3);
    printf("E2E latency p90      : %.2f us\n", lat.p90 / 1e3);
    printf("E2E latency p99      : %.2f us\n", lat.p99 / 1e3);
    printf("E2E latency p99.9    : %.2f us\n", lat.p99_9 / 1e3);
    printf("E2E latency max      : %.2f us\n", lat.max_ns / 1e3);
  }

  if (write_benchmark_report(report_path, BUILD_TYPE_STR, &stats, &lat,
                             elapsed_ms) != 0) {
    fprintf(stderr, "Failed to write %s\n", report_path);
    return 1;
  }
  printf("\nWrote %s\n", report_path);
  return 0;
}
