// avr_kv_batch.c
// Быстрый групповой перебор по окнам, подготовленным avr_scout.c
// Не выполняет демодуляцию/FEC — работает только по предобработанным 27B payload на SF.
// Автор: DSD/DMR (каркас)

// src/avr_kv_batch.c
#include "avr_kv.h"
#include "dsd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <mbelib.h> // for mbe_* functions (vocoder error correction & decode)
// Include or forward-declare encryption helpers:
#include "pc4.h" // for Tytera PC4 functions (tyt_ap_pc4_keystream_creation, etc)
#include "rc2.h" // for RC4 (if rc2.h contains RC4 definitions in this context)

extern const int rW[36];
extern const int rX[36];
extern const int rY[36];
extern const int rZ[36];

// ====================== ВНУТРЕННИЕ ТИПЫ ДЛЯ КЛЮЧЕЙ =======================

// Унифицированная запись ключа: поддержим разные ALG (AES128/256, HYT и т.п.)
typedef struct
{
  uint8_t alg_id;  // ALG_ID (как в PI/LE)
  int key_id;      // KEY_ID (или -1, если не задан)
  uint8_t key[32]; // максимум для AES-256 (32 байта)
  uint8_t key_len; // 16 или 32 (или иной размер для нестандартных)
  uint64_t index;  // <--- добавляем сюда
} kv_key_t;

// Набор ключей (SoA/вектор можно позже для SIMD; пока массив структур)
typedef struct
{
  kv_key_t *v;
  size_t n;
} kv_keyset_t;

// Структура временного результата по паре (key × window)
typedef struct
{
  int gi;      // индекс группы
  int wi;      // индекс окна в группе
  int ki;      // индекс ключа в gKS
  int score;   // итоговая оценка (пока заглушка)
  int used_sf; // сколько SF реально отработали (для лога/диагн)
} kv_eval_result_t;

// ======================= KV-BATCH EVAL (Этап 2 каркас) =======================

// из avr_kv.c (и dsd-fme) — уже есть реализация в проекте
extern void aes_ecb_bytewise_payload_crypt(uint8_t *in, uint8_t *key, uint8_t *out, int type, int enc);

//==========================================================================

// 1) Копирование 27-байт пэйлоада по окну из Scout (RAM → локальный буфер).
//    TODO(Этап 3): реализовать через внутренние буферы SC.hist.
//    Сейчас возвращаем 0, чтобы не трогать Scout.
// Возвращает число байтов (27 * len_sf) при успехе, 0 если что-то не нашли.
// static
size_t kv_fetch_window_payload(uint8_t slot, const avr_scout_window_ref_t *wref,
                               uint8_t *dst, size_t dst_cap)
{
  if (!wref || !dst)
    return 0;
  const size_t need = 27u * (size_t)wref->len_sf;
  if (dst_cap < need)
    return 0;

  size_t off = 0;
  for (uint8_t i = 0; i < wref->len_sf; ++i)
  {
    uint8_t tmp[27];
    const uint32_t sf_idx = wref->start_sf_idx + i;
    if (avr_scout_get_27B_by_sf(slot, sf_idx, tmp) != 0)
      return 0; // чего-то не нашли — прерываем окно
    memcpy(dst + off, tmp, 27);
    off += 27;
  }
  return off;
}

// ====================== ВНУТРЕННИЕ СТАТИКИ/ПРОТОТИПЫ ======================
// ---------------- CSV Loader (встроенная реализация) ----------------

static int hex2nibble(int c, int *out)
{
  if (c >= '0' && c <= '9')
  {
    *out = c - '0';
    return 0;
  }
  if (c >= 'a' && c <= 'f')
  {
    *out = 10 + (c - 'a');
    return 0;
  }
  if (c >= 'A' && c <= 'F')
  {
    *out = 10 + (c - 'A');
    return 0;
  }
  return -1;
}

static int parse_hexbytes(const char *s, uint8_t *out, size_t *out_len, size_t max_len)
{
  size_t n = 0;
  int hi = -1, lo = -1;
  while (*s)
  {
    if (*s == ' ' || *s == ':' || *s == '-' || *s == ',')
    {
      s++;
      continue;
    }
    if (*s == 'x' || *s == 'X')
    {
      s++;
      continue;
    } // допускаем 0x внутри
    if (!isxdigit((unsigned char)*s))
    {
      s++;
      continue;
    }
    if (hex2nibble(*s++, &hi) != 0)
      return -1;
    while (*s && !isxdigit((unsigned char)*s))
    {
      s++;
    } // пропустить разделитель
    if (!*s)
      return -1;
    if (hex2nibble(*s++, &lo) != 0)
      return -1;
    if (n >= max_len)
      return -1;
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  *out_len = n;
  return 0;
}

static int parse_int_auto(const char *s, int *out)
{
  while (*s == ' ' || *s == '\t')
    s++;
  int base = 10;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
  {
    base = 16;
    s += 2;
  }
  char *end = NULL;
  long v = strtol(s, &end, base);
  if (end == s)
    return -1;
  *out = (int)v;
  return 0;
}

static int parse_uint64_auto(const char *s, uint64_t *out) {
  while (*s==' '||*s=='\t') s++;
  int base = 10;
  if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, base);
  if (end == s) return -1;
  *out = (uint64_t)v;
  return 0;
}

// экспорт групп/окон от Разведчика (сейчас — через публичный API)
// extern const avr_scout_export_t *avr_scout_export(void);
extern int kv_csv_load_and_filter(const char *csv_path, kv_key_t **out_vec, size_t *out_n);

static int kv_batch_load_keys_from_csv(dsd_opts *opts, kv_keyset_t *out)
{
  if (!opts || !out)
    return -1;
  out->v = NULL;
  out->n = 0;

  kv_key_t *vec = NULL;
  size_t n = 0;

  int rc = kv_csv_load_and_filter(opts->kv_csv_path, &vec, &n);
  if (rc != 0 || n == 0)
    return -2;

  out->v = vec;
  out->n = n;
  return 0;
}
// --- Глобальный кэш ключей, чтобы не читать CSV заново ---
static kv_keyset_t gKS = {0};
static int gKS_loaded = 0;

