#include "asm_conflator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <pthread.h>
#elif defined(__linux__)
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#endif

uint64_t asm_steady_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t asm_hw_concurrency(void) {
#if defined(__APPLE__)
  int ncpu = 0;
  size_t len = sizeof(ncpu);
  if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
    return (uint64_t)ncpu;
#endif
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint64_t)n : 4;
}

void asm_pin_interactive(void) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

void asm_pin_background(void) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif
}

const char *asm_affinity_backend(void) {
#if defined(__APPLE__)
  return "macOS QoS (USER_INTERACTIVE / BACKGROUND)";
#elif defined(__linux__)
  return "pthread_setaffinity_np";
#else
  return "none";
#endif
}

/* ---- latency_compute (sort + percentiles) ---- */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static double percentile(const uint64_t *s, size_t n, double p) {
  if (n == 0)
    return 0.0;
  double idx = p * (double)(n - 1);
  size_t lo = (size_t)idx;
  size_t hi = lo + 1 < n ? lo + 1 : lo;
  double frac = idx - (double)lo;
  return (double)s[lo] * (1.0 - frac) + (double)s[hi] * frac;
}

void latency_compute(LatencyRing *r, LatencyStats *out) {
  memset(out, 0, sizeof(*out));
  uint64_t n = r->write_idx;
  if (n > r->capacity)
    n = r->capacity;
  out->sample_count = n;
  out->dropped = r->dropped;
  if (n == 0)
    return;
  uint64_t *tmp = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
  if (!tmp)
    return;
  memcpy(tmp, r->samples, (size_t)n * sizeof(uint64_t));
  qsort(tmp, (size_t)n, sizeof(uint64_t), cmp_u64);
  out->min_ns = tmp[0];
  out->max_ns = tmp[n - 1];
  double sum = 0.0;
  for (uint64_t i = 0; i < n; ++i)
    sum += (double)tmp[i];
  out->mean_ns = sum / (double)n;
  out->p50 = percentile(tmp, (size_t)n, 0.50);
  out->p90 = percentile(tmp, (size_t)n, 0.90);
  out->p99 = percentile(tmp, (size_t)n, 0.99);
  out->p99_9 = percentile(tmp, (size_t)n, 0.999);
  free(tmp);
}
