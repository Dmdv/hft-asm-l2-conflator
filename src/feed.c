#include "asm_conflator.h"

#include <stdio.h>
#include <string.h>

enum { BIN_MAGIC = 0x314E4942u };

typedef struct BinHdr {
  uint32_t magic;
  uint32_t symbol_id;
  uint64_t ts_ms;
  uint16_t bid_count;
  uint16_t ask_count;
} BinHdr;

/* Kernel-style binary packet (benchmark hot path). */
void feed_format_packet(RawPacket *pkt, uint8_t exchange, uint32_t symbol_id,
                        uint64_t ts_ms, const BookLevel *bids, uint32_t nb,
                        const BookLevel *asks, uint32_t na,
                        uint64_t enqueue_ns) {
  if (nb > ASM_MAX_DEPTH)
    nb = ASM_MAX_DEPTH;
  if (na > ASM_MAX_DEPTH)
    na = ASM_MAX_DEPTH;

  BinHdr *h = (BinHdr *)(void *)pkt->bytes;
  h->magic = BIN_MAGIC;
  h->symbol_id = symbol_id;
  h->ts_ms = ts_ms;
  h->bid_count = (uint16_t)nb;
  h->ask_count = (uint16_t)na;
  BookLevel *lv = (BookLevel *)(void *)(pkt->bytes + sizeof(BinHdr));
  for (uint32_t i = 0; i < nb; ++i)
    lv[i] = bids[i];
  for (uint32_t i = 0; i < na; ++i)
    lv[nb + i] = asks[i];

  size_t len = sizeof(BinHdr) + (size_t)(nb + na) * sizeof(BookLevel);
  if (len > ASM_MAX_PAYLOAD)
    len = ASM_MAX_PAYLOAD;

  pkt->exchange = exchange;
  pkt->symbol_id = symbol_id;
  pkt->length = (uint32_t)len;
  pkt->enqueue_ns = enqueue_ns;
}

/* JSON formatter retained for explicit tests if needed */
void feed_format_packet_json(RawPacket *pkt, uint8_t exchange,
                             uint32_t symbol_id, uint64_t ts_ms,
                             const BookLevel *bids, uint32_t nb,
                             const BookLevel *asks, uint32_t na,
                             uint64_t enqueue_ns) {
  char *p = pkt->bytes;
  char *end = pkt->bytes + ASM_MAX_PAYLOAD;
  const char *sym = asm_symbol_name(symbol_id);
  int n = snprintf(p, (size_t)(end - p),
                   "{\"s\":\"%s\",\"E\":%llu,\"e\":\"depth\",\"b\":[", sym,
                   (unsigned long long)ts_ms);
  if (n < 0)
    n = 0;
  p += n;
  for (uint32_t i = 0; i < nb && p < end - 64; ++i) {
    n = snprintf(p, (size_t)(end - p), "%s[\"%.4f\",\"%.6f\"]", i ? "," : "",
                 bids[i].price, bids[i].qty);
    if (n < 0)
      break;
    p += n;
  }
  n = snprintf(p, (size_t)(end - p), "],\"a\":[");
  if (n > 0)
    p += n;
  for (uint32_t i = 0; i < na && p < end - 64; ++i) {
    n = snprintf(p, (size_t)(end - p), "%s[\"%.4f\",\"%.6f\"]", i ? "," : "",
                 asks[i].price, asks[i].qty);
    if (n < 0)
      break;
    p += n;
  }
  n = snprintf(p, (size_t)(end - p), "]}");
  if (n > 0)
    p += n;
  pkt->exchange = exchange;
  pkt->symbol_id = symbol_id;
  pkt->length = (uint32_t)(p - pkt->bytes);
  pkt->enqueue_ns = enqueue_ns;
}