void kv_batch_init(dsd_opts *opts, dsd_state *st)
{
  if (!opts)
    return;
  if (gKS_loaded)
    return; // уже загружено
  if (opts->kv_csv_path[0] == '\0')
    return;

  int rc = kv_batch_load_keys_from_csv(opts, &gKS);
  if (rc == 0 && gKS.n > 0)
  {
    gKS_loaded = 1;
    // разовая сводка
    size_t per_alg[256] = {0};
    for (size_t i = 0; i < gKS.n; ++i)
      per_alg[gKS.v[i].alg_id]++;
    fprintf(stderr, "[KV-BATCH] keys loading: total=%zu from %s\n", gKS.n, opts->kv_csv_path);
    for (int a = 0; a < 256; ++a)
      if (per_alg[a])
        fprintf(stderr, "  alg=0x%02X → %zu\n", a, per_alg[a]);

    if (gKS.n > 1)
    {
      // это именно режим перебора
      opts->kv_batch_enable = 1;
      fprintf(stderr, "[KV-BATCH] START !\n");
      // В batch не держим smooth "вечно включённым" с запуска.
      // Включим его только тогда, когда окна готовы (см. dsd_frame.c правку ниже).
      if (opts->kv_smooth)
      {
        fprintf(stderr, "[KV-BATCH] defer smooth=0 for batch (will enable on first ready window)\n");
        opts->kv_smooth = 0;
      }
    }
    else if (gKS.n == 1)
    {
      // одиночный CSV — это НЕ перебор
      opts->kv_batch_enable = 0;

      // single + smooth: включаем smooth сразу (если не включён через -js)
      if (!opts->kv_smooth)
      {
        opts->kv_smooth = 1;
        fprintf(stderr, "Smooth MBE validation auto-enabled (single CSV key).\n");
      }
    }

    if (gKS.n == 1 /* && !opts->has_inline_key_flags */)
    {
      const kv_key_t *K = &gKS.v[0];
      kv_apply_runtime_key_from_bytes(opts, st, K->alg_id, K->key, K->key_len, K->key_id);
      fprintf(stderr, "[KV-BATCH] auto-applied single CSV key: alg=0x%02X len=%u\n",
              K->alg_id, K->key_len);
    }
    if (gKS.n > 1)
      opts->kv_batch_enable = 1;
    opts->run_scout = 1;
    fprintf(stderr, "[KV-BATCH] opts->kv_batch_enable %d. gKS.n = %d  ", opts->kv_batch_enable, (int)gKS.n);
  }
}

void kv_batch_free(void)
{
  if (gKS.v)
    free(gKS.v);
  memset(&gKS, 0, sizeof(gKS));
  gKS_loaded = 0;
}
// ====================== ПУБЛИЧНЫЙ ВХОД В БАТЧ =============================
// ====================== РЕАЛИЗАЦИЯ CSV-ЗАГРУЗЧИКА =========================

// Обёртка: грузим ключи из CSV, пользуясь существующим кодом в avr_kv.c,
// где у тебя уже был пайплайн под -jc.
// Глобальная функция с ИМЕНЕМ, которое ждёт линкер (как в твоём вызове)
// --- helper: parse ALG token -> ALG_ID (0x24/0x25/0x21/...) ---
// --- helper: parse ALG token -> ALG_ID (0x24/0x25/...) ---
static int parse_alg_token(const char *s, int *alg_id_out)
{
  if (!s || !*s)
    return -1;

  // numeric first: 25 / 0x25
  int v = 0;
  if (parse_int_auto(s, &v) == 0 && v >= 0 && v <= 0xFF)
  {
    *alg_id_out = v;
    return 0;
  }

  // textual: aes256/aes128/aes192/arc4/rc4
  char buf[32];
  int i = 0;
  while (*s && i < (int)sizeof(buf) - 1)
    buf[i++] = (char)tolower((unsigned char)*s++);
  buf[i] = '\0';

  if (strstr(buf, "aes256") || strstr(buf, "aes-256"))
  {
    *alg_id_out = 0x25;
    return 0;
  }
  if (strstr(buf, "aes192") || strstr(buf, "aes-192"))
  {
    *alg_id_out = 0x23;
    return 0;
  }
  if (strstr(buf, "aes128") || strstr(buf, "aes-128"))
  {
    *alg_id_out = 0x24;
    return 0;
  }
  if (strstr(buf, "arc4") || strstr(buf, "rc4"))
  {
    *alg_id_out = 0x21;
    return 0;
  }

  return -1;
}

// --- helper: sanitize HEX ("0x", spaces, ':', '-', ',') and convert to bytes ---
static int parse_hex_relaxed(const char *in, uint8_t *out, size_t *out_len, size_t cap)
{
  if (!in || !out || !out_len)
    return -1;
  char tmp[2048];
  size_t w = 0;
  for (const char *p = in; *p && w < sizeof(tmp) - 1; ++p)
  {
    unsigned char c = (unsigned char)*p;
    if (c == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
      ++p;
      continue;
    }
    if (isxdigit(c))
      tmp[w++] = (char)c;
  }
  tmp[w] = '\0';
  if (w == 0 || (w & 1))
    return -1;

  size_t n = w / 2;
  if (n > cap)
    return -1;
  for (size_t i = 0; i < n; ++i)
  {
    unsigned v = 0;
    if (sscanf(tmp + 2 * i, "%2x", &v) != 1)
      return -1;
    out[i] = (uint8_t)v;
  }
  *out_len = n;
  return 0;
}

