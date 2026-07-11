#include "asm_conflator.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

static void cpu_model(char *buf, size_t n) {
#if defined(__APPLE__)
  size_t len = n;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, NULL, 0) == 0)
    return;
#endif
  struct utsname u;
  if (uname(&u) == 0) {
    snprintf(buf, n, "%s", u.machine);
    return;
  }
  snprintf(buf, n, "unknown");
}

int write_benchmark_report(const char *path, const char *build_type,
                           const PipelineStats *stats, const LatencyStats *lat,
                           double elapsed_ms) {
  char cpu[128];
  cpu_model(cpu, sizeof(cpu));
  const char *os =
#if defined(__APPLE__)
      "macOS"
#elif defined(__linux__)
      "Linux"
#else
      "unknown"
#endif
      ;

  double thr = 0.0;
  if (elapsed_ms > 0.0)
    thr = (double)stats->packets_parsed / (elapsed_ms / 1000.0);

  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  fprintf(f,
          "{\n"
          "  \"agent_name\": \"hft-asm\",\n"
          "  \"environment\": {\n"
          "    \"os\": \"%s\",\n"
          "    \"compiler\": \"Clang AArch64 asm + C harness\",\n"
          "    \"cpu_model\": \"%s\",\n"
          "    \"build_type\": \"%s\"\n"
          "  },\n"
          "  \"metrics\": {\n"
          "    \"total_messages_processed\": %llu,\n"
          "    \"total_execution_time_ms\": %.6f,\n"
          "    \"throughput_msg_per_sec\": %.6f,\n"
          "    \"latency_ns\": {\n"
          "      \"min\": %llu,\n"
          "      \"max\": %llu,\n"
          "      \"mean\": %.6f,\n"
          "      \"p50\": %.6f,\n"
          "      \"p90\": %.6f,\n"
          "      \"p99\": %.6f,\n"
          "      \"p99_9\": %.6f\n"
          "    }\n"
          "  }\n"
          "}\n",
          os, cpu, build_type,
          (unsigned long long)stats->packets_parsed, elapsed_ms, thr,
          (unsigned long long)lat->min_ns, (unsigned long long)lat->max_ns,
          lat->mean_ns, lat->p50, lat->p90, lat->p99, lat->p99_9);
  fclose(f);
  return 0;
}
