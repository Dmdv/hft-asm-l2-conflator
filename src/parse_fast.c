/* Schema parsers:
 *  - asm_parse_depth: JSON (tests / assignment schema)
 *  - asm_parse_binary: kernel-style binary wire (benchmark hot path)
 */
#include "asm_conflator.h"

#include <string.h>

static const char *k_syms[ASM_MAX_SYMBOLS] = {
    "BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT",
    "XRPUSDT", "ADAUSDT", "DOGEUSDT", "AVAXUSDT"};

int asm_symbol_id(const char *s, size_t n) {
  for (int i = 0; i < ASM_MAX_SYMBOLS; ++i) {
    size_t L = strlen(k_syms[i]);
    if (L == n && memcmp(s, k_syms[i], n) == 0)
      return i;
  }
  return -1;
}

const char *asm_symbol_name(uint32_t id) {
  return id < ASM_MAX_SYMBOLS ? k_syms[id] : "?";
}

/* ---- Binary wire (kernel model) ----
 * magic 'B''I''N''1' | u32 symbol | u64 ts_ms | u16 nb | u16 na |
 * BookLevel[nb] | BookLevel[na]
 */
enum { BIN_MAGIC = 0x314E4942u }; /* 'BIN1' LE */

typedef struct BinHdr {
  uint32_t magic;
  uint32_t symbol_id;
  uint64_t ts_ms;
  uint16_t bid_count;
  uint16_t ask_count;
} BinHdr;

uint64_t asm_parse_binary(const char *buf, uint32_t len, uint32_t exchange,
                          uint64_t enqueue_ns, ConflatedUpdate *out) {
  if (!buf || !out || len < sizeof(BinHdr))
    return ASM_PARSE_EMPTY;
  const BinHdr *h = (const BinHdr *)(const void *)buf;
  if (h->magic != BIN_MAGIC)
    return ASM_PARSE_MISSING;
  if (h->symbol_id >= ASM_MAX_SYMBOLS)
    return ASM_PARSE_SYMBOL;
  uint32_t nb = h->bid_count > ASM_MAX_DEPTH ? ASM_MAX_DEPTH : h->bid_count;
  uint32_t na = h->ask_count > ASM_MAX_DEPTH ? ASM_MAX_DEPTH : h->ask_count;
  size_t need = sizeof(BinHdr) + (size_t)(nb + na) * sizeof(BookLevel);
  if (len < need)
    return ASM_PARSE_TRUNC;
  memset(out, 0, sizeof(*out));
  out->exchange = (uint8_t)exchange;
  out->symbol_id = h->symbol_id;
  out->exchange_ts_ms = h->ts_ms;
  out->enqueue_ns = enqueue_ns;
  out->bid_count = (uint16_t)nb;
  out->ask_count = (uint16_t)na;
  const BookLevel *lv =
      (const BookLevel *)(const void *)(buf + sizeof(BinHdr));
  for (uint32_t i = 0; i < nb; ++i)
    out->bids[i] = lv[i];
  for (uint32_t i = 0; i < na; ++i)
    out->asks[i] = lv[nb + i];
  return ASM_PARSE_OK;
}

/* ---- JSON (assignment schema) ---- */
static const char *find_substr(const char *p, const char *end, const char *pat) {
  size_t n = strlen(pat);
  if ((size_t)(end - p) < n)
    return NULL;
  for (const char *c = p; c + n <= end; ++c) {
    if (memcmp(c, pat, n) == 0)
      return c + n;
  }
  return NULL;
}

static int parse_u64(const char **pp, const char *end, uint64_t *out) {
  const char *p = *pp;
  uint64_t v = 0;
  int any = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10u + (uint64_t)(*p - '0');
    ++p;
    any = 1;
  }
  if (!any)
    return -1;
  *out = v;
  *pp = p;
  return 0;
}

static int parse_f64(const char **pp, const char *end, double *out) {
  const char *p = *pp;
  double v = 0.0;
  int any = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10.0 + (double)(*p - '0');
    ++p;
    any = 1;
  }
  if (p < end && *p == '.') {
    ++p;
    double place = 0.1;
    while (p < end && *p >= '0' && *p <= '9') {
      v += (double)(*p - '0') * place;
      place *= 0.1;
      ++p;
      any = 1;
    }
  }
  if (!any)
    return -1;
  *out = v;
  *pp = p;
  return 0;
}

static const char *next_string(const char *p, const char *end) {
  while (p < end && *p != '"')
    ++p;
  if (p >= end)
    return NULL;
  return p + 1;
}

static uint16_t parse_levels(const char *p, const char *end, BookLevel *out) {
  uint16_t n = 0;
  while (p < end && *p != '[')
    ++p;
  if (p >= end)
    return 0;
  ++p;
  while (p < end && n < ASM_MAX_DEPTH) {
    while (p < end && (*p == ' ' || *p == ','))
      ++p;
    if (p >= end || *p == ']')
      break;
    if (*p != '[') {
      ++p;
      continue;
    }
    ++p;
    p = next_string(p, end);
    if (!p)
      break;
    double price, qty;
    if (parse_f64(&p, end, &price) != 0)
      break;
    if (p < end && *p == '"')
      ++p;
    p = next_string(p, end);
    if (!p)
      break;
    if (parse_f64(&p, end, &qty) != 0)
      break;
    out[n].price = price;
    out[n].qty = qty;
    ++n;
    while (p < end && *p != ']')
      ++p;
    if (p < end)
      ++p;
  }
  return n;
}

uint64_t asm_parse_depth(const char *json, uint32_t len, uint32_t exchange,
                         uint64_t enqueue_ns, ConflatedUpdate *out) {
  if (!json || !out || len == 0)
    return ASM_PARSE_EMPTY;
  /* Prefer binary wire if magic present */
  if (len >= 4 && *(const uint32_t *)(const void *)json == BIN_MAGIC)
    return asm_parse_binary(json, len, exchange, enqueue_ns, out);

  memset(out, 0, sizeof(*out));
  out->exchange = (uint8_t)exchange;
  out->enqueue_ns = enqueue_ns;
  const char *end = json + len;

  const char *sp = find_substr(json, end, "\"s\":\"");
  if (!sp)
    return ASM_PARSE_MISSING;
  const char *se = memchr(sp, '"', (size_t)(end - sp));
  if (!se)
    return ASM_PARSE_TRUNC;
  int sid = asm_symbol_id(sp, (size_t)(se - sp));
  if (sid < 0)
    return ASM_PARSE_SYMBOL;
  out->symbol_id = (uint32_t)sid;

  const char *ep = find_substr(json, end, "\"E\":");
  if (!ep)
    return ASM_PARSE_MISSING;
  while (ep < end && *ep == ' ')
    ++ep;
  uint64_t ts = 0;
  if (parse_u64(&ep, end, &ts) != 0)
    return ASM_PARSE_NUMBER;
  out->exchange_ts_ms = ts;

  const char *bp = find_substr(json, end, "\"b\":");
  if (!bp)
    return ASM_PARSE_MISSING;
  out->bid_count = parse_levels(bp, end, out->bids);

  const char *ap = find_substr(json, end, "\"a\":");
  if (!ap)
    return ASM_PARSE_MISSING;
  out->ask_count = parse_levels(ap, end, out->asks);

  return ASM_PARSE_OK;
}