//======================================================================================================
// локальная распаковка 27 байт (216 бит) в 3 AMBE-фрейма по схеме, как в avr_kv.c
static void kv_unpack_27B_to_3frames(const uint8_t *dec27,
                                     char ambe_fr1[4][24],
                                     char ambe_fr2[4][24],
                                     char ambe_fr3[4][24])
{
  // подготовим массив битов
  uint8_t bits[216];
  // есть extern из avr_kv.c: unpack_byte_array_into_bit_array(bytes,bits,byte_count)
  extern void unpack_byte_array_into_bit_array(uint8_t *bytes, uint8_t *bits, int byte_count);
  unpack_byte_array_into_bit_array((uint8_t *)dec27, bits, 27);

  int bit_idx = 0;
  // вставляем только payload-позиции ((j<12)||(j>14)), как в твоем коде
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 24; j++)
      if ((j < 12) || (j > 14))
        ambe_fr1[i][j] = bits[bit_idx++];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 24; j++)
      if ((j < 12) || (j > 14))
        ambe_fr2[i][j] = bits[bit_idx++];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 24; j++)
      if ((j < 12) || (j > 14))
        ambe_fr3[i][j] = bits[bit_idx++];
}

//======================================================================================================
// Глобальная функция с ИМЕНЕМ, которое ждёт линкер (используется внутри этого модуля)

// ====== helpers ======

static inline int is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

// Собираем только HEX-цифры из произвольной строки (игнорируем пробелы, 0x, :, -, , и т.п.)
static size_t hex_collect_digits(const char *in, char *out, size_t outsz)
{
  size_t k = 0;
  for (const char *p = in; *p; ++p)
  {
    if (is_hex_digit(*p))
    {
      if (k + 1 >= outsz)
        break;
      out[k++] = *p;
    }
  }
  out[k] = '\0';
  return k;
}

// Нормализуем представление: вставляем пробел после каждых 16 HEX-символов (без пробела в конце)
static void normalize_hex_group16(const char *hex_digits, char *out, size_t outsz)
{
  // hex_digits — уже только [0-9a-fA-F]
  size_t n = strlen(hex_digits);
  size_t k = 0;
  for (size_t i = 0; i < n; ++i)
  {
    if (k + 1 >= outsz)
      break;
    out[k++] = hex_digits[i];
    // если достигли конца 16-символьной группы и это не конец строки — добавим пробел
    if (((i + 1) % 16) == 0 && (i + 1) < n)
    {
      if (k + 1 >= outsz)
        break;
      out[k++] = ' ';
    }
  }
  if (k < outsz)
    out[k] = '\0';
}

// Преобразуем HEX-строку (может содержать пробелы/0x/:/-/,) в байты
static int hex_to_bytes_relaxed(const char *in, uint8_t *out, size_t *out_len, size_t out_cap)
{
  char digits[256 * 3] = {0};
  size_t n = hex_collect_digits(in, digits, sizeof(digits));
  if (n == 0 || (n % 2) != 0)
    return -1; // нечётная длина HEX — ошибка
  size_t bytes = n / 2;
  if (bytes > out_cap)
    return -2;
  for (size_t i = 0; i < bytes; ++i)
  {
    char hi = digits[2 * i], lo = digits[2 * i + 1];
    uint8_t v = 0;
    if (hi >= '0' && hi <= '9')
      v = (hi - '0') << 4;
    else
      v = ((hi & ~0x20) - 'A' + 10) << 4;
    if (lo >= '0' && lo <= '9')
      v |= (lo - '0');
    else
      v |= ((lo & ~0x20) - 'A' + 10);
    out[i] = v;
  }
  *out_len = bytes;
  return 0;
}

// Сопоставление кодов из твоей таблицы — к ожидаемому парсером виду.
// Возвращает 0, если удалось получить alg_id через parse_alg_token; иначе пытается "aes".
static int resolve_alg_id_with_fallback(const char *alg_token_raw, int *alg_id_out,
                                        const char **canonical_alg_used,
                                        size_t key_hex_chars /*число HEX-символов после нормализации*/)
{
  // нормализуем регистр и удалим пробелы вокруг
  char t[64] = {0};
  size_t len = 0;
  for (const char *p = alg_token_raw; *p && len < sizeof(t) - 1; ++p)
  {
    char c = *p;
    if (c == ' ' || c == '\t' || c == '"')
      continue;
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    t[len++] = c;
  }
  t[len] = '\0';

  // приведение популярных псевдонимов
  // (можно расширять по мере необходимости)
  struct
  {
    const char *in;
    const char *canon;
    int expect_chars;
  } map[] = {
      {"arc4", "arc4", 10}, // 10 hex chars = 5 байт (из твоей таблицы)
      {"aes128", "aes128", 32},
      {"aes192", "aes192", 48},
      {"aes256", "aes256", 64},
      {"tyt128pc4", "tyt128pc4", 32},
      {"tyt256pc4", "tyt256pc4", 64},
      {"tyt128", "tyt128", 32}, // TYT Enhanced Privacy (EP)
      {"veda128", "veda128", 32},
      {"bp", "bp", 4}, // Basic
      {"des64", "des64", 16},
      {"tyt_bp", "tyt_bp", 4},
      {"bp10", "bp10", 10},
      {"bp16", "bp16", 16},
      {"bp32", "bp32", 32},
      {"bp48", "bp48", 48},
      {"bp64", "bp64", 64},
      {"lira", "lira", 10},
  };

  const char *canon = t;
  int matched = 0;
  int expect_chars = -1;
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
  {
    if (strcmp(t, map[i].in) == 0)
    {
      canon = map[i].canon;
      expect_chars = map[i].expect_chars;
      matched = 1;
      break;
    }
  }

  // если формат из таблицы и длина (в символах) не совпала — принудительно "aes"
  if (matched && expect_chars > 0 && key_hex_chars > 0 && key_hex_chars != (size_t)expect_chars)
  {
    canon = "aes";
  }

  if (parse_alg_token(canon, alg_id_out) == 0)
  {
    if (canonical_alg_used)
      *canonical_alg_used = canon;
    return 0;
  }

  // фолбэк: aes
  if (parse_alg_token("aes", alg_id_out) == 0)
  {
    if (canonical_alg_used)
      *canonical_alg_used = "aes";
    return 0;
  }

  return -1;
}

// ====== основная функция ======

int kv_csv_load_and_filter(const char *csv_path, kv_key_t **out_vec, size_t *out_n)
{
  *out_vec = NULL;
  *out_n = 0;

  FILE *f = fopen(csv_path, "r");
  if (!f)
  {
    fprintf(stderr, "[KV-BATCH] ERROR: cannot open CSV: %s\n", csv_path);
    return -1;
  }

  size_t cap = 64, n = 0;
  kv_key_t *vec = (kv_key_t *)malloc(cap * sizeof(kv_key_t));
  if (!vec)
  {
    fclose(f);
    return -2;
  }
  memset(vec, 0, cap * sizeof(kv_key_t));

  char line[4096];
  while (fgets(line, sizeof(line), f))
  {
    // trim left
    const char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0' || *p == '\n' || *p == '#' || *p == ';')
      continue;

    // Разобьём сами (без strtok), чтобы аккуратно взять до 4 токенов с учётом пробелов в HEX
    // Правило: первые 2 разделителя (, ; \t) отделяют первые токены, остальное — как есть.
    char t0[128] = {0}, t1[128] = {0}, t2[3500] = {0}, t3[128] = {0};
    int parts = 0;
    const char *cur = p;
    while (*cur && parts < 3)
    {
      // пропустим ведущие пробелы
      while (*cur == ' ' || *cur == '\t')
        cur++;
      const char *start = cur;
      while (*cur && *cur != ',' && *cur != ';' && *cur != '\t' && *cur != '\r' && *cur != '\n')
        cur++;
      size_t len = (size_t)(cur - start);
      if (len > 0)
      {
        if (parts == 0)
        {
          if (len >= sizeof(t0))
            len = sizeof(t0) - 1;
          memcpy(t0, start, len);
          t0[len] = 0;
        }
        else if (parts == 1)
        {
          if (len >= sizeof(t1))
            len = sizeof(t1) - 1;
          memcpy(t1, start, len);
          t1[len] = 0;
        }
        else if (parts == 2)
        {
          if (len >= sizeof(t2))
            len = sizeof(t2) - 1;
          memcpy(t2, start, len);
          t2[len] = 0;
        }
        parts++;
      }
      if (*cur == ',' || *cur == ';' || *cur == '\t')
        cur++; // пропускаем разделитель
    }
    // Остаток строки — потенциальный t3 (может быть пуст)
    while (*cur == ' ' || *cur == '\t' || *cur == ',' || *cur == ';')
      cur++;
    if (*cur)
    {
      size_t len = strcspn(cur, "\r\n");
      if (len >= sizeof(t3))
        len = sizeof(t3) - 1;
      memcpy(t3, cur, len);
      t3[len] = 0;
    }

    // Определяем формат
    int alg_id = -1, kid = 0;
    uint64_t index_val = 0;
    const char *hex_raw = NULL;
    int have_len_hint = 0; // из CSV
    int len_hint_chars = 0;

    if (t0[0] && t1[0] && t2[0] && !t3[0])
    {
      // либо A) alg_or_code,kid,key_hex
      // либо B) index,alg_or_code,key_hex
      uint64_t tmp64;
      if (parse_uint64_auto(t0, &tmp64) == 0) {
        index_val = tmp64;
        hex_raw = t2;
        // alg определим позже
      }
      else
      {
        // alg_or_code,kid,key_hex
        if (parse_int_auto(t1, &kid) != 0)
          kid = 0;
        hex_raw = t2;
      }
    }
    else if (t0[0] && t1[0] && t2[0] && t3[0])
    {
      // B) index,alg_or_code,key_hex,len  ИЛИ alg_or_code,kid,key_hex,len
      uint64_t tmp64;
      hex_raw = t2;
      if (parse_int_auto(t3, &len_hint_chars) == 0)
        have_len_hint = 1;
      if (parse_uint64_auto(t0, &tmp64) == 0) 
      {
        index_val = tmp64;
      }
      else
      {
        // alg_or_code,kid, ...
        if (parse_int_auto(t1, &kid) != 0)
          kid = 0;
      }
    }
    else if (t0[0] && t1[0] && !t2[0])
    {
      // C) alg_or_code, key_hex
      hex_raw = t1;
    }
    else
    {
      // Нечитаемый формат
      continue;
    }

    // Соберём только HEX-цифры и заранее узнаем длину в символах
    char hex_digits[256 * 3] = {0};
    size_t hex_chars = hex_collect_digits(hex_raw, hex_digits, sizeof(hex_digits));
    if (hex_chars == 0)
      continue;

    // Если есть len-hint, но он не совпал — игнорируем подсказку и считаем сами
    (void)have_len_hint;
    (void)len_hint_chars; // само наличие нам не критично, мы уже пересчитали

    // Определим alg_id с учётом таблицы и длины в символах
    const char *canon_alg = NULL;
    if (t1[0] && !t2[0])
    {
      // формат C: alg_or_code, key_hex
      if (resolve_alg_id_with_fallback(t0, &alg_id, &canon_alg, (int)hex_chars) != 0)
        continue;
    }
    else if (t0[0] && t1[0])
    {
      // форматы A/B
      // t1 — это либо kid, либо alg. Если t1 — число, возьмём алг из t0, иначе — из t1
      int dummy;
      if (parse_int_auto(t1, &dummy) == 0)
      {
        if (resolve_alg_id_with_fallback(t0, &alg_id, &canon_alg, (int)hex_chars) != 0)
          continue;
      }
      else
      {
        if (resolve_alg_id_with_fallback(t1, &alg_id, &canon_alg, (int)hex_chars) != 0)
          continue;
      }
    }
    else
    {
      // fallback — aes
      if (parse_alg_token("aes", &alg_id) != 0)
        continue;
      canon_alg = "aes";
    }

    // Нормализуем (для логов/проверки): пробел после каждых 16 символов
    char hex_grouped[256 * 3] = {0};
    normalize_hex_group16(hex_digits, hex_grouped, sizeof(hex_grouped));
    // Теперь превращаем в байты
    kv_key_t K;
    memset(&K, 0, sizeof(K));
    size_t keylen = 0;
    if (hex_to_bytes_relaxed(hex_grouped, K.key, &keylen, sizeof(K.key)) != 0)
    {
      continue;
    }

    // Для "aes" (или aes128/192/256) — проверим валидные длины 16/24/32. Иначе можно оставить как есть для не-AES.
    if (canon_alg &&
        (strcmp(canon_alg, "aes") == 0 || strncmp(canon_alg, "aes", 3) == 0))
    {
      if (!(keylen == 16 || keylen == 24 || keylen == 32))
      {
        // некорректная длина для AES — отбрасываем
        continue;
      }
    }

    K.key_len = (uint8_t)keylen;
    K.alg_id = (uint8_t)(alg_id & 0xFF);
    K.key_id = kid;
    K.index = index_val;

    if (n >= cap)
    {
      cap *= 2;
      kv_key_t *nv = (kv_key_t *)realloc(vec, cap * sizeof(kv_key_t));
      if (!nv)
      {
        free(vec);
        fclose(f);
        return -3;
      }
      vec = nv;
      // обнулять не обязательно, мы заполняем по месту
    }
    vec[n++] = K;

    // По желанию: лог красиво отформатированного ключа
    // fprintf(stderr, "[KV-BATCH] loaded key: alg=%s kid=%d hex=\"%s\" len=%zu bytes index_val =%ld\n",
    //         canon_alg ? canon_alg : "?", kid, hex_grouped, keylen, index_val);
    // break;         
  }

  fclose(f);

  if (n == 0)
  {
    free(vec);
    return -4;
  }
  *out_vec = vec;
  *out_n = n;
  return 0;
}

// Публичный хелпер: если загружен РОВНО 1 ключ — применить его к стейту
void kv_batch_maybe_apply_single_key(dsd_opts *opts, dsd_state *st)
{
  if (!opts || !st)
    return;
  if (!gKS_loaded)
    return;
  if (gKS.n != 1)
    return;

  const kv_key_t *K = &gKS.v[0];
  // kid из CSV может быть 0 → трактуем как «не задан» (ставим 0 в стейт)
  // kv_apply_runtime_key_from_bytes(opts, st, K->alg_id, K->key, K->key_len, K->key_id);
  if (K->alg_id > 0)
  { // ->alg_id == alg ||
    kv_apply_runtime_key_from_bytes(opts, st, K->alg_id, K->key, K->key_len, K->key_id);
    // лог
    fprintf(stderr, "[KV-RUNTIME] runtime key applied (single CSV key): alg=0x%02X len=32\n", K->alg_id);
  }
}

void avr_kv_batch_begin(dsd_opts *opts, dsd_state *st)
{
  (void)opts;
  (void)st;
}
//==========================================================================================
// --- РЕАЛИЗАЦИЯ AES-OFB ---
//==========================================================================================
/*
 * Оценивает одно окно суперкадров с заданным ключом.
 * slot: таймслот (0 или 1), к которому относится это окно.
 * dec27: указатель на nsf*27 байт зашифрованных голосовых данных (каждые 27Б = один суперкадр).
 * nsf: количество суперкадров в окне.
 * key_bytes/key_len/alg_id: данные ключа и идентификатор алгоритма.
 * Выходные параметры: avg_smooth (средняя гладкость), ok_frames (количество полностью декодированных кадров), bad_errs2 (количество кадров с неисправимыми ошибками).
 * Возвращает 0 при успехе, или -1, если расшифровка не удалась (например, неподдерживаемый алгоритм).
 */

// сохраняем сигнатуру, но внутри используем live AES
static inline void aes_ofb_dec_27B_one_sf(uint8_t iv16[16],
                                          const uint8_t *key, int key_len,
                                          const uint8_t in27[27], uint8_t out27[27])
{
  uint8_t ks[16];
  size_t off = 0;
  // const int type = (key_len == 16) ? 1 : 2; // как в твоём проекте

  int type = 0;
  if (key_len == 16)
    type = 1; // AES-128
  else if (key_len == 24)
    type = 3; // AES-192 (если поддерживается в твоём AES-ECB)
  else if (key_len == 32)
    type = 2; // AES-256
  else if (key_len == 10)
    type = 4; // ARC4
  else
    return; // непредусмотренная длина
  while (off < 27)
  {
    // ks = AES_ENC(IV)
    aes_ecb_bytewise_payload_crypt(iv16, (uint8_t *)key, ks, type, 1);
    size_t take = (27 - off > 16) ? 16 : (27 - off);
    for (size_t i = 0; i < take; i++)
      out27[off + i] = in27[off + i] ^ ks[i];
    // OFB feedback
    memcpy(iv16, ks, 16);
    off += take;
  }
}
// ====== ГЛАВНЫЙ проход по окну — дешифр., AMBE, smooth, интеграция в kv ======
#ifndef KV_TRACE
#define KV_TRACE 1
#endif

#if KV_TRACE
#define KVTR(...)                 \
  do                              \
  {                               \
    fprintf(stderr, __VA_ARGS__); \
  } while (0)
#else
#define KVTR(...) \
  do              \
  {               \
  } while (0)
#endif

// ======================

static inline int kv_alg_is_aes_dmra(int alg_id)
{
  return (alg_id == KV_ALG_AES128 || alg_id == KV_ALG_AES192 || alg_id == KV_ALG_AES256 ||
          alg_id == 0x21 || alg_id == 0x23 || alg_id == 0x25);
}

// 3) Расшифровка 27B OFB → 27B голосового payload’а (1 SF).
static int kv_ofb_decrypt_27B(const uint8_t *iv16,
                              const uint8_t *key, int key_len,
                              const uint8_t *in, uint8_t *out)
{
  if (!iv16 || !key || !in || !out)
    return -1;
  if (!(key_len == 16 || key_len == 24 || key_len == 32))
    return -1;

  // Подбор "type" для aes_ecb_bytewise_payload_crypt:
  // В проекте ранее встречалось: (key_len == 16 ? 1 : 2)
  // Ориентируемся на это же правило: 1 = AES-128, 2 = AES-256 (192 не используете).
  const int aes_type = (key_len == 16) ? 1 : 2;

  uint8_t ofb_iv[16];
  memcpy(ofb_iv, iv16, 16);

  size_t produced = 0;
  while (produced < 27)
  {
    uint8_t ks[16];

    // ENC(IV) -> keystream
    // aes_ecb_bytewise_payload_crypt(in, key, out, type, enc)
    // здесь in = IV, out = ks, enc = 1 (encrypt), type = aes_type
    aes_ecb_bytewise_payload_crypt(ofb_iv, (uint8_t *)key, ks, aes_type, 1);

    const size_t take = ((27 - produced) > 16) ? 16 : (27 - produced);
    for (size_t i = 0; i < take; i++)
      out[produced + i] = in[produced + i] ^ ks[i];

    // OFB chaining: IV := ENC(IV)
    memcpy(ofb_iv, ks, 16);
    produced += take;
  }
  return 0;
}

static void kv_mean_stddev(const float *data, int n, float *mean, float *stddev)
{
  if (n == 0)
  {
    *mean = 0;
    *stddev = 0;
    return;
  }
  double sum = 0.0, sum_sq = 0.0;
  for (int i = 0; i < n; i++)
  {
    sum += data[i];
    sum_sq += data[i] * data[i];
  }
  *mean = (float)(sum / n);
  double var = (sum_sq / n) - (*mean * *mean);
  *stddev = (float)sqrt(fmax(0.0, var));
}

#define BATCH_MAX_S_VALUES 64

static inline void kv_bits72_to_ambe_enc(const uint8_t b72[72], char ambe_fr[4][24])
{
  int k = 0;
  for (int j = 0; j < 24; j++)
  {
    if (j >= 12 && j <= 14)
      continue; // три служебных столбца
    // порядок строк 0..3 соответствует live-пути
    ambe_fr[0][j] = (char)(b72[k++] & 1);
    ambe_fr[1][j] = (char)(b72[k++] & 1);
    ambe_fr[2][j] = (char)(b72[k++] & 1);
    ambe_fr[3][j] = (char)(b72[k++] & 1);
  }
}

// Разложить 27 байт (216 бит) суперкадра в три массива по 72 бита (побитово, MSB→LSB).
// Это просто линейная раскладка без каких-либо «голосовых» перестановок.
static inline void kv_split_27B_to_3x72bits(const uint8_t enc27[27],
                                            uint8_t out72[3][72])
{
  int bit = 0;
  for (int i = 0; i < 27; i++)
  {
    for (int b = 7; b >= 0; b--)
    {
      uint8_t v = (enc27[i] >> b) & 1u;
      int blk = bit / 72;
      int ofs = bit % 72;
      out72[blk][ofs] = v;
      bit++;
    }
  }
}

static inline void kv_batch_seed_prev_mp_from_scout(dsd_state *st, uint8_t slot)
{
  mbe_parms pre;
  if (avr_scout_get_pre_window_mbe_parms(slot, &pre) == 0)
  {
    memcpy(st->prev_mp, &pre, sizeof(mbe_parms));
  }
}

static inline void kv_accumulate_frame(dsd_opts *opts, dsd_state *st,
                                       int slot, uint8_t kid, int alg_id,
                                       double *s_sum, int *s_cnt,
                                       int *ok_frames, int *bad_errs2)
{
  const int errs2_local = st->errs2;
  float s_local = 0.0f;
  kv_compute_s(st->cur_mp, st->prev_mp, &s_local);
  // kv_after_mbe_core_batch(opts, st, slot, kid, alg_id, s_local, errs2_local);
  kv_after_mbe_core_batch(opts, st);

  if (errs2_local >= 0 && errs2_local <= 3 && isfinite(s_local))
  {
    *s_sum += (double)s_local;
    *s_cnt += 1;
    (*ok_frames)++;
  }
  else
  {
    (*bad_errs2)++;
  }
}

// extern-ы у тебя уже есть выше:
extern const int rW[36];
extern const int rX[36];
extern const int rY[36];
extern const int rZ[36];

// Раскладка 27 байт (216 бит, MSB->LSB) в ТРИ зашифрованных AMBE-кадра,
// точно по той же схеме, что live-ветка (36 дибитов/кадр через rW/rX/rY/rZ).
static inline void kv_unpack_27B_enc_to_ambe3(const uint8_t enc27[27],
                                              char fr1[4][24],
                                              char fr2[4][24],
                                              char fr3[4][24])
{
  // 1) распакуем в 216 бит (MSB-first)
  uint8_t b[216];
  int bit = 0;
  for (int i = 0; i < 27; i++)
  {
    for (int k = 7; k >= 0; k--)
      b[bit++] = (enc27[i] >> k) & 1u;
  }

  // 2) три кадра по 36 дибитов = 72 бита каждый
  const uint8_t *p = b; // frame1: b[0..71], frame2: b[72..143], frame3: b[144..215]

  // кадр 1
  memset(fr1, 0, sizeof(char) * 4 * 24);
  for (int i = 0; i < 36; i++)
  {
    uint8_t b1 = *p++; // bit1 (MSB of dibit)
    uint8_t b0 = *p++; // bit0 (LSB of dibit)
    fr1[rW[i]][rX[i]] = (char)b1;
    fr1[rY[i]][rZ[i]] = (char)b0;
  }

  // кадр 2
  memset(fr2, 0, sizeof(char) * 4 * 24);
  for (int i = 0; i < 36; i++)
  {
    uint8_t b1 = *p++;
    uint8_t b0 = *p++;
    fr2[rW[i]][rX[i]] = (char)b1;
    fr2[rY[i]][rZ[i]] = (char)b0;
  }

  // кадр 3
  memset(fr3, 0, sizeof(char) * 4 * 24);
  for (int i = 0; i < 36; i++)
  {
    uint8_t b1 = *p++;
    uint8_t b0 = *p++;
    fr3[rW[i]][rX[i]] = (char)b1;
    fr3[rY[i]][rZ[i]] = (char)b0;
  }
}

// --- MSB-first дибит из 27B
static inline uint8_t kv_get_dibit_from_enc27(const uint8_t enc27[27], int k)
{
  const int byte = k >> 2;
  const int shift = 6 - ((k & 3) << 1);
  return (enc27[byte] >> shift) & 0x3;
}

// --- строгая распаковка 27B -> 3 AMBE (обратная scout_pack_27bytes_from_superframe)
extern const int rW[36], rX[36], rY[36], rZ[36];
static inline void kv_unpack_27B_enc_to_ambe3_strict(const uint8_t enc27[27],
                                                     char fr1[4][24], char fr2[4][24], char fr3[4][24])
{
  memset(fr1, 0, 4 * 24);
  memset(fr2, 0, 4 * 24);
  memset(fr3, 0, 4 * 24);

  // VF #1: dibits 0..35
  for (int i = 0; i < 36; i++)
  {
    uint8_t d = kv_get_dibit_from_enc27(enc27, 0 + i);
    fr1[rW[i]][rX[i]] = (d >> 1) & 1;
    fr1[rY[i]][rZ[i]] = d & 1;
  }
  // VF #2: dibits 36..71 (разложено в 2×18 как в online)
  for (int i = 0; i < 18; i++)
  {
    uint8_t d = kv_get_dibit_from_enc27(enc27, 36 + i);
    fr2[rW[i]][rX[i]] = (d >> 1) & 1;
    fr2[rY[i]][rZ[i]] = d & 1;
  }
  for (int i = 18; i < 36; i++)
  {
    uint8_t d = kv_get_dibit_from_enc27(enc27, 54 + (i - 18));
    fr2[rW[i]][rX[i]] = (d >> 1) & 1;
    fr2[rY[i]][rZ[i]] = d & 1;
  }
  // VF #3: dibits 72..107
  for (int i = 0; i < 36; i++)
  {
    uint8_t d = kv_get_dibit_from_enc27(enc27, 72 + i);
    fr3[rW[i]][rX[i]] = (d >> 1) & 1;
    fr3[rY[i]][rZ[i]] = d & 1;
  }
}

int kv_batch_eval_window(dsd_opts *opts, dsd_state *state, int slot,
                         size_t nsf, uint8_t kid, int alg_id,
                         const uint8_t *enkey_bytes, int key_len,
                         uint32_t start_sf_idx,
                         float *avg_smooth, int *ok_frames, int *bad_errs2)
{
  if (!opts || !state || !enkey_bytes || !avg_smooth || !ok_frames || !bad_errs2)
    return -1;
  if (nsf == 0)
    return -1;
  if (key_len != 16 && key_len != 24 && key_len != 32)
    return -1;

  *avg_smooth = 0.f;
  *ok_frames = 0;
  *bad_errs2 = 0;

  // изолируем стейт
  dsd_state *st = (dsd_state *)malloc(sizeof(dsd_state));
  if (!st)
    return -1;
  memcpy(st, state, sizeof(dsd_state));

  st->cur_mp = (mbe_parms *)calloc(1, sizeof(mbe_parms));
  st->prev_mp = (mbe_parms *)calloc(1, sizeof(mbe_parms));
  st->prev_mp_enhanced = (mbe_parms *)calloc(1, sizeof(mbe_parms));
  if (!st->cur_mp || !st->prev_mp || !st->prev_mp_enhanced)
  {
    free(st->cur_mp);
    free(st->prev_mp);
    free(st->prev_mp_enhanced);
    free(st);
    return -1;
  }
  mbe_initMbeParms(st->cur_mp, st->prev_mp, st->prev_mp_enhanced);

  KVTR("[KV-TRACE] alg_id %d!\n", alg_id);

  // применяем ключ
  if (kv_apply_runtime_key_from_bytes(opts, st, alg_id, enkey_bytes, key_len, (int)kid) != 0)
  {
    KVTR("[KV-TRACE] kv_apply_runtime_key_from_bytes error %d!\n", alg_id);
    free(st->cur_mp);
    free(st->prev_mp);
    free(st->prev_mp_enhanced);
    free(st);
    return -1;
  }
  KVTR("[KV-TRACE] kv_apply_runtime_key_from_bytes OK!!! %d!\n", alg_id);

  st->is_simulation_active = 1;

  // слотовая/crypto маркировка
  st->currentslot = (slot & 1);
  st->payload_algid = (uint8_t)alg_id;
  st->payload_algidR = (uint8_t)alg_id;
  st->H = 1;
  st->aes_key_loaded[0] = 1;
  st->aes_key_loaded[1] = 1;
  st->dmr_so |= 0x40;
  st->dmr_soR |= 0x40;
  const uint8_t cur_slot = st->currentslot;
  // const uint8_t cur_keyid = st->currentslot  ? (uint8_t)st->payload_keyidR : (uint8_t)st->payload_keyid;

  // seed prev_mp "до" первого VC*
  mbe_parms seed_prev;
  // if (avr_scout_get_pre_window_mbe_parms((uint8_t)cur_slot, start_sf_idx, &seed_prev) == 0)
  if (avr_scout_get_pre_window_mbe_parms((uint8_t)cur_slot, &seed_prev) == 0)
  {
    memcpy(st->prev_mp, &seed_prev, sizeof(mbe_parms));
  }
  // >>> КРИТИЧЕСКОЕ: обнуляем счётчики ОДИН РАЗ перед всей серией из 6×VC* (18 VF)
  st->DMRvcL = 0;       // считать VF 0..17 как в live
  st->bit_counterL = 0; // сбросить битовый счётчик OFB только один раз

  double s_sum = 0.0;
  int s_cnt = 0;
  KVTR("[KV-TRACE] for (size_t si = 0; si < %d; ++si)!\n", nsf);

  for (size_t si = 0; si < nsf; ++si)
  { // nsf = кол-во KV-SF в окне!
    const uint32_t sf_abs = start_sf_idx + si;
    // KVTR("[KV-TRACE] sf=%u: no hist entry\n", sf_abs);

    // достаём запись:
    uint8_t six27[6][27];
    uint8_t sixIV[6][16];
    uint8_t mask = 0;

    int rc = avr_scout_get_6x27B_and_6xIV_by_sf((uint8_t)cur_slot, sf_abs, six27, sixIV, &mask);
    if (rc < 0)
    {
      KVTR("[KV-TRACE] sf=%u: no hist entry\n", sf_abs);
      continue;
    }

    /* сбрасываем счётчики VF/битов ТОЛЬКО при входе в ПЕРВЫЙ KV-SF окна */
    if (si == 0)
    {
      st->DMRvcL = 0;
      st->bit_counterL = 0;
    }
    int res = 0;

    for (int b = 0; b < 6; ++b)
    {
      if ((mask & (1u << b)) == 0)
      {
        KVTR("[KV-TRACE] пропуск неполных VC*\n");
        continue; // пропуск неполных VC*
      }

      memcpy(st->aes_iv, sixIV[b], 16); // IV per VC*
      // НЕ СБРАСЫВАТЬ DMRvcL/bit_counterL здесь!

      char fr1[4][24], fr2[4][24], fr3[4][24];
      kv_unpack_27B_enc_to_ambe3_strict(six27[b], fr1, fr2, fr3);
      // KVTR("[KV-TRACE] int vf = 0; vf < 3; ++vf\n");
      for (int vf = 0; vf < 3; ++vf)
      {
        char (*ambe)[24] = (vf == 0) ? fr1 : (vf == 1 ? fr2 : fr3);
        processMbeFrame(opts, st, NULL, ambe, NULL);
        float s_local = 0.f;
        // kv_compute_s(st->cur_mp, st->prev_mp, &s_local);
        res = kv_after_mbe_core_batch(opts, st); // , (slot & 1), kid, (uint8_t)alg_id, s_local, st->errs2);
        mbe_moveMbeParms(st->cur_mp, st->prev_mp);
      }
    }
    if (res != 0)
      break;
  }

  if (s_cnt > 0)
    *avg_smooth = (float)(s_sum / (double)s_cnt);

  free(st->cur_mp);
  free(st->prev_mp);
  free(st->prev_mp_enhanced);
  free(st);
  return 0;
}

void avr_kv_batch_run(dsd_opts *opts, dsd_state *st)
{
  const avr_scout_export_t *E = avr_scout_export();
  if (!E)
    return;

  if (gKS_loaded || gKS.n > 0)
    st->kv_enum_count = (int)gKS.n;

  if (!opts->kv_batch_enable)
    return;

  if (gKS.n == 0)
  {
    fprintf(stderr, "[KV-BATCH] no keys loaded (gKS.n=0)\n");
    return;
  }

  KVTR("[KV-TRACE] batch_run: groups=%d keys=%zu\n", E->ngroups, gKS.n);

  for (int gi = 0; gi < E->ngroups; ++gi)
  {
    const avr_scout_group_t *G = &E->groups[gi];
    if (G->nwins <= 0)
      continue;

    fprintf(stderr,
            "\n MS group[%d] slot=%d alg=0x%02X kid=%u tg=%u src=%u. windows=%d (keys=%zu)\n",
            gi, G->slot, G->alg_id, (unsigned)G->key_id, G->tg, G->src, G->nwins, gKS.n);

    for (int wi = 0; wi < G->nwins; ++wi)
    {
      const avr_scout_window_ref_t *W = &G->win[wi];
      fprintf(stderr,
              "[KV-BATCH] APPLY group slot=%d alg=0x%02X kid=%u tg=%u src=%u | win=%d sf=%d\n",
              G->slot, G->alg_id, (unsigned)G->key_id, G->tg, G->src, wi, W->len_sf);

      // --- Больше НЕ собираем enc_window и НЕ проверяем длину ---
      KVTR("[KV-TRACE] window ok: start_sf=%u len_sf=%d (scout-backed)\n",
           (unsigned)W->start_sf_idx, W->len_sf);
      for (size_t ki = 0; ki < gKS.n; ++ki)
      {
        const kv_key_t *K = &gKS.v[ki];
        if (K->alg_id != G->alg_id)
          continue;
        opts->curr_index = K->index;

        // ЛОГ: ALG/KID/LEN + первые байты ключа
        char kfull[3 * 32 + 1] = "";
        for (size_t j = 0; j < K->key_len && j < 32; ++j)
          sprintf(kfull + strlen(kfull), "%02X ", K->key[j]);
        if (strlen(kfull) > 0 && kfull[strlen(kfull) - 1] == ' ')
          kfull[strlen(kfull) - 1] = '\0';

        fprintf(stderr,
                "[KV-BATCH] KEY[%zu/%zu] alg=0x%02X kid=%u len=%u key=[%s]\n",
                ki + 1, gKS.n, K->alg_id, K->key_id, K->key_len, kfull);
        const uint8_t kid = (uint8_t)G->key_id; // ВАЛИДИРУЕМ пару (slot, G->key_id)
        // fprintf(stderr, "kv_reset_pair_local\n");
        kv_reset_pair_local(G->slot, kid);

        float avg_s = 0.0f;
        int ok = 0, bad = 0;

        fprintf(stderr, "kv_batch_eval_window K->index %" PRIu64 "\n\n", K->index);

        // НОВЫЙ вызов: БЕЗ enc_window, всё берём из scout per-SF
        int rc = kv_batch_eval_window(
            opts, st,
            G->slot,            // slot
            (size_t)W->len_sf,  // nsf
            kid,                // kid (из группы!)
            G->alg_id,          // alg_id
            K->key, K->key_len, // key bytes/len
            W->start_sf_idx,    // абсолютный старт окна
            &avg_s, &ok, &bad   // метрики окна
        );

        // KVTR("[KV-TRACE] eval done: rc=%d slot=%d kid=%u ok=%d bad=%d avg_s=%.3f\n",
        //     rc, G->slot, (unsigned)kid, ok, bad, avg_s);

        int total_sf = 0, good_sf = 0, bad_sf = 0;
        kv_hist_stats(G->slot, kid, &total_sf, &good_sf, &bad_sf);
        KVTR("[KV-TRACE] hist before finalize: slot=%d kid=%u total_sf=%d good=%d bad=%d\n",
             G->slot, (unsigned)kid, total_sf, good_sf, bad_sf);

        // финализация именно этой пары (slot,kid)
        kv_batch_finalize_decision_for_pair(opts, st, G->slot, kid);

        kv_hist_stats(G->slot, kid, &total_sf, &good_sf, &bad_sf);
        KVTR("[KV-TRACE] hist after finalize: slot=%d kid=%u total_sf=%d good=%d bad=%d\n",
             G->slot, (unsigned)kid, total_sf, good_sf, bad_sf);
      }
    }
  }
}
