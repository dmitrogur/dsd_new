// avr_kv.c (Совместимая версия с отключенным перебором)
#include "avr_kv.h" // Предполагается, что этот заголовочный файл существует и объявляет публичные структуры и функции.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h> // для bool в функции-заглушке

bf_store_t g_freqKeys = {.v = {0}, .n = 0};

// ===== ПАРАМЕТРЫ ВАЛИДАЦИИ =====
int g_kv_batch_smooth = 0;

// Запасной вариант для порога 'smoothness', если он не задан в опциях
#ifndef KV_S_FOR_STATS_MAX_DEFAULT
#define KV_S_FOR_STATS_MAX_DEFAULT 140.0f
#endif

// Суперкадр (SF) состоит из 18 фреймов голосового кодека (VC)
#ifndef KV_SF_FRAMES
#define KV_SF_FRAMES 18
#endif

// Максимальное количество суперкадров для хранения в истории анализа
#ifndef KV_SF_HIST_MAX
#define KV_SF_HIST_MAX 8
#endif

// Границы отсечения для значения 'smoothness' для избежания выбросов
#ifndef SMOOTH_MIN_CLIP
#define SMOOTH_MIN_CLIP 2.0f
#endif
#ifndef SMOOTH_MAX_CLIP
#define SMOOTH_MAX_CLIP 500.0f
#endif
#ifndef KV_S_OUTLIER
#define KV_S_OUTLIER 800.0f // Порог для экстремальных выбросов
#endif

// --- Параметры для локального анализа суперкадров (SF) ---
#ifndef KV_LOCAL_STDDEV_MAX
#define KV_LOCAL_STDDEV_MAX 20.0f
#endif
#ifndef KV_CLUSTER_WINDOW_LEN
#define KV_CLUSTER_WINDOW_LEN 12
#endif
#ifndef KV_MIN_STABLE_RUN_LEN
#define KV_MIN_STABLE_RUN_LEN 4
#endif
#ifndef KV_MIN_FLAT_RUN_LEN
#define KV_MIN_FLAT_RUN_LEN 3
#endif
#ifndef KV_STABLE_RUN_DELTA_PROC
#define KV_STABLE_RUN_DELTA_PROC 15.0f
#endif
#ifndef KV_STABLE_RUN_DELTA_MIN
#define KV_STABLE_RUN_DELTA_MIN 3.0f
#endif
#ifndef KV_RANGE_PROC
#define KV_RANGE_PROC 40.0f
#endif
#ifndef KV_RANGE_MIN_ABS
#define KV_RANGE_MIN_ABS 10.0f
#endif
#ifndef KV_FLAT_RUN_PROC
#define KV_FLAT_RUN_PROC 15.0f
#endif
#ifndef KV_FLAT_RUN_MIN_ABS
#define KV_FLAT_RUN_MIN_ABS 0.5f
#endif

// --- Параметры для глобального анализа и принятия решений ---
#define KV_MIN_SF_BEFORE_DECISION 3
#define KV_EARLY_VALID_GOOD_SF 5
#define KV_EARLY_FAIL_BAD_SF 3
#define KV_HARD_CAP_TOTAL_SF 8
#ifndef KV_GLOBAL_MIN_POINTS_FOR_DECISION
#define KV_GLOBAL_MIN_POINTS_FOR_DECISION 50
#endif
#ifndef KV_GLOBAL_CV_MAX
#define KV_GLOBAL_CV_MAX 0.35f
#endif
#ifndef KV_AVG_COEFF_VALID_THR
#define KV_AVG_COEFF_VALID_THR 4.5f
#endif
#ifndef PROBABILITY_THRESHOLD_OK
#define PROBABILITY_THRESHOLD_OK 60
#endif

// --- Буферы и утилиты логирования ---
#define KV_GLOBAL_MAX_POINTS (KV_SF_HIST_MAX * KV_SF_FRAMES)
#define KV_LOG_ERR(...) fprintf(stderr, "[KV - er] " __VA_ARGS__)
#define KV_LOG_SKIP(...) fprintf(stderr, "[KV skip] " __VA_ARGS__)
#define KV_LOG_FECERR(fmt, ...) fprintf(stderr, "[KV FEC ERROR] " fmt, ##__VA_ARGS__)
#define KV_LOG_OK1(...) fprintf(stderr, "[KV ok 1] " __VA_ARGS__)
#define KV_LOG_OK2(...) fprintf(stderr, "[KV ok 2] " __VA_ARGS__)
#define KV_LOG_VALI(...) fprintf(stderr, "[KV VALIDATED] " __VA_ARGS__)
#define KV_LOG_FAIL(...) fprintf(stderr, "[KV FAIL] " __VA_ARGS__)

//-------------------------- Локальные структуры ---------------------------

// Хранит сырые данные одного суперкадра перед анализом
typedef struct
{
    float s[KV_SF_FRAMES];
    uint8_t n_valid;
    uint8_t n_vc;
    uint8_t n_err;
    uint8_t n_skip;
    uint8_t n_gate;
} kv_sf_buf_t;

// Хранит рассчитанные метрики для одного суперкадра
typedef struct
{
    uint8_t valid_cnt;
    uint8_t good;
    float std_dev;
    float min_range;
    uint8_t max_stable_run;
    uint8_t max_flat_run;
    uint8_t quality_coeff;
} kv_sf_metrics_t;

// Контейнер для истории суперкадров
typedef struct
{
    kv_sf_metrics_t m;
    uint8_t in_use;
    uint8_t quality_score;
} kv_hist_cell_t;

//------------------------------ Глобальные переменные --------------------------------

// Буфер для текущего обрабатываемого суперкадра, по слоту и KID
static kv_sf_buf_t g_sf[2][256];
// Кольцевой буфер для истории последних N проанализированных суперкадров
static kv_hist_cell_t g_hist[2][256][KV_SF_HIST_MAX];
// Указатель на текущую позицию в кольцевом буфере
static uint8_t g_hist_pos[2][256];
// Количество валидных суперкадров в истории
static uint8_t g_hist_count[2][256];
// Глобальный буфер со всеми валидными значениями 's' для долгосрочного анализа
static float g_raw_buf[2][256][KV_GLOBAL_MAX_POINTS];
static uint16_t g_raw_len[2][256];
// Отслеживание последних ALGID и KID для обнаружения изменений
static uint8_t kv_last_algid[2] = {0, 0};
static uint8_t kv_last_kid[2] = {0, 0};
// Счетчики прогресса (используются только для логирования)
static uint8_t g_seen_ok[2][256];

static int jb_loaded = 0;



static inline uint32_t read_u32_auto(const char *s, int base, uint32_t defv)
{
    if (!s || !*s)
        return defv;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, base);
    return (end == s) ? defv : (uint32_t)v;
}

static inline int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    return -1;
}
static int hexstr_to_bytes(const char *s, uint8_t *out, int max_out)
{
    // допускаем пробелы, запятые и пр. — игнорируем, берём только [0-9a-fA-F]
    uint8_t tmp[128];
    int tn = 0;
    for (const char *p = s; *p; ++p)
    {
        int h = hex_digit(*p);
        if (h >= 0)
        {
            if (tn >= (int)sizeof(tmp))
                break;
            tmp[tn++] = (uint8_t)h;
        }
    }
    if (tn < 2)
        return 0;
    if (tn & 1)
    { // нечётно — добиваем 0 в конец
        if (tn >= (int)sizeof(tmp))
            return 0;
        tmp[tn++] = 0;
    }
    int outn = tn / 2;
    if (outn > max_out)
        outn = max_out;
    for (int i = 0; i < outn; i++)
    {
        out[i] = (uint8_t)((tmp[2 * i] << 4) | tmp[2 * i + 1]);
    }
    return outn;
}


// CSV: Freq,ALG_ID,KEY_ID,CC,TG,Enkey
// Разрешается: любые пустые поля, КРОМЕ Enkey
#ifndef KV_TYT_AUTO_ID
#define KV_TYT_AUTO_ID 0xFE
#endif

// --- локальные утилиты для этого файла (один раз) ---
static int is_hex_digit_(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_hex_token(const char *s)
{
  if (!s) return 0;
  // разрешаем префикс 0x/0X
  if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) s += 2;
  if (!*s) return 0;
  for (const char *p=s; *p; ++p) if (!is_hex_digit_((unsigned char)*p)) return 0;
  return 1;
}
static int parse_hex_u8_or_neg1(const char *s) // для alg/key
{
  if (!s || !*s) return -1;
  if (!is_hex_token(s)) return -1;
  unsigned long v = strtoul(s, NULL, 0); // 0x.. или без префикса
  if (v > 0xFFUL) v = 0xFFUL;
  return (int)v;
}

// CSV: Freq,ALG_ID,KEY_ID,CC,TG,Enkey
// Разрешается: любые пустые поля, КРОМЕ Enkey
int kv_load_freq_keys_from_csv(const char *path, bf_store_t *store)
{
  if (!path || !*path || !store) return -1;
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Key validation: can't open CSV '%s'\n", path);
    return -2;
  }

  store->n = 0;
  char line[1024];
  int  ln = 0;

  while (fgets(line, sizeof(line), f))
  {
    ln++;
    // пропускаем комментарии/пустые
    char *p = line;
    while (*p==' '||*p=='\t') p++;
    if (*p=='#' || *p=='\n' || *p=='\0' || *p=='\r') continue;

    // распил по запятым, оставляя пустые
    char *fld[8] = {0};
    int   nf = 0;
    for (char *q=p; nf<8; )
    {
      fld[nf++] = q;
      char *c = strchr(q, ',');
      if (!c) break;
      *c = 0;
      q = c+1;
    }

    // трим пробелы по краям
    for (int i=0;i<nf;i++) if (fld[i]) {
      // left trim
      char *s = fld[i];
      while (*s==' '||*s=='\t') s++;
      if (s != fld[i]) memmove(fld[i], s, strlen(s)+1);
      // right trim
      size_t L = strlen(fld[i]);
      while (L>0 && (fld[i][L-1]==' '||fld[i][L-1]=='\t'||fld[i][L-1]=='\r'||fld[i][L-1]=='\n')) fld[i][--L]=0;
    }

    // найти последнюю НЕПУСТУЮ колонку — это Enkey
    int idx_en = -1;
    for (int i = nf-1; i >= 0; --i) {
      if (fld[i] && *fld[i]) { idx_en = i; break; }
    }
    if (idx_en < 0) {
      fprintf(stderr, "CSV[%d]: empty Enkey — skipped\n", ln);
      continue;
    }
    const char *cEN = fld[idx_en];

    if (store->n >= KV_FREQ_MAX) {
      fprintf(stderr, "CSV: reached limit %d lines — stop\n", KV_FREQ_MAX);
      break;
    }

    // индексы полей, если есть
    const char *cFREQ = (nf >= 1) ? fld[0] : "";
    const char *cALG  = (nf >= 2) ? fld[1] : "";
    const char *cKID  = (nf >= 3) ? fld[2] : "";
    // CC/TG при 6 полях: [3]=CC, [4]=TG; при 5 полях: [3]=TG
    const char *cCC   = NULL;
    const char *cTG   = NULL;
    if (nf >= 6) { cCC = fld[3]; cTG = fld[4]; }
    else if (nf == 5) { cTG = fld[3]; }

    freq_key_t *K = &store->v[store->n];
    memset(K, 0, sizeof(*K));
    K->cc = -1;
    K->tg = -1;

    // Freq десятичный
    K->freq = (cFREQ && *cFREQ) ? (uint32_t)strtoul(cFREQ, NULL, 10) : 0;

    // ALG_ID — только hex; если нет значения → 0xFE; если >0xA0 → 0xFE
    int a = parse_hex_u8_or_neg1(cALG);
    if (a < 0) K->alg_id = 0xFE;                // нет значения/мусор → TYT_AUTO
    // else if ((unsigned)a > 0xA0u) K->alg_id = 0xFE; // вымышленное — привязываем к AUTO/TYT
    else K->alg_id = (uint8_t)a;

    // KEY_ID — только hex; если пусто/мусор → 0
    int kid = parse_hex_u8_or_neg1(cKID);
    K->key_id = (kid < 0) ? 0 : (uint8_t)kid;

    // CC/TG (десятичные, опциональны)
    if (cCC && *cCC) K->cc = (int)strtol(cCC, NULL, 10);
    if (cTG && *cTG) K->tg = (int)strtol(cTG, NULL, 10);

    // Enkey → bytes (HEX с пробелами)
    int nbytes = hexstr_to_bytes(cEN, K->enkey, (int)sizeof(K->enkey));
    if (nbytes <= 0) {
      fprintf(stderr, "CSV[%d]: bad Enkey hex — skipped\n", ln);
      continue;
    }
    K->key_len = (uint8_t)nbytes;

    store->n++;
  }

  fclose(f);
  fprintf(stderr, "Key validation: loaded %d freq-keys from %s\n", store->n, path);
  return store->n;
}

void kv_init(dsd_opts *opts, dsd_state *st)
{
    if (!opts)
        return;
    if (jb_loaded)
        return; // уже загружено
    if (opts->fb_csv_path[0] == '\0')
        return;

    int rc = kv_load_freq_keys_from_csv(opts->fb_csv_path, &g_freqKeys);
    if (rc > 0 && g_freqKeys.n > 0)
    {
        jb_loaded = 1;
        fprintf(stderr, "Key validation: FREQ-CSV path set to %s (n=%d, cap=%d).\n", opts->fb_csv_path, g_freqKeys.n, KV_FREQ_MAX);
        // Автоприменить ПЕРВЫЙ ключ, если есть
        const freq_key_t *K = &g_freqKeys.v[0];
        kv_apply_key_for_name_bytes(opts, st, K->alg_id, K->enkey, K->key_len, K->key_id);
        // kv_apply_runtime_key_from_bytes(opts, st, K->alg_id, K->enkey, K->key_len, K->key_id);

        fprintf(stderr, "[KV] auto-applied single CSV key: alg=0x%02X len=%u (key_id=0x%02X)\n",
                K->alg_id, K->key_len, K->key_id);
    }
    else
        fprintf(stderr, "Key validation: freq-CSV load failed: %s\n", opts->fb_csv_path);
}

//------------------- Вспомогательные функции (математика и система) -------------------
// Создает директорию и родительские каталоги, если они не существуют
int kv_mkdir_p(const char *path)
{
    struct stat st;
    if (!path || !*path)
    {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &st) == 0)
    {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0)
    {
        return -1;
    }
    return 0;
}

// Сортировка вставками для небольших массивов
static inline void kv_isort(float *a, int n)
{
    for (int i = 1; i < n; i++)
    {
        float v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v)
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

// Абсолютное расстояние в циклическом пространстве (для углов)
static inline float kv_wrap_abs(float a, float b, float half)
{
    float d = fmodf(a - b, half * 2.0f);
    if (d > half)
        d -= half * 2.0f;
    if (d < -half)
        d += half * 2.0f;
    return fabsf(d);
}

// Нормализованное угловое расстояние (0..1)
static inline float kv_angle_unit01(float a, float b)
{
    const float h1 = (float)M_PI;
    float d = kv_wrap_abs(a, b, h1);
    return d / h1;
}

// Вычисляет среднее и стандартное отклонение для массива float
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

//------------------- Основная логика валидации (анализ Smoothness) -------------------

// Сбрасывает состояние валидации для пары (slot, kid)
void kv_reset_pair_local(int slot, uint8_t kid)
{
    memset(&g_sf[slot][kid], 0, sizeof(kv_sf_buf_t));
    g_hist_count[slot][kid] = 0;
    g_hist_pos[slot][kid] = 0;
    memset(g_hist[slot][kid], 0, sizeof(g_hist[slot][kid]));
    g_seen_ok[slot][kid] = 0;
    g_raw_len[slot][kid] = 0;
}

// Вычисляет метрику 'smoothness' между двумя AMBE-фреймами
static inline float kv_ambe_smooth_score_ex3(const mbe_parms *cur, const mbe_parms *prev)
{
    if (!cur || !prev)
        return 9999.0f;
    float sb = 0.0f;
    sb += fabsf(cur->w0 - prev->w0) / (float)M_PI;
    sb += fabsf(cur->gamma - prev->gamma) / (fabsf(cur->gamma) + fabsf(prev->gamma) + 1e-6f);
    int N = (cur->L < prev->L) ? cur->L : prev->L;
    if (N < 0)
        N = 0;
    if (N > 57)
        N = 57;
    float sv = 0.0f, sm = 0.0f, sl = 0.0f, sp = 0.0f, ss = 0.0f;
    for (int i = 0; i < N; i++)
    {
        sv += (cur->Vl[i] != prev->Vl[i]) ? 1.0f : 0.0f;
        const float ma = fabsf(cur->Ml[i]), mb = fabsf(prev->Ml[i]);
        sm += fabsf(cur->Ml[i] - prev->Ml[i]) / (ma + mb + 1e-6f);
        sl += fabsf(cur->log2Ml[i] - prev->log2Ml[i]) * 0.1f;
        sp += kv_angle_unit01(cur->PHIl[i], prev->PHIl[i]);
        ss += kv_angle_unit01(cur->PSIl[i], prev->PSIl[i]);
    }
    return sb + sv + sm + sl + sp + ss;
}

// Обёртка для вычисления метрики 's'
// static inline void kv_compute_s(const mbe_parms *cur, const mbe_parms *prev, float *out_s) {
void kv_compute_s(mbe_parms *cur_mp, mbe_parms *prev_mp, float *out_s)
{
    if (out_s)
        *out_s = kv_ambe_smooth_score_ex3(cur_mp, prev_mp);
}

// Добавляет новый суперкадр в кольцевой буфер истории
static void kv_hist_push(int slot, uint8_t kid, const kv_sf_metrics_t *m, uint8_t quality_score)
{
    const uint8_t pos = g_hist_pos[slot][kid];
    g_hist[slot][kid][pos].m = *m;
    g_hist[slot][kid][pos].in_use = 1;
    g_hist[slot][kid][pos].quality_score = quality_score;
    g_hist_pos[slot][kid] = (pos + 1) % KV_SF_HIST_MAX;
    if (g_hist_count[slot][kid] < KV_SF_HIST_MAX)
    {
        g_hist_count[slot][kid]++;
    }
}

// Собирает статистику из истории суперкадров (общее, хорошие, плохие)
void kv_hist_stats(int slot, uint8_t kid, int *T, int *G, int *B)
{
    *T = g_hist_count[slot][kid];
    *G = 0;
    *B = 0;
    if (*T == 0)
        return;
    for (int i = 0; i < *T; i++)
    {
        if (g_hist[slot][kid][i].in_use)
        {
            if (g_hist[slot][kid][i].m.good)
                (*G)++;
            else
                (*B)++;
        }
    }
}

static void kv_sf_metrics_clean(const kv_sf_buf_t *sf, kv_sf_metrics_t *out, uint8_t *quality_score)
{
    // --- 1. Инициализация ---
    memset(out, 0, sizeof(*out));
    out->valid_cnt = sf->n_valid;
    out->std_dev = 999.0f;
    out->min_range = 999.0f;
    *quality_score = 0;

    // Требуем минимальное количество валидных кадров для анализа
    if (sf->n_valid < KV_CLUSTER_WINDOW_LEN)
    {
        out->good = 0;
        return;
    }

    // --- 2. Расчет базовых метрик, требующих сортировки ---
    float sorted_arr[KV_SF_FRAMES];
    memcpy(sorted_arr, sf->s, sf->n_valid * sizeof(float));
    kv_isort(sorted_arr, sf->n_valid);

    // --- 2.1. min_range (разброс самого плотного кластера) ---
    for (int i = 0; i <= sf->n_valid - KV_CLUSTER_WINDOW_LEN; i++)
    {
        float current_range = sorted_arr[i + KV_CLUSTER_WINDOW_LEN - 1] - sorted_arr[i];
        if (current_range < out->min_range)
        {
            out->min_range = current_range;
        }
    }

    // --- 2.2. std_dev (общий разброс) ---
    float mean;
    kv_mean_stddev(sf->s, sf->n_valid, &mean, &out->std_dev);

    // --- 2.3. Медиана (основа для динамических порогов) ---
    const float median_s = (sf->n_valid & 1)
                               ? sorted_arr[sf->n_valid >> 1]
                               : 0.5f * (sorted_arr[(sf->n_valid >> 1) - 1] + sorted_arr[sf->n_valid >> 1]);

    // --- 3. Расчет динамических порогов на основе медианы ---
    const float final_delta_thr = fmaxf(median_s * (KV_STABLE_RUN_DELTA_PROC / 100.0f), KV_STABLE_RUN_DELTA_MIN);
    const float final_range_thr = fmaxf(median_s * (KV_RANGE_PROC / 100.0f), KV_RANGE_MIN_ABS);

    // --- 4. Расчет метрик стабильности во времени (на несортированных данных) ---
    // --- 4.1. max_stable_run (длина самого длинного стабильного участка) ---
    int max_run = 0, run = 0;
    if (sf->n_valid > 0)
    {
        max_run = 1;
        run = 1;
    }
    for (int i = 1; i < sf->n_valid; i++)
    {
        if (fabsf(sf->s[i] - sf->s[i - 1]) <= final_delta_thr)
        {
            run++;
        }
        else
        {
            if (run > max_run)
                max_run = run;
            run = 1;
        }
    }
    if (run > max_run)
        max_run = run;
    out->max_stable_run = (uint8_t)max_run;

    // --- 4.2. max_flat_run (длина идеально ровного участка) ---
    int max_flat = 0, current_flat = 0;
    if (sf->n_valid > 0)
    {
        max_flat = 1;
        current_flat = 1;
        float prev_valid_s = sf->s[0];
        for (int i = 1; i < sf->n_valid; i++)
        {
            const float local_flat_thr = fmaxf(prev_valid_s * (KV_FLAT_RUN_PROC / 100.0f), KV_FLAT_RUN_MIN_ABS);
            if (fabsf(sf->s[i] - prev_valid_s) <= local_flat_thr)
            {
                current_flat++;
                prev_valid_s = sf->s[i];
            }
            else
            {
                if (current_flat > max_flat)
                    max_flat = current_flat;
                current_flat = 1;
                prev_valid_s = sf->s[i];
            }
        }
        if (current_flat > max_flat)
            max_flat = current_flat;
    }
    out->max_flat_run = (uint8_t)max_flat;

    // --- 5. Вынесение вердикта 'good'/'bad' на основе метрик ---
    int stddev_ok = (out->std_dev <= KV_LOCAL_STDDEV_MAX);
    int range_ok = (out->min_range <= final_range_thr);
    int run_ok = (out->max_stable_run >= KV_MIN_STABLE_RUN_LEN); //KV_MIN_STABLE_RUN_LEN
    int flat_ok = (out->max_flat_run >= KV_MIN_FLAT_RUN_LEN);

    out->good = (stddev_ok && range_ok) && (run_ok || flat_ok);

    // --- 6. Расчет КОЭФФИЦИЕНТА КАЧЕСТВА (quality_coeff) ---
    out->quality_coeff = 0;
    if (stddev_ok)
        out->quality_coeff += 2;
    if (range_ok)
        out->quality_coeff += 2;
    if (run_ok)
        out->quality_coeff += 1;
    if (flat_ok)
        out->quality_coeff += 1;
}

static void kv_sf_metrics_clean_batch(const kv_sf_buf_t *sf, kv_sf_metrics_t *out, uint8_t *quality_score)
{
    // --- 1. Инициализация ---
    memset(out, 0, sizeof(*out));
    out->valid_cnt = sf->n_valid;
    out->std_dev = 999.0f;
    out->min_range = 999.0f;
    *quality_score = 0;

    // Требуем минимальное количество валидных кадров для анализа
    if (sf->n_valid < KV_CLUSTER_WINDOW_LEN)
    {
        out->good = 0;
        return;
    }

    // --- 2. Расчет базовых метрик, требующих сортировки ---
    float sorted_arr[KV_SF_FRAMES];
    memcpy(sorted_arr, sf->s, sf->n_valid * sizeof(float));
    kv_isort(sorted_arr, sf->n_valid);

    // --- 2.1. min_range (разброс самого плотного кластера) ---
    for (int i = 0; i <= sf->n_valid - KV_CLUSTER_WINDOW_LEN; i++)
    {
        float current_range = sorted_arr[i + KV_CLUSTER_WINDOW_LEN - 1] - sorted_arr[i];
        if (current_range < out->min_range)
        {
            out->min_range = current_range;
        }
    }

    // --- 2.2. std_dev (общий разброс) ---
    float mean;
    kv_mean_stddev(sf->s, sf->n_valid, &mean, &out->std_dev);

    // --- 2.3. Медиана (основа для динамических порогов) ---
    const float median_s = (sf->n_valid & 1)
                               ? sorted_arr[sf->n_valid >> 1]
                               : 0.5f * (sorted_arr[(sf->n_valid >> 1) - 1] + sorted_arr[sf->n_valid >> 1]);

    // --- 3. Расчет динамических порогов на основе медианы --- 
    const float final_delta_thr = fmaxf(median_s * (KV_STABLE_RUN_DELTA_PROC / 100.0f), KV_STABLE_RUN_DELTA_MIN);
    const float final_range_thr = fmaxf(median_s * (KV_RANGE_PROC / 100.0f), KV_RANGE_MIN_ABS);

    // --- 4. Расчет метрик стабильности во времени (на несортированных данных) ---

    // --- 4.1. max_stable_run (длина самых длинных стабильных участков) ---
    // /// --- НАЧАЛО ИЗМЕНЕНИЙ --- ///
    int max_run1 = 0, max_run2 = 0, current_run = 0;
    if (sf->n_valid > 0)
    {
        current_run = 1;
    }

    for (int i = 1; i < sf->n_valid; i++)
    {
        if (fabsf(sf->s[i] - sf->s[i - 1]) <= final_delta_thr)
        {
            current_run++;
        }
        else
        {
            // Участок закончился, обновляем два максимальных значения
            if (current_run > max_run1)
            {
                max_run2 = max_run1;
                max_run1 = current_run;
            }
            else if (current_run > max_run2)
            {
                max_run2 = current_run;
            }
            current_run = 1; // Начинаем новый участок
        }
    }

    // Проверяем последний участок после окончания цикла
    if (current_run > max_run1)
    {
        max_run2 = max_run1;
        max_run1 = current_run;
    }
    else if (current_run > max_run2)
    {
        max_run2 = current_run;
    }
    
    // Сохраняем самый длинный участок в выходную структуру для статистики и отладки
    out->max_stable_run = (uint8_t)max_run1;
    // /// --- КОНЕЦ ИЗМЕНЕНИЙ --- ///

    // --- 4.2. max_flat_run (длина идеально ровного участка) ---
    // Этот блок оставлен без изменений, так как запрос касался `max_stable_run`
    int max_flat = 0, current_flat = 0;
    if (sf->n_valid > 0)
    {
        max_flat = 1;
        current_flat = 1;
        float prev_valid_s = sf->s[0];
        for (int i = 1; i < sf->n_valid; i++)
        {
            const float local_flat_thr = fmaxf(prev_valid_s * (10.0f / 100.0f), KV_FLAT_RUN_MIN_ABS);
            if (fabsf(sf->s[i] - prev_valid_s) <= local_flat_thr)
            {
                current_flat++;
                prev_valid_s = sf->s[i];
            }
            else
            {
                if (current_flat > max_flat)
                    max_flat = current_flat;
                current_flat = 1;
                prev_valid_s = sf->s[i];
            }
        }
        if (current_flat > max_flat)
            max_flat = current_flat;
    }
    out->max_flat_run = (uint8_t)max_flat;

    // --- 5. Вынесение вердикта 'good'/'bad' на основе метрик ---
    int stddev_ok = (out->std_dev <= 18.0); //KV_LOCAL_STDDEV_MAX
    int range_ok = (out->min_range <= final_range_thr);
    int flat_ok = (out->max_flat_run >= KV_MIN_FLAT_RUN_LEN);

    // /// --- НАЧАЛО ИЗМЕНЕНИЙ --- ///
    // Новая логика проверки стабильных участков:
    // Сигнал считается стабильным, если есть:
    // - один длинный стабильный участок (>= 8, как в старом `run_ok`)
    // - ИЛИ два средних стабильных участка (оба >= 5, как в старом `run_ok2`)
    int one_long_run_ok = (max_run1 >= 8 || max_run2 >= 8); // KV_MIN_STABLE_RUN_LEN
    int two_medium_runs_ok = (max_run1 >= KV_MIN_STABLE_RUN_LEN && max_run2 >= KV_MIN_STABLE_RUN_LEN); // KV_MIN_STABLE_RUN_LEN_MID
    int one_runs_ok = (max_run1 >= KV_MIN_STABLE_RUN_LEN || max_run2 >= KV_MIN_STABLE_RUN_LEN); // KV_MIN_STABLE_RUN_LEN_MID
    
    int run_ok = one_long_run_ok || two_medium_runs_ok;
    // /// --- КОНЕЦ ИЗМЕНЕНИЙ --- ///

    out->good = (stddev_ok && range_ok && one_runs_ok) && (run_ok || flat_ok);

    fprintf(stderr, "!!!! stddev_ok %d, range_ok %d, flat_ok %d, run_ok %d, one_runs_ok %d\n", stddev_ok, range_ok, flat_ok, run_ok, one_runs_ok);
    out->good = (stddev_ok && range_ok && one_runs_ok) && (run_ok || flat_ok) || (stddev_ok && flat_ok && run_ok && one_runs_ok);

    // --- 6. Расчет КОЭФФИЦИЕНТА КАЧЕСТВА (quality_coeff) ---
    // Расчет также обновлен, чтобы использовать новую логику `run_ok`.
    out->quality_coeff = 0;
    if(stddev_ok && flat_ok && run_ok && one_runs_ok)
        out->quality_coeff += 2;
    if (stddev_ok)
        out->quality_coeff += 2;
    if (range_ok)
        out->quality_coeff += 2;
    if (run_ok) // Условие `run_ok` теперь включает проверку одного или двух участков
        out->quality_coeff += 1;
    if (flat_ok)
        out->quality_coeff += 1;
}

// Анализирует полный суперкадр и вычисляет его метрики качества
static void kv_sf_metrics_clean_batch2(const kv_sf_buf_t *sf, kv_sf_metrics_t *out, uint8_t *quality_score)
{
    // --- 1. Инициализация ---
    memset(out, 0, sizeof(*out));
    out->valid_cnt = sf->n_valid;
    out->std_dev = 999.0f;
    out->min_range = 999.0f;
    *quality_score = 0;

    // Требуем минимальное количество валидных кадров для анализа
    if (sf->n_valid < KV_CLUSTER_WINDOW_LEN)
    {
        out->good = 0;
        return;
    }

    // --- 2. Расчет базовых метрик, требующих сортировки ---
    float sorted_arr[KV_SF_FRAMES];
    memcpy(sorted_arr, sf->s, sf->n_valid * sizeof(float));
    kv_isort(sorted_arr, sf->n_valid);

    // --- 2.1. min_range (разброс самого плотного кластера) ---
    for (int i = 0; i <= sf->n_valid - KV_CLUSTER_WINDOW_LEN; i++)
    {
        float current_range = sorted_arr[i + KV_CLUSTER_WINDOW_LEN - 1] - sorted_arr[i];
        if (current_range < out->min_range)
        {
            out->min_range = current_range;
        }
    }

    // --- 2.2. std_dev (общий разброс) ---
    float mean;
    kv_mean_stddev(sf->s, sf->n_valid, &mean, &out->std_dev);

    // --- 2.3. Медиана (основа для динамических порогов) ---
    const float median_s = (sf->n_valid & 1)
                               ? sorted_arr[sf->n_valid >> 1]
                               : 0.5f * (sorted_arr[(sf->n_valid >> 1) - 1] + sorted_arr[sf->n_valid >> 1]);

    // --- 3. Расчет динамических порогов на основе медианы --- 
    // KV_STABLE_RUN_DELTA_PROC
    const float final_delta_thr = fmaxf(median_s * (KV_STABLE_RUN_DELTA_PROC / 100.0f), KV_STABLE_RUN_DELTA_MIN);
    const float final_range_thr = fmaxf(median_s * (KV_RANGE_PROC / 100.0f), KV_RANGE_MIN_ABS);

    // --- 4. Расчет метрик стабильности во времени (на несортированных данных) ---
    // --- 4.1. max_stable_run (длина самого длинного стабильного участка) ---
    int max_run = 0, run = 0;
    if (sf->n_valid > 0)
    {
        max_run = 1;
        run = 1;
    }
    for (int i = 1; i < sf->n_valid; i++)
    {
        if (fabsf(sf->s[i] - sf->s[i - 1]) <= final_delta_thr)
        {
            run++;
        }
        else
        {
            if (run > max_run)
                max_run = run;
            run = 1;
        }
    }
    if (run > max_run)
        max_run = run;
    out->max_stable_run = (uint8_t)max_run;

    // --- 4.2. max_flat_run (длина идеально ровного участка) ---
    int max_flat = 0, current_flat = 0;
    if (sf->n_valid > 0)
    {
        max_flat = 1;
        current_flat = 1;
        float prev_valid_s = sf->s[0];
        for (int i = 1; i < sf->n_valid; i++)
        {
            // KV_FLAT_RUN_PROC
            const float local_flat_thr = fmaxf(prev_valid_s * ( 10.0f / 100.0f), KV_FLAT_RUN_MIN_ABS);
            if (fabsf(sf->s[i] - prev_valid_s) <= local_flat_thr)
            {
                current_flat++;
                prev_valid_s = sf->s[i];
            }
            else
            {
                if (current_flat > max_flat)
                    max_flat = current_flat;
                current_flat = 1;
                prev_valid_s = sf->s[i];
            }
        }
        if (current_flat > max_flat)
            max_flat = current_flat;
    }
    out->max_flat_run = (uint8_t)max_flat;

    // --- 5. Вынесение вердикта 'good'/'bad' на основе метрик ---
    int stddev_ok = (out->std_dev <= KV_LOCAL_STDDEV_MAX); //KV_LOCAL_STDDEV_MAX
    int range_ok = (out->min_range <= final_range_thr);
    int run_ok = (out->max_stable_run >= 8); //KV_MIN_STABLE_RUN_LEN); // Q1
    int flat_ok = (out->max_flat_run >= KV_MIN_FLAT_RUN_LEN);
    int run_ok2 = (out->max_stable_run >= KV_MIN_STABLE_RUN_LEN); //KV_MIN_STABLE_RUN_LEN); // Q1
    fprintf(stderr, "!!!! stddev_ok %d, range_ok, flat_ok, run_ok2", stddev_ok, range_ok, flat_ok, run_ok2);
    out->good = (stddev_ok && range_ok && run_ok2) && (run_ok || flat_ok);
    // out->good = (stddev_ok && range_ok);

    // --- 6. Расчет КОЭФФИЦИЕНТА КАЧЕСТВА (quality_coeff) ---
    out->quality_coeff = 0;
    if (stddev_ok)
        out->quality_coeff += 2;
    if (range_ok)
        out->quality_coeff += 2;
    if (run_ok)
        out->quality_coeff += 1;
    if (flat_ok)
        out->quality_coeff += 1;
}

// Проверяет глобальную стабильность сигнала по всем собранным данным
static int kv_check_global_stability(int slot, uint8_t kid)
{
    const uint16_t n_points = g_raw_len[slot][kid];
    if (n_points < KV_GLOBAL_MIN_POINTS_FOR_DECISION)
        return 0;
    float mean, stddev;
    kv_mean_stddev(g_raw_buf[slot][kid], n_points, &mean, &stddev);
    if (mean < 1.0f)
        return 0;
    float coeff_of_variation = stddev / mean;
    return (coeff_of_variation < KV_GLOBAL_CV_MAX) ? 1 : 0;
}

// Вычисляет средний коэффициент качества по истории
static float calculate_average_coeff(int slot, uint8_t kid)
{
    int total_sf = g_hist_count[slot][kid];
    if (total_sf == 0)
        return 0.0f;
    float coeff_sum = 0;
    for (int i = 0; i < total_sf; i++)
    {
        coeff_sum += g_hist[slot][kid][i].m.quality_coeff;
    }
    return coeff_sum / (float)total_sf;
}

// Вычисляет итоговую вероятность правильности ключа
static int calculate_final_probability(int slot, uint8_t kid)
{
    int total_sf = g_hist_count[slot][kid];
    if (total_sf == 0)
        return 0;
    float avg_coeff = calculate_average_coeff(slot, kid);
    float base_prob = 100.0f * (avg_coeff / 6.0f);
    int time_penalty = (total_sf < 3) ? abs(total_sf - 3) * 10 : abs(total_sf - 3) * 1;
    int final_prob = (int)(base_prob - time_penalty);
    if (final_prob > 100)
        return 100;
    if (final_prob < 0)
        return 0;
    return final_prob;
}

//================================================================================
//======================= ПУБЛИЧНЫЕ И ОСНОВНЫЕ ФУНКЦИИ ===========================
//================================================================================

// Формирует путь для сохранения файлов с результатами.
void kv_build_result_path(const dsd_opts *opts, const char *basename, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    if (opts && opts->kv_results_dir[0])
    {
        snprintf(out, outsz, "%s/%s", opts->kv_results_dir, basename ? basename : "");
    }
    else
    {
        snprintf(out, outsz, "%s", basename ? basename : "");
    }
}

// Записывает файл keyOK_<id>.txt с валидированным ключом.
void kv_write_key_ok_file(const dsd_opts *opts, const dsd_state *state, int kid, int ord)
{

    uint8_t keybuf[36] = {0};
    int keylen = 0;
    const int slot = (state->currentslot & 1);
    uint8_t alg = slot ? (uint8_t)state->payload_algidR : (uint8_t)state->payload_algid;
    uint64_t curr = (opts->curr_index >= 0) ? opts->curr_index : ord;
    fprintf(stderr, "!!!!kv_write_key_ok_file alg = %d, kv_key_probability = %d, opts->curr_index %"PRIu64"\n",  alg, state->kv_key_probability[slot][kid], opts->curr_index);

    if(state->kv_key_probability[slot][kid]>60) {
    if (alg == 0x24)
    { // AES-128
        for (int i = 0; i < 8; i++)
        {
            keybuf[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
        }
        keylen = 16;
    }
    else if (alg == 0x23)
    { // AES-192
        for (int i = 0; i < 8; i++)
        {
            keybuf[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
        }
        keylen = 24;
    }        
    else if (alg == 0x25)
    { // AES-256
        for (int i = 0; i < 8; i++)
        {
            keybuf[i + 0] = (state->A1[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 8] = (state->A2[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 16] = (state->A3[0] >> (56 - (i * 8))) & 0xFF;
            keybuf[i + 24] = (state->A4[0] >> (56 - (i * 8))) & 0xFF;
        }
        keylen = 32;
    }
    else
    {
        return; // Неподдерживаемый алгоритм
    }

    char keyhex[32 * 2 + 1] = {0};
    for (int i = 0; i < keylen; i++)
    {
        sprintf(keyhex + i * 2, "%02X", keybuf[i]);
    }
    char fname[534];
    if (opts->kv_results_dir[0])
    {
        kv_mkdir_p(opts->kv_results_dir);
        snprintf(fname, sizeof(fname), "%s/keyOK_%"PRIu64".txt", opts->kv_results_dir, curr);
    }
    else
    {
        snprintf(fname, sizeof(fname), "keyOK_%"PRIu64".txt", curr);
    }

    FILE *fo = fopen(fname, "w");
    if (!fo)
        return;
    fprintf(fo, "id=%"PRIu64"\n", curr);
    fprintf(fo, "alg_id=0x%02X\n", (unsigned)alg);
    fprintf(fo, "key_id=0x%02X\n", kid);
    fprintf(fo, "key=%s\n", keyhex);
    fprintf(fo, "prob=%u\n", state->kv_key_probability[slot][kid]);
    fprintf(fo, "TG=%u\n", state->lasttg);
    fprintf(fo, "SRC=%u\n", state->lastsrc);
    fprintf(fo, "SLOT=%u\n", slot);
    fprintf(fo, "COLOR=%u\n", state->dmr_color_code);
    fclose(fo);
    }
}

// Вызывается на границе суперкадра (заглушка).
void kv_on_superframe_boundary(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
}

// Вызывается в конце голосовой передачи.
void kv_on_voice_end(dsd_opts *opts, dsd_state *state)
{
    if (!opts || !state || !opts->kv_smooth)
        return;
    const int slot = (state->currentslot & 1);
    const uint8_t kid = slot ? (uint8_t)state->payload_keyidR : (uint8_t)state->payload_keyid;
    const uint8_t algid = slot ? (uint8_t)state->payload_algidR : (uint8_t)state->payload_algid;
    if (algid == 0x00 || kid >= 256)
        return;

    // Если решение уже принято, ничего не делать.
    if (state->dmr_key_validation_status[slot][kid] != KEY_UNKNOWN)
    {
        return;
    }
    int total_sf, good_sf, bad_sf;
    kv_hist_stats(slot, kid, &total_sf, &good_sf, &bad_sf);

    // Если материала для анализа слишком мало, считаем это неудачей
    if (total_sf < KV_MIN_SF_BEFORE_DECISION)
    {
        state->dmr_key_validation_status[slot][kid] = KEY_FAILED;
        KV_LOG_FAIL("alg=0x%02X kid=%u cause=end-of-voice (too short) sf=%d\n",
                    (unsigned)algid, (unsigned)kid, total_sf);
    }
    else
    {
        // Финальный вердикт: если хороших SF больше, чем плохих, - успех.
        if (good_sf > bad_sf)
        {
            state->dmr_key_validation_status[slot][kid] = KEY_VALIDATED;
            state->kv_key_probability[slot][kid] = calculate_final_probability(slot, kid);
            KV_LOG_VALI("alg=0x%02X kid=%u cause=end-of-voice (tie-break) sf=%d prob=%u%%\n",
                        (unsigned)algid, (unsigned)kid, total_sf, state->kv_key_probability[slot][kid]);
        }
        else
        {
            state->dmr_key_validation_status[slot][kid] = KEY_FAILED;
            KV_LOG_FAIL("alg=0x%02X kid=%u cause=end-of-voice (tie-break) sf=%d\n",
                        (unsigned)algid, (unsigned)kid, total_sf);
        }
    }
    kv_reset_pair_local(slot, kid);
}

// Основная функция анализа, вызывается после обработки каждого MBE-фрейма.
void kv_after_mbe(dsd_opts *opts, dsd_state *state)
{
    // fprintf(stderr, "kv_after_mbe start\n");
    //if (!opts || !state || !opts->kv_smooth)
    //    return; //  || !opts->kv_smooth
    const int slot = (state->currentslot & 1);
    const uint8_t kid = slot ? (uint8_t)state->payload_keyidR : (uint8_t)state->payload_keyid;
    const uint8_t algid = slot ? (uint8_t)state->payload_algidR : (uint8_t)state->payload_algid;

    // Прекращаем, если нет ALGID, неверный KID, или решение уже принято
    if (algid == 0x00 || kid >= 256)
        return;

    /// if (state->dmr_key_validation_status[slot][kid] != KEY_UNKNOWN) return;

    // fprintf(stderr, "kv_after_mbe state->dmr_key_validation_status[%d][%d] = %d \n", slot, kid, state->dmr_key_validation_status[slot][kid]);

    // Сбрасываем состояние при смене ALGID или KID
    if (kv_last_algid[slot] != algid || kv_last_kid[slot] != kid)
    {
        kv_reset_pair_local(slot, kid);
        kv_last_algid[slot] = algid;
        kv_last_kid[slot] = kid;
    }

    kv_sf_buf_t *sf = &g_sf[slot][kid];
    const float stat_thr = (opts->kv_stat_thr > 0.0f) ? opts->kv_stat_thr : KV_S_FOR_STATS_MAX_DEFAULT;
    const int errs2 = slot ? state->errs2R : state->errs2;

    // --- 1. Первичная фильтрация кадра ---
    sf->n_vc++; // Увеличиваем счетчик полученных кадров в SF
    if (errs2 > KV_UNCORR_ERRS2_THR)
    {
        sf->n_err++;
        // KV_LOG_FECERR("smooth NA\n");
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return;
    }
    // fprintf(stderr, "kv_after_mbe errs2 (%d) <= KV_UNCORR_ERRS2_THR) \n", errs2);

    float s = 0.0f;
    kv_compute_s(state->cur_mp, state->prev_mp, &s);

    // Отбрасываем кадр, если значение 's' является выбросом
    if (s < SMOOTH_MIN_CLIP || s > SMOOTH_MAX_CLIP)
    {
        sf->n_gate++;
        // KV_LOG_ERR("smooth=%.3f OUTLIER -> GATED\n", s);
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return;
    }

    // Пропускаем кадр (но не считаем ошибкой), если 's' выше порога для статистики
    if (s > stat_thr)
    {
        sf->n_skip++;
        // KV_LOG_SKIP("smooth=%.3f > stat_thr=%.1f -> SKIP\n", s, stat_thr);
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return;
    }

    // --- 2. Накопление валидных данных ---
    if (sf->n_valid < KV_SF_FRAMES)
    {
        sf->s[sf->n_valid++] = s;
    }
    if (DEBUG)
    {
        KV_LOG_OK1("smooth=%.3f ALG=0x%02X KID=%u\n", s, (unsigned)algid, (unsigned)kid);
    }
    // fprintf(stderr, "[KV ok 1] smooth=%.3f ALG=0x%02X KID=%u slot=%d\n" s, (unsigned)algid, (unsigned)kid, slot);
    // fprintf(stderr, "kv_after_mbe 7\n");

    // Если суперкадр еще не заполнен, выходим
    if (sf->n_vc < KV_SF_FRAMES)
        return;

// --- 3. Анализ и принятие решения на границе суперкадра ---
CLOSE_SF:;

    fprintf(stderr, "kv_after_mbe CLOSE_SF\n");

    // Отбрасываем SF, если процент ошибок слишком высок
    if (sf->n_vc > 0 && (100 * (int)sf->n_err) / (int)sf->n_vc > 20)
    {
        memset(sf, 0, sizeof(*sf));
        return;
    }

    // Рассчитываем метрики качества для собранного SF
    kv_sf_metrics_t m;
    uint8_t quality;
    kv_sf_metrics_clean(sf, &m, &quality);

    // Накапливаем сырые данные в глобальный буфер для долгосрочного анализа
    for (int i = 0; i < sf->n_valid; i++)
    {
        uint16_t *pn = &g_raw_len[slot][kid];
        if (*pn < KV_GLOBAL_MAX_POINTS)
        {
            g_raw_buf[slot][kid][(*pn)++] = sf->s[i];
        }
    }
    // Добавляем результаты анализа SF в историю
    kv_hist_push(slot, kid, &m, quality);

    // Получаем общую статистику по всем SF в истории
    int total_sf, good_sf, bad_sf;
    kv_hist_stats(slot, kid, &total_sf, &good_sf, &bad_sf);

    // --- 4. Многоуровневая логика принятия решений ---
    int action = 0; // 0: продолжать, 1: валидировать, -1: отказ

    // --- Путь А: Ранние решения на основе перевеса "хороших" или "плохих" SF ---
    if (total_sf >= KV_MIN_SF_BEFORE_DECISION)
    {
        if (good_sf >= bad_sf + 2)
            action = 1;
        else if (bad_sf >= good_sf + 2)
            action = -1;
        else if (bad_sf >= KV_EARLY_FAIL_BAD_SF)
            action = -1;
        else if (good_sf >= KV_EARLY_VALID_GOOD_SF)
            action = 1;
    }
    // --- Путь Б: Решение на основе среднего коэффициента качества ---
    if (action == 0 && total_sf >= 4)
    {
        float avg_coeff = calculate_average_coeff(slot, kid);
        if (avg_coeff >= KV_AVG_COEFF_VALID_THR)
            action = 1;
    }
    // --- Путь В: Решение на основе глобальной стабильности сигнала ---
    if (action == 0 && total_sf > 2 && kv_check_global_stability(slot, kid))
    {
        action = 1;
    }
    // --- Путь Г: Принудительное решение по таймауту (достигнут лимит SF) ---
    if (total_sf >= KV_HARD_CAP_TOTAL_SF)
    {
        action = (good_sf > bad_sf) ? 1 : -1;
    }

    // --- 5. Выполнение действия и логирование ---
    int final_probability = calculate_final_probability(slot, kid);
    KV_LOG_OK2("SF=%d | std=%.1f rng=%.1f run=%u flat=%u coeff=%u | HIST g=%d b=%d prob=%d%% | -> %s\n",
               total_sf, m.std_dev, m.min_range, (unsigned)m.max_stable_run, (unsigned)m.max_flat_run, (unsigned)m.quality_coeff,
               good_sf, bad_sf, final_probability,
               (action == 1 ? "VALID" : (action == -1 ? "FAIL" : "CONT")));

    if (action == 1)
    {
        state->dmr_key_validation_status[slot][kid] = KEY_VALIDATED;
        state->kv_key_probability[slot][kid] = final_probability;
        KV_LOG_VALI("alg=0x%02X kid=%u sf=%d prob=%u%%\n",
                    (unsigned)algid, (unsigned)kid, total_sf, final_probability);
    }
    else if (action == -1)
    {
        state->dmr_key_validation_status[slot][kid] = KEY_FAILED;
        KV_LOG_FAIL("alg=0x%02X kid=%u sf=%d\n", (unsigned)algid, (unsigned)kid, total_sf);
    }

    // Если решение принято, сбросить всё. Иначе - только буфер текущего SF.
    if (action != 0)
    {
        kv_reset_pair_local(slot, kid);
    }
    else
    {
        memset(sf, 0, sizeof(*sf));
    }
}

// Объявления внешних функций, необходимых для kv_decrypt_ambe_frames
extern void pack_bit_array_into_byte_array(uint8_t *bits, uint8_t *bytes, int byte_count);
extern void unpack_byte_array_into_bit_array(uint8_t *bytes, uint8_t *bits, int byte_count);
extern void aes_ecb_bytewise_payload_crypt(uint8_t *in, uint8_t *key, uint8_t *out, int type, int enc);

// Расшифровывает три AMBE-фрейма, используя текущее состояние.
void kv_decrypt_ambe_frames2(dsd_state *st, char ambe_fr1[4][24], char ambe_fr2[4][24], char ambe_fr3[4][24])
{
    // --- Шаг 1: Подготовка ключа и IV ---
    uint8_t aes_key[32];
    if (st->payload_algid == 0x25)
    { // AES-256
        for (int i = 0; i < 8; i++)
        {
            aes_key[i + 0] = (st->A1[0] >> (56 - (i * 8))) & 0xFF;
            aes_key[i + 8] = (st->A2[0] >> (56 - (i * 8))) & 0xFF;
            aes_key[i + 16] = (st->A3[0] >> (56 - (i * 8))) & 0xFF;
            aes_key[i + 24] = (st->A4[0] >> (56 - (i * 8))) & 0xFF;
        }
    }
    else if (st->payload_algid == 0x24)
    { // AES-128
        for (int i = 0; i < 8; i++)
        {
            aes_key[i + 0] = (st->A1[0] >> (56 - (i * 8))) & 0xFF;
            aes_key[i + 8] = (st->A2[0] >> (56 - (i * 8))) & 0xFF;
        }
    }
    else
    {
        return; // Алгоритм не поддерживается
    }

    uint8_t ofb_iv[16];
    memcpy(ofb_iv, st->aes_iv, 16);

    // --- Шаг 2: Извлечение зашифрованных битов из 3 фреймов (3 * 72 = 216 бит) ---
    uint8_t encrypted_bits[216];
    int bit_idx = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                encrypted_bits[bit_idx++] = ambe_fr1[i][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                encrypted_bits[bit_idx++] = ambe_fr2[i][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                encrypted_bits[bit_idx++] = ambe_fr3[i][j];

    // --- Шаг 3: Упаковка битов в байты (216 бит = 27 байт) ---
    uint8_t encrypted_bytes[27];
    pack_bit_array_into_byte_array(encrypted_bits, encrypted_bytes, 27);

    // --- Шаг 4: AES-OFB дешифрование ---
    uint8_t decrypted_bytes[27];
    size_t produced = 0;
    while (produced < 27)
    {
        uint8_t keystream_block[16];
        // Генерируем блок гаммы: AES_ENC(IV)
        aes_ecb_bytewise_payload_crypt(ofb_iv, aes_key, keystream_block, 2, 1);
        size_t take = (27 - produced > 16) ? 16 : (27 - produced);
        for (size_t k = 0; k < take; k++)
        {
            decrypted_bytes[produced + k] = encrypted_bytes[produced + k] ^ keystream_block[k];
        }
        // Обновляем IV для следующего блока: IV := AES_ENC(IV)
        memcpy(ofb_iv, keystream_block, 16);
        produced += take;
    }

    // --- Шаг 5: Распаковка расшифрованных байтов обратно в биты ---
    uint8_t decrypted_bits[216];
    unpack_byte_array_into_bit_array(decrypted_bytes, decrypted_bits, 27);

    // --- Шаг 6: Вставка расшифрованных битов обратно во фреймы ---
    bit_idx = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                ambe_fr1[i][j] = decrypted_bits[bit_idx++];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                ambe_fr2[i][j] = decrypted_bits[bit_idx++];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 24; j++)
            if ((j < 12) || (j > 14))
                ambe_fr3[i][j] = decrypted_bits[bit_idx++];
}

// --- Функции-заглушки для совместимости с компоновщиком ---
// Эти функции вызываются из других частей проекта, но их функциональность (групповой перебор ключей) удалена.
// Они оставлены пустыми, чтобы избежать ошибок "undefined reference" при сборке.

// Заглушка для обработки заголовка PI (инициация перебора)
void kv_on_pi_header(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
}

// Заглушка для проверки, активен ли перебор
bool getG_enum_active()
{
    return false; // Перебор всегда неактивен
}

// Заглушка для начала перебора
void kv_enum_try_begin(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
}

// Заглушка для обработки каждого кадра во время перебора
void kv_enum_on_frame(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
}

static int kv_ieq(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

static void kv_trim(char **s)
{
    if (!s || !*s)
        return;
    char *p = *s;
    while (*p && isspace((unsigned char)*p))
        p++;
    char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1]))
        *--e = 0;
    *s = p;
}

// Заглушка для загрузки настроек из .ini файла
// INI ключи:
//   js=0|1               -> включить smooth-валидацию (если не задано через CLI)
//   je=0|1               -> выйти при первом OK (если не задано через CLI)
//   jc=path/to/keys.csv  -> путь к CSV с кандидатами (включит smooth, если оно 0)
//   jk=<kid>             -> фильтр по KID (целое), применяется если не задан через CLI
//   ja=arc4|aes128|aes192|aes256|auto  -> фильтр по алгоритму (0=auto, 1=ARC4, 2=AES128, 3=AES192, 4=AES256)
//   jp=./result          -> каталог для результатов
int kv_load_ini_overrides(dsd_opts *opts, dsd_state *st)
{
    if (!opts || !opts->kv_ini_path[0])
        return 0;

    FILE *f = fopen(opts->kv_ini_path, "r");
    if (!f)
        return -1;

    char line[2048];
    while (fgets(line, sizeof(line), f))
    {
        // пропуск пустых/комментных строк
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == ';' || *p == '\0')
            continue;

        // key = value
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';

        char *k = p;
        char *v = eq + 1;
        kv_trim(&k);
        kv_trim(&v);

        if (kv_ieq(k, "jb"))
        {
            if (opts->kv_csv_path[0] == '\0')
            {
                int n = kv_load_freq_keys_from_csv(opts->kv_csv_path, &g_freqKeys);
                if (n <= 0)
                {
                    fprintf(stderr, "Key validation: freq-CSV load failed: %s\n", opts->kv_csv_path);
                    break;
                }
                fprintf(stderr, "Key validation: FREQ-CSV path set to %s (n=%d, cap=%d).\n",
                        opts->kv_csv_path, g_freqKeys.n, KV_FREQ_MAX);

                // Автоприменить ПЕРВЫЙ ключ, если есть
                const freq_key_t *K = &g_freqKeys.v[0];
                kv_apply_key_for_name_bytes(opts, st, K->alg_id, K->enkey, K->key_len, K->key_id);
                fprintf(stderr, "[KV-BATCH] auto-applied single CSV key: alg=0x%02X len=%u (key_id=0x%02X)\n",
                        K->alg_id, K->key_len, K->key_id);
            }
        }
        else if (kv_ieq(k, "js"))
        {
            // CLI имеет приоритет: если уже 1 — не трогаем
            if (opts->kv_smooth == 0)
                opts->kv_smooth = (atoi(v) != 0);
        }
        else if (kv_ieq(k, "je"))
        {
            // exit on first ok; CLI имеет приоритет
            if (opts->kv_exit_on_first_ok == 0)
                opts->kv_exit_on_first_ok = (atoi(v) != 0);
        }
        else if (kv_ieq(k, "jc"))
        {
            // путь к CSV — только если не задан через CLI
            if (opts->kv_csv_path[0] == '\0')
            {
                strncpy(opts->kv_csv_path, v, sizeof(opts->kv_csv_path) - 1);
                opts->kv_csv_path[sizeof(opts->kv_csv_path) - 1] = '\0';
                opts->kv_smooth = 0; // включаем SMOOTH автоматически
                kv_batch_init(opts, st);
                fprintf(stderr, "Key validation: CSV path set to %s.\n", opts->kv_csv_path);
            }
        }
        else if (kv_ieq(k, "jk"))
        {
            if (opts->kv_filter_kid == -1) // только если не задан через CLI
                opts->kv_filter_kid = atoi(v);
        }
        else if (kv_ieq(k, "ja"))
        {
            if (opts->kv_filter_alg == 0) // 0 = auto/не задано (CLI приоритет)
            {
                if (!strcasecmp(v, "arc4"))
                    opts->kv_filter_alg = 1;
                else if (!strcasecmp(v, "aes128"))
                    opts->kv_filter_alg = 2;
                else if (!strcasecmp(v, "aes192"))
                    opts->kv_filter_alg = 3;
                else if (!strcasecmp(v, "aes256"))
                    opts->kv_filter_alg = 4;
                else
                    opts->kv_filter_alg = 0; // auto
            }
        }
        else if (kv_ieq(k, "jp"))
        {
            if (opts->kv_results_dir[0] == '\0') // не перезаписываем CLI
            {
                strncpy(opts->kv_results_dir, v, sizeof(opts->kv_results_dir) - 1);
                opts->kv_results_dir[sizeof(opts->kv_results_dir) - 1] = '\0';
            }
        }
        // неизвестные ключи тихо игнорируем
    }
    fclose(f);

    // краткая телеметрия по итогу
    const char *alg_s = "auto";
    switch (opts->kv_filter_alg)
    {
    case 1:
        alg_s = "arc4";
        break;
    case 2:
        alg_s = "aes128";
        break;
    case 3:
        alg_s = "aes192";
        break;
    case 4:
        alg_s = "aes256";
        break;
    default:
        break;
    }

    fprintf(stderr, "[KV INI] loaded: %s\n", opts->kv_ini_path);
    fprintf(stderr, "[KV INI] js=%d, je=%d, ja=%s, jk=%d\n",
            opts->kv_smooth ? 1 : 0,
            opts->kv_exit_on_first_ok ? 1 : 0,
            alg_s, opts->kv_filter_kid);
    fprintf(stderr, "[KV INI] jc=%s (%s)\n",
            opts->kv_csv_path[0] ? opts->kv_csv_path : "(none)",
            opts->kv_csv_path[0] ? "ok" : "none");
    fprintf(stderr, "[KV INI] jp=%s\n",
            opts->kv_results_dir[0] ? opts->kv_results_dir : "(none)");

    return 0;
}

// Заглушка для корректировки состояния после загрузки настроек
void kv_post_cli_ini_adjust(dsd_opts *opts, dsd_state *state)
{
    (void)opts;
    (void)state;
}

// Заглушка для обработки конца потока
void kv_on_stream_end(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
}

// Устанавливает runtime-ключ (аналог -H/-!),
// вызывается из batch если CSV содержит только один ключ.
// УСТАНОВКА АКТИВНОГО КЛЮЧА В СТЕЙТ (аналог -H/-!) ИЗ СЫРЫХ БАЙТ CSV
// key_len — ровно 16 (AES-128) или 32 (AES-256) байт.
// kid     — можно передать -1/0, если не нужен. В стейте поставим 0.
// helper: pack 8 bytes (big-endian) -> uint64_t (C version)
static inline uint64_t kv_pack64_be(const uint8_t *b)
{
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] << 8) | ((uint64_t)b[7] << 0);
}

// Установить ключ из сырых байт (16 или 32), разложить в A1..A4 (BE),
// проставить alg_id и key_id в стейт. Совместимо с местом, где ты
// собираешь aes_key[] обратно из A1..A4.
// Установить ключ из сырых байт (16 или 32) в A1..A4, как делает -H:
// - дублируем в [0] и [1]
// - поднимаем aes_key_loaded для обоих слотов
// - снимаем мьют шифрованного звука
int kv_apply_runtime_key_from_bytes(dsd_opts *opts, dsd_state *st,
                                    int alg_id,
                                    const uint8_t *key_bytes,
                                    size_t key_len,
                                    int kid)
{
    if (!st || !opts || !key_bytes || !(key_len == 16 || key_len == 32))
    {
        fprintf(stderr, "[KV-RUNTIME] invalid params (len=%d)\n", (int) key_len);
        return -1;
    }
    
    uint64_t A1 = 0, A2 = 0, A3 = 0, A4 = 0;

    if (key_len == 16)
    {
        A1 = kv_pack64_be(&key_bytes[0]);
        A2 = kv_pack64_be(&key_bytes[8]);
    }
    else
    { // 32
        A1 = kv_pack64_be(&key_bytes[0]);
        A2 = kv_pack64_be(&key_bytes[8]);
        A3 = kv_pack64_be(&key_bytes[16]);
        A4 = kv_pack64_be(&key_bytes[24]);
    }

    // Пишем как при -H: и в [0], и в [1]
    st->A1[0] = st->A1[1] = A1;
    st->A2[0] = st->A2[1] = A2;
    st->A3[0] = st->A3[1] = A3;
    st->A4[0] = st->A4[1] = A4;

    // Алгоритм + KID (если есть)
    st->payload_algid = (uint8_t)(alg_id & 0xFF);
    st->payload_keyid = (kid > 0 ? kid : 0);

    // Сигнализируем, что ключ загружен (как -H).
    st->aes_key_loaded[0] = 1;
    st->aes_key_loaded[1] = 1;

    // Снять мьют, чтобы слышать декодированный голос (как -H).
    opts->dmr_mute_encL = 0;
    opts->dmr_mute_encR = 0;

    // Отключить keyloader (в -H так и сделано)
    st->keyloader = 0;

    // fprintf(stderr, "[KV-RUNTIME] runtime key applied: alg=0x%02X kid=%d len=%d\n", st->payload_algid, st->payload_keyid, (int) key_len);
    return 0;
}

//================   Реальный OFB вместо stub ==========
extern void aes_ecb_bytewise_payload_crypt(uint8_t *in, uint8_t *key,
                                           uint8_t *out, int type, int enc);

static int kv_ofb_decrypt_27B(const uint8_t *in, uint8_t *out,
                              const uint8_t *key, uint32_t key_len,
                              const uint8_t iv16[16])
{
    if (!in || !out || !iv16 || !key || (key_len != 16 && key_len != 32))
        return -1;

    uint8_t ofb[16];
    memcpy(ofb, iv16, 16);

    size_t produced = 0;
    while (produced < 27)
    {
        uint8_t ks[16];
        // type=2 (AES256) для key_len=32, type=0 (AES128) для key_len=16; enc=1
        int type = (key_len == 32) ? 2 : 0;
        aes_ecb_bytewise_payload_crypt(ofb, (uint8_t *)key, ks, type, 1);
        size_t take = (27 - produced > 16) ? 16 : (27 - produced);
        for (size_t i = 0; i < take; i++)
            out[produced + i] = in[produced + i] ^ ks[i];
        memcpy(ofb, ks, 16); // OFB chain
        produced += take;
    }
    return 0;
}

//===================== 3) Оффлайн-оценка «как -js», но без влияния на живой декодер ==========================
// Прототипы mbelib/dsd (у вас уже тянутся через dsd.h, оставляю как комментарии)
// int mbe_eccAmbe3600x2450C0 (char ambe_fr[4][24]);
// void mbe_demodulateAmbe3600x2450Data (char ambe_fr[4][24]);
// int mbe_eccAmbe3600x2450Data (char ambe_fr[4][24], char ambe_d[49]);

// Хелпер: распаковать 27B (MSB-first) в 3 AMBE-фрейма (каждый 49 бит «ambe_d» -> затем через mbe_*)
// Гипотеза: порядок битов MSB-first; если у вас в live-тракте обратная полярность — переверните здесь.
static void kv_unpack_27B_to_3_ambe(const uint8_t *b27,
                                    char ambe_d[3][49])
{
    int bit = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 49; j++)
            ambe_d[i][j] = 0;

    for (int i = 0; i < 27; i++)
    {
        for (int b = 7; b >= 0; b--)
        {
            int dst_idx = bit / 49;
            int dst_off = bit % 49;
            if (dst_idx < 3)
            {
                ambe_d[dst_idx][dst_off] = ((b27[i] >> b) & 1);
            }
            bit++;
        }
    }
}

// Хелпер: 49 бит -> ambe_fr[4][24] как ожидают mbe_* (минимально корректное преобразование)
static void kv_bits49_to_ambe_fr(const char ambe_d[49], char ambe_fr[4][24])
{
    // Простая раскладка: ambe_d -> ambe_fr в порядке, который использует mbe_*.
    // Если у вас уже есть pack_ambe/unpack_ambe — используйте их вместо этой функции.
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 24; c++)
            ambe_fr[r][c] = 0;

    for (int i = 0; i < 49; i++)
    {
        int r = (i / 24);
        int c = (i % 24);
        ambe_fr[r][c] = ambe_d[i] & 1;
    }
}

// Возвращает 0 при успехе, выставляет avg_smooth/ok_frames/bad_errs2.
int kv_eval_window_smooth(dsd_opts *opts, dsd_state *st,
                          int slot,
                          const uint8_t *dec27, size_t nsf,
                          float *avg_smooth, int *ok_frames, int *bad_errs2)
{
    (void)slot; // при необходимости можно учесть слот для слотовых счетчиков
    if (!dec27 || nsf == 0)
        return -1;

    int ok = 0, bad = 0;
    double smooth_sum = 0.0;

    for (size_t s = 0; s < nsf; s++)
    {
        const uint8_t *p = dec27 + s * 27;
        char ambe_d[3][49];
        kv_unpack_27B_to_3_ambe(p, ambe_d);

        for (int f = 0; f < 3; f++)
        {
            char ambe_fr[4][24];
            char ambe_bits[49];
            for (int i = 0; i < 49; i++)
                ambe_bits[i] = ambe_d[f][i];

            // emulation: путь mbe_* такой же, как в live
            // (если у вас есть готовая pack_ambe/unpack_ambe — используйте их)
            kv_bits49_to_ambe_fr(ambe_bits, ambe_fr);

            int errs = mbe_eccAmbe3600x2450C0(ambe_fr);
            mbe_demodulateAmbe3600x2450Data(ambe_fr);
            errs += mbe_eccAmbe3600x2450Data(ambe_fr, ambe_bits);

            // Простая метрика «как -js»: ок-фрейм если errs<=2
            if (errs <= 2)
            {
                ok++;
                // упрощённая «smooth» как  (3 - errs) * 10.0  (настройте по своему kv_after_mbe)
                smooth_sum += (3 - errs) * 10.0;
            }
            else
            {
                bad++;
            }
        }
    }

    if (avg_smooth)
        *avg_smooth = (ok > 0) ? (float)(smooth_sum / ok) : 0.0f;
    if (ok_frames)
        *ok_frames = ok;
    if (bad_errs2)
        *bad_errs2 = bad;
    return 0;
}

void kv_batch_finalize_decision_for_pair(dsd_opts *opts, dsd_state *st,
                                         int slot, uint8_t kid)
{
    fprintf(stderr, "[KV final]  slot %u, kid %u \n", slot, kid);
    if (!opts || !st)
        return;
    slot &= 1;
    if (kid >= 256)
        return;
    // fprintf(stderr, "[KV final]  1\n");

    int total_sf = 0, good_sf = 0, bad_sf = 0;
    kv_hist_stats(slot, kid, &total_sf, &good_sf, &bad_sf);

    // итог по истории пары (slot,kid)
    int action = 0;                      // 1=VALID, -1=FAIL, 0=CONT
    const int MIN_SF = AVR_SCOUT_MAX_SF; // окно батча = 6

    // если вообще ничего не накопили — не делаем "FAIL sf=0", просто выходим
    if (total_sf == 0)
    {
        KV_LOG_ERR("batch-final: slot=%d kid=%u has no SF -> skip finalize\n", slot, (unsigned)kid);
        return;
    }
    // fprintf(stderr, "[KV final]  2\n");

    // базовый порог: ждём минимум окно полностью
    if (total_sf >= MIN_SF)
    {
        if (good_sf >= bad_sf + 1)
            action = 1;
        else if (bad_sf >= good_sf + 2)
            action = -1; // чуть жёстче к "плохим"
    }
    //  fprintf(stderr, "[KV final] action %d\n", action);

    // если ещё колеблемся — тай-брейк по среднему коэффициенту
    if (action == 0)
    {
        float avg_coeff = calculate_average_coeff(slot, kid);
        if (avg_coeff >= KV_AVG_COEFF_VALID_THR)
            action = 1;
        else
            action = (good_sf >= bad_sf) ? 1 : -1;
    }

    const uint8_t algid = (slot ? (uint8_t)st->payload_algidR : (uint8_t)st->payload_algid);
    int final_probability = calculate_final_probability(slot, kid);

    if (action == 1)
    {
        st->dmr_key_validation_status[slot][kid] = KEY_VALIDATED;
        st->kv_key_probability[slot][kid] = (unsigned)final_probability;
        KV_LOG_VALI("alg=0x%02X kid=%u (batch-final) sf=%d prob=%u%%\n",
                    (unsigned)algid, (unsigned)kid, total_sf, (unsigned)st->kv_key_probability[slot][kid]);
    }
    else
    {
        st->dmr_key_validation_status[slot][kid] = KEY_FAILED;
        KV_LOG_FAIL("alg=0x%02X kid=%u (batch-final) sf=%d\n",
                    (unsigned)algid, (unsigned)kid, total_sf);
    }

    // очистить локальные накопители, чтобы следующий ключ не наследовал статистику
    kv_reset_pair_local(slot, kid);
}


//====================================================================
//====================================================================
//====================================================================
int kv_after_mbe_core_batch(dsd_opts *opts, dsd_state *state)
{
    //if (!opts || !state || !opts->kv_smooth)
    //    return; //  || !opts->kv_smooth

    const int slot = (state->currentslot & 1);
    const uint8_t kid = slot ? (uint8_t)state->payload_keyidR : (uint8_t)state->payload_keyid;
    const uint8_t algid = slot ? (uint8_t)state->payload_algidR : (uint8_t)state->payload_algid;
    /// fprintf(stderr, "kv_after_mbe_core_batch\n");
    // Прекращаем, если нет ALGID, неверный KID, или решение уже принято
    if (algid == 0x00 || kid >= 256)
        return 0;
    // fprintf(stderr, "algid (%d, kid %d) \n", algid, kid);
    
    /*
    if (state->dmr_key_validation_status[slot][kid] != KEY_UNKNOWN) {
        fprintf(stderr, "return. kv_after_mbe state->dmr_key_validation_status[%d][%d] = %d \n", slot, kid, state->dmr_key_validation_status[slot][kid]);
        return;
    }    
    */
    // Сбрасываем состояние при смене ALGID или KID
    if (kv_last_algid[slot] != algid || kv_last_kid[slot] != kid)
    {
        kv_reset_pair_local(slot, kid);
        kv_last_algid[slot] = algid;
        kv_last_kid[slot] = kid;
    }

    kv_sf_buf_t *sf = &g_sf[slot][kid];
    const float stat_thr = (opts->kv_stat_thr > 0.0f) ? opts->kv_stat_thr : KV_S_FOR_STATS_MAX_DEFAULT;
    const int errs2 = slot ? state->errs2R : state->errs2;

    // --- 1. Первичная фильтрация кадра ---
    sf->n_vc++; // Увеличиваем счетчик полученных кадров в SF
    if (errs2 > KV_UNCORR_ERRS2_THR+2)
    {
        sf->n_err++;
        KV_LOG_FECERR("smooth NA\n");
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return 0;
    }
    // fprintf(stderr, "kv_after_mbe errs2 (%d) <= KV_UNCORR_ERRS2_THR) \n", errs2);

    float s = 0.0f;
    kv_compute_s(state->cur_mp, state->prev_mp, &s);
    // Отбрасываем кадр, если значение 's' является выбросом
    if (s < 5 || s > SMOOTH_MAX_CLIP)
    {
        sf->n_gate++;
        KV_LOG_ERR("smooth=%.3f OUTLIER -> GATED\n", s);
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return 0;
    }

    
    // Пропускаем кадр (но не считаем ошибкой), если 's' выше порога для статистики
    
    if (s > stat_thr)
    {
        sf->n_skip++;
        KV_LOG_SKIP("smooth=%.3f > stat_thr=%.1f -> SKIP\n", s, stat_thr);
        if (sf->n_vc >= KV_SF_FRAMES)
            goto CLOSE_SF;
        return;
    }

    // --- 2. Накопление валидных данных ---
    if (sf->n_valid < KV_SF_FRAMES)
    {
        sf->s[sf->n_valid++] = s;
    }
    if (DEBUG)
    {
        KV_LOG_OK1("smooth=%.3f ALG=0x%02X KID=%u\n", s, (unsigned)algid, (unsigned)kid);
    }
    // fprintf(stderr, "[KV ok 1] smooth=%.3f ALG=0x%02X KID=%u slot=%d\n" s, (unsigned)algid, (unsigned)kid, slot);
    // fprintf(stderr, "kv_after_mbe 7\n");

    // Если суперкадр еще не заполнен, выходим
    if (sf->n_vc < KV_SF_FRAMES)
        return 0;

// --- 3. Анализ и принятие решения на границе суперкадра ---
CLOSE_SF:;

    // fprintf(stderr, "kv_after_mbe CLOSE_SF\n");
    int action = 0; // 0: продолжать, 1: валидировать, -1: отказ

    // Отбрасываем SF, если процент ошибок слишком высок
    if (sf->n_vc > 0 && (100 * (int)sf->n_err) / (int)sf->n_vc > 20)
    {
        memset(sf, 0, sizeof(*sf));
        KV_LOG_OK1("if (sf->n_vc > 0 && (100 * (int)sf->n_err) / (int)sf->n_vc > 20)");
        action = -1;
        return 0;
    }

    // Рассчитываем метрики качества для собранного SF
    kv_sf_metrics_t m;
    uint8_t quality;
    kv_sf_metrics_clean_batch(sf, &m, &quality); //QQQQQQQQQQQQQQQQQQQ

    // Накапливаем сырые данные в глобальный буфер для долгосрочного анализа
    for (int i = 0; i < sf->n_valid; i++)
    {
        uint16_t *pn = &g_raw_len[slot][kid];
        if (*pn < KV_GLOBAL_MAX_POINTS)
        {
            g_raw_buf[slot][kid][(*pn)++] = sf->s[i];
        }
    }
    // Добавляем результаты анализа SF в историю
    kv_hist_push(slot, kid, &m, quality);

    // Получаем общую статистику по всем SF в истории
    int total_sf, good_sf, bad_sf;
    kv_hist_stats(slot, kid, &total_sf, &good_sf, &bad_sf);

    // --- 4. Многоуровневая логика принятия решений ---

    fprintf(stderr, "good_sf %d\n",  good_sf);

    // --- Путь А: Ранние решения на основе перевеса "хороших" или "плохих" SF ---
    if (total_sf >= 1) {
        if(good_sf>0)
            action = 1;
        else     
            action = -1;
    }
    fprintf(stderr, "action %d\n",  action);

    /*
    if (total_sf >= KV_MIN_SF_BEFORE_DECISION)
    {
        if (good_sf >= bad_sf + 2)
            action = 1;
        else if (bad_sf >= good_sf + 2)
            action = -1;
        else if (bad_sf >= KV_EARLY_FAIL_BAD_SF)
            action = -1;
        else if (good_sf >= KV_EARLY_VALID_GOOD_SF)
            action = 1;
    }
    */
    // --- Путь Б: Решение на основе среднего коэффициента качества ---
    /*
    if (action == 0 && total_sf >= 4)
    {
        float avg_coeff = calculate_average_coeff(slot, kid);
        if (avg_coeff >= KV_AVG_COEFF_VALID_THR)
            action = 1;
    }
    // --- Путь В: Решение на основе глобальной стабильности сигнала ---
    if (action == 0 && total_sf > 2 && kv_check_global_stability(slot, kid))
    {
        action = 1;
    }
    // --- Путь Г: Принудительное решение по таймауту (достигнут лимит SF) ---
    if (total_sf >= KV_HARD_CAP_TOTAL_SF)
    {
        action = (good_sf > bad_sf) ? 1 : -1;
    }
    */
    // --- 5. Выполнение действия и логирование ---
    int final_probability = calculate_final_probability(slot, kid);
    KV_LOG_OK2("SF=%d | std=%.1f rng=%.1f run=%u flat=%u coeff=%u | HIST g=%d b=%d prob=%d%% | -> %s\n",
               total_sf, m.std_dev, m.min_range, (unsigned)m.max_stable_run, (unsigned)m.max_flat_run, (unsigned)m.quality_coeff,
               good_sf, bad_sf, final_probability,
               (action == 1 ? "VALID" : (action == -1 ? "FAIL" : "CONT")));
    if(final_probability<65) //QQQQ
        action = -1;
    if (action == 1)
    {
        state->dmr_key_validation_status[slot][kid] = KEY_VALIDATED;
        state->kv_key_probability[slot][kid] = final_probability;
        KV_LOG_VALI("alg=0x%02X kid=%u sf=%d prob=%u%%\n",
                    (unsigned)algid, (unsigned)kid, total_sf, final_probability);
            // kv_result.txt
            long now_ms = dsd_now_ms();
            long t_key_ms = (state->kv_key_t0_ms[slot][kid] > 0) ? now_ms - state->kv_key_t0_ms[slot][kid] : 0;
            long t_total_ms = (state->kv_prog_t0_ms > 0) ? now_ms - state->kv_prog_t0_ms : 0;            
            char kvpath[600];
            kv_build_result_path(opts, "kv_result.txt", kvpath, sizeof(kvpath));
            FILE *pFile; // file pointer
            if ((pFile = fopen(kvpath, "a")) != NULL)
            {
              fprintf(pFile, "KEY_VALIDATED (BS) alg=0x%02X keyid=%d t_key_ms=%ld t_total_ms=%ld prob=%u\n",
                      (unsigned)algid, kid, t_key_ms, t_total_ms, (unsigned)state->kv_key_probability[slot][kid]);
              fclose(pFile);
            }
            // keyOK_<id>.txt
            kv_write_key_ok_file(opts, state, kid, 0);    }
    else if (action == -1)
    {
        state->dmr_key_validation_status[slot][kid] = KEY_FAILED;
        KV_LOG_FAIL("alg=0x%02X kid=%u sf=%d\n", (unsigned)algid, (unsigned)kid, total_sf);
    }

    // Если решение принято, сбросить всё. Иначе - только буфер текущего SF.
    if (action != 0)
    {
        kv_reset_pair_local(slot, kid);
    }
    else
    {
        memset(sf, 0, sizeof(*sf));
    }
    return state->dmr_key_validation_status[slot][kid];
}

static inline void strtrim(char *s)
{
    if (!s)
        return;
    // left trim
    char *p = s;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    // right trim
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    {
        s[--n] = 0;
    }
}

static int split_csv_line(char *line, char *out[8], int max_out)
{
    int n = 0;
    char *p = line;
    while (n < max_out)
    {
        out[n++] = p;
        char *c = strchr(p, ',');
        if (!c)
            break;
        *c = 0;
        p = c + 1;
    }
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

// Парсим строку HEX -> bytes (игнорируем пробелы, допускаем префикс 0x)
// Возвращает кол-во байт или -1
static int parse_hex_bytes(const char *hex, uint8_t *out, size_t out_cap)
{
    if (!hex)
        return -1;
    // пропустим ведущие "0x"/"0X" если есть
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex += 2;

    // Соберём только [0-9A-Fa-f]
    char buf[256];
    size_t m = 0;
    for (const char *p = hex; *p; ++p)
    {
        int v = hexval((unsigned char)*p);
        if (v >= 0)
        {
            if (m + 1 < sizeof(buf))
                buf[m++] = *p;
            else
                return -1;
        }
    }
    if (m == 0 || (m & 1))
        return -1; // нечётная длина hex

    size_t bytes = m / 2;
    if (bytes > out_cap)
        return -1;

    for (size_t i = 0; i < bytes; ++i)
    {
        int hi = hexval((unsigned char)buf[2 * i]);
        int lo = hexval((unsigned char)buf[2 * i + 1]);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)bytes;
}
//===========================================================
//===========================================================
//===========================================================

static inline uint8_t kv_guess_len_by_alg(uint8_t alg_id)
{
    switch (alg_id)
    {
    // ARC4 / Basic Privacy — в природе 5..16 байт. Возьмём безопасный дефолт 16.
    case 0x01:
    case 0x10:
    case 0x21:
        return 16;

    // DES-64
    case 0x22:
        return 8;

    // AES-128
    case 0x11:
    case 0x24:
        return 16;

    // AES-192
    case 0x23:
        return 24;

    // AES-256 (в т.ч. «проприетарные» синонимы)
    case 0x02:
    case 0x05:
    case 0x07:
    case 0x12:
    case 0x25:
        return 32;

    // Hytera HAS / Tytera AP/PC4 — чаще AES-блок, примем 32 по умолчанию
    case 0x90: /*HAS*/
    case 0xA0: /*Tait/закрыт.*/
        return 32;

    default:
        // Неизвестно: дефолтимся на 16 (AES-128)
        return 16;
    }
}
//===========================================================

// считаем кол-во hex-нибблов в C-строке (до '\0'), игнорируя пробелы/таб/разделители/`0x`
static size_t kv_count_hex_nibbles_cstr(const char *s)
{
    if (!s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    size_t n = 0;
    for (const char *p = s; *p; ++p)
        if (isxdigit((unsigned char)*p))
            n++;
    return n;
}

// Упаковка произвольного массива байт в до 4 слов по 8 байт (big-endian)
static size_t kv_bytes_to_u64(const uint8_t *b, size_t blen, unsigned long long *dst, size_t maxw)
{
    size_t used = 0;
    while (used < maxw && blen > 0)
    {
        unsigned long long v = 0;
        size_t take = blen >= 8 ? 8 : blen;
        for (size_t k = 0; k < take; k++)
            v = (v << 8) | b[k];
        dst[used++] = v;
        if (blen <= 8)
            break;
        b += 8;
        blen -= 8;
    }
    return used;
}
// распарсить HEX-строку в байты; нечётный хвост дополняем 0
static size_t kv_hexstr_to_bytes(const char *s, uint8_t *out, size_t cap)
{
    if (!s || !out || !cap)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint8_t nib[512];
    size_t nn = 0;
    for (const char *p = s; *p && nn < sizeof(nib); ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (isxdigit(c))
        {
            if (c <= '9')
                c -= '0';
            else if (c >= 'a')
                c = (unsigned char)(10 + c - 'a');
            else
                c = (unsigned char)(10 + c - 'A');
            nib[nn++] = c;
        }
    }
    if (!nn)
        return 0;
    if (nn & 1)
    {
        if (nn >= sizeof(nib))
            return 0;
        nib[nn++] = 0;
    }
    size_t need = nn / 2;
    if (need > cap)
        need = cap;
    for (size_t i = 0; i < need; i++)
        out[i] = (uint8_t)((nib[2 * i] << 4) | nib[2 * i + 1]);
    return need;
}

static size_t kv_count_hex_nibbles_cstr_V1(const char *s)
{
    if (!s)
        return 0;
    size_t n = 0;
    // пропустим ведущий 0x/0X (если хотят так писать)
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    for (const char *p = s; *p; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (isxdigit(c))
            n++; // считаем только 0-9A-Fa-f
        // остальные символы (пробелы, запятые и т.п.) игнорируем
    }
    return n;
}

// DES=8; RC4/BP → 5..16 (держим в пределах); остальное → AES 16/24/32
static inline uint8_t kv_norm_len_for_alg(uint8_t alg_id, size_t raw_bytes)
{
    if (alg_id == 0x22)
        return 8; // DES
    if (alg_id == 0x01 || alg_id == 0x10 || alg_id == 0x21)
    { // RC4/BP
        if (raw_bytes < 5)
            return 16;
        if (raw_bytes > 16)
            return 16;
        return (uint8_t)raw_bytes;
    }
    if (raw_bytes <= 16)
        return 16;
    if (raw_bytes <= 24)
        return 24;
    return 32;
}

// Сформировать ключевую строку «как в -5/-!/-@»: группы по 8 байт (16 hex), разделённые пробелом.
static void kv_bytes_to_keystring_groups8(const uint8_t *kb, size_t key_len,
                                          char *out, size_t out_cap)
{
    // макс. 4 группы * (16 символов + 1 пробел) + '\0' = 4*17+1 = 69
    size_t groups = (key_len + 7) / 8; // 2 для 16 байт, 4 для 32
    if (groups > 4)
        groups = 4;
    size_t off = 0;

    for (size_t g = 0; g < groups; g++)
    {
        size_t start = g * 8;
        size_t take = (key_len - start >= 8) ? 8 : (key_len - start);
        // всегда печатаем РОВНО 16 hex-символов группы; недостающие байты — «00» справа
        for (size_t i = 0; i < 8; i++)
        {
            uint8_t b = (i < take) ? kb[start + i] : 0;
            if (off + 2 >= out_cap)
            {
                out[off] = 0;
                return;
            }
            off += (size_t)snprintf(out + off, out_cap - off, "%02X", b);
        }
        if (g + 1 < groups)
        {
            if (off + 1 >= out_cap)
            {
                out[off] = 0;
                return;
            }
            out[off++] = ' ';
        }
    }
    out[(off < out_cap) ? off : out_cap - 1] = 0;
}

// обнуление ВСЕХ режимов, которые могут конфликтовать с TYT/AP/EP
static inline void kv_clear_all_crypt_flags(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    // «обычный» набор
    st->R = st->RR = st->K = st->H = 0;
    st->K1 = st->K2 = st->K3 = st->K4 = 0;
    memset(st->aes_key, 0, sizeof(st->aes_key));
    memset(st->A1, 0, sizeof(st->A1));
    memset(st->A2, 0, sizeof(st->A2));
    memset(st->A3, 0, sizeof(st->A3));
    memset(st->A4, 0, sizeof(st->A4));
    st->aes_key_loaded[0] = 0;
    st->aes_key_loaded[1] = 0;

    // Погасить все возможные спец-флаги (чтобы остался только нужный)
    st->tyt_ap = 0;
    st->tyt_ep = 0;
    st->tyt_bp = 0;
    st->any_bp = 0;
    st->ken_sc = 0;
    st->straight_ks = 0;
    st->straight_mod = 0;

    // не трогаем ctx из pc4/tyt — им управляют сами tyt_*_keystream_creation()
}
kv_alg_filter_t kv_algid_to_filter(uint8_t alg_id, uint8_t key_len)
{
    // Нормализуем популярные ID, но помним: для dsd-fme AES128/192/256 почти одинаковы
    switch (alg_id)
    {
    case 0x00:
        return KV_ALG_AUTO;
    case 0x01:
    case 0x10:
    case 0x21:
        return KV_ALG_ARC4;
    case 0x22:
        return KV_ALG_DES;
    case 0x23:
        return KV_ALG_AES192;
    case 0x24:
    case 0x11:
        return KV_ALG_AES128;
    case 0x25:
    case 0x02:
    case 0x05:
    case 0x07:
    case 0x12:
        return KV_ALG_AES256;
    case 0x90:
        return KV_ALG_TYT_AP; // Hytera HAS → считаем как AP/AES
    case 0xA0:
        return KV_ALG_TYT_EP; // Tait/закрытое — условно
    case 0xFC:                // наш sentinel для TYT_AUTO из CSV
        return KV_ALG_TYT_BP;
    case 0xFD:                // наш sentinel для TYT_AUTO из CSV
        return KV_ALG_TYT_AP;
    case 0xFE:                // наш sentinel для TYT_AUTO из CSV
        return (key_len <= 16) ? KV_ALG_TYT_EP : KV_ALG_TYT_AP;
    default:
        // если неизвестный, попробуем по длине ключа
        if (key_len == 16)
            return KV_ALG_AES128;
        if (key_len == 24)
            return KV_ALG_AES192;
        if (key_len == 32)
            return KV_ALG_AES256;
        return KV_ALG_AUTO;
    }
}

void kv_apply_key_for_name_bytes(dsd_opts *opts, dsd_state *st,
                                  uint8_t alg_id, const uint8_t *key_bytes, uint8_t key_len,
                                  uint8_t key_id)
{
    if (!st || !key_bytes)
        return;

    // 0) Чистим предыдущее состояние ключей
    st->R = st->RR = st->K = st->H = 0;
    st->K1 = st->K2 = st->K3 = st->K4 = 0;
    memset(st->aes_key, 0, sizeof(st->aes_key));
    memset(st->A1, 0, sizeof(st->A1));
    memset(st->A2, 0, sizeof(st->A2));
    memset(st->A3, 0, sizeof(st->A3));
    memset(st->A4, 0, sizeof(st->A4));
    memset(st->aes_key_loaded, 0, sizeof(st->aes_key_loaded));

    // 1) Получаем фактические БАЙТЫ ключа
    uint8_t kb[64] = {0};
    size_t kb_len = 0;
    if (key_len == 0 || key_len > 32)
    {
        // HEX-строка
        const char *hex = (const char *)key_bytes;
        size_t nibbles = kv_count_hex_nibbles_cstr(hex);
        size_t raw_bytes = (nibbles + 1) / 2;
        uint8_t norm = kv_norm_len_for_alg(alg_id, raw_bytes);
        kb_len = kv_hexstr_to_bytes(hex, kb, norm);
        if (kb_len < norm)
            memset(kb + kb_len, 0, norm - kb_len);
        key_len = norm;
    }
    else
    {
        kb_len = key_len;
        if (kb_len > sizeof(kb))
            kb_len = sizeof(kb);
        memcpy(kb, key_bytes, kb_len);
    }
    int tyt_auto = 0;
    if (alg_id == 0xFE)
        tyt_auto = 1; // помечен в CSV как TYT_AUTO
    if ((alg_id == 0x00) && (key_id == 0))
        tyt_auto = 1; // вообще нет ALG/KID

    kv_alg_filter_t flt;
    if (tyt_auto)
    {
        // выбираем EP/AP по фактической длине
        if (key_len <= 16)
            flt = KV_ALG_TYT_EP; // 128-bit AES
        else if (key_len <= 24)
            flt = KV_ALG_AES192; // редкий случай — уводим в AES
        else if (key_len <= 32)
            flt = KV_ALG_TYT_AP; // PC4 256
        else
            flt = KV_ALG_TYT_AP; // лишнее отрежется ниже до 32
    }
    else
    {
        flt = kv_algid_to_filter(alg_id, key_len);
    }

    // 2) «Как -H»: форсим ключ — не привязываем к идентификаторам шифрования из эфира
    //    (иначе при несовпадении KID/ALG поток может игнорировать загруженный ключ)
    /*
    if (opts)
    {
        opts->dmr_mute_encL = 0;
        opts->dmr_mute_encR = 0;
        opts->unmute_encrypted_p25 = 0;
    }
    st->keyloader = 0;
    */
    // 3) Классифицируем (УЖЕ зная реальную длину)
    // flt = kv_algid_to_filter(alg_id, key_len);

    switch (flt)
    {
    // -------------------- ARC4 / Basic Privacy --------------------
    case KV_ALG_ARC4:
    case KV_ALG_BP: // Hytera BP (10/32/64 chars) трактуем через RC4-ветку
    {
        // Для совместимости кладём первые до 8 байт в 64-бит R/RR
        size_t use = key_len > 8 ? 8 : key_len;
        unsigned long long v = 0;
        for (size_t i = 0; i < use; i++)
            v = (v << 8) | kb[i];
        st->R = st->RR = v;

        // Отметим идентификаторы, но форсим KID=0 (см. ниже)
        st->payload_algid = alg_id;
        st->payload_algidR = alg_id;
        break;
    }

    // -------------------- DES-64 --------------------
    case KV_ALG_DES:
    {
        unsigned long long v = 0;
        for (size_t i = 0; i < 8 && i < kb_len; i++)
            v = (v << 8) | kb[i];
        st->R = st->RR = v;
        st->payload_algid = alg_id;
        st->payload_algidR = alg_id;
        break;
    }

    // -------------------- Tytera Basic Privacy (16-бит) --------------------
    case KV_ALG_TYT_BP:
    {
        // Первые 2 байта → H, выставить флаг tyt_bp
        uint16_t h = (uint16_t)((kb[0] << 8) | kb[1]);
        st->H = (uint32_t)(h & 0xFFFF);
        st->tyt_bp = 1;
        st->payload_algid = alg_id;
        st->payload_algidR = alg_id;
        break;
    }

    // -------------------- Tytera AP (PC4) / Tytera EP (AES-128) / Retevis AP (RC2) --------------------
    case KV_ALG_TYT_AP:  // PC4 (-!)
    case KV_ALG_TYT_EP:  // AES-128 (-5)
    case KV_ALG_RETEVIS: // RC2 (-@)
    {
        char ks[96] = {0};
        kv_bytes_to_keystring_groups8(kb, key_len, ks, sizeof(ks));
        st->payload_algid = 0x00;
        st->payload_algidR = 0x00;
        st->payload_keyid = 0;
        st->payload_keyidR = 0;

        st->keyloader = 0;
        if (opts)
        {
            opts->dmr_mute_encL = 0;
            opts->dmr_mute_encR = 0;
            opts->unmute_encrypted_p25 = 0;
        }

        if (flt == KV_ALG_TYT_AP)
        {
            tyt_ap_pc4_keystream_creation(st, ks);
            fprintf(stderr, "[KV-APPLY] TYT_AP(PC4) len=%u ks=[%s]\n", key_len, ks);
        }
        else if (flt == KV_ALG_TYT_EP)
        {
            tyt_ep_aes_keystream_creation(st, ks);
            fprintf(stderr, "[KV-APPLY] TYT_EP(AES-128) len=%u ks=[%s]\n", key_len, ks);
        }
        else
        {
            retevis_rc2_keystream_creation(st, ks);
            fprintf(stderr, "[KV-APPLY] RETEVIS_RC2 len=%u ks=[%s]\n", key_len, ks);
        }

        // форсим применение без привязки к ALG/KID из эфира

        break;
    }

    // -------------------- AES (128/192/256) + VEDA/прочее по умолчанию --------------------
    case KV_ALG_AES128:
    case KV_ALG_AES192:
    case KV_ALG_AES256:
    case KV_ALG_AES:
    case KV_ALG_VEDA:
    default:
    {
        uint64_t A1 = 0, A2 = 0, A3 = 0, A4 = 0;

        if (key_len == 16)
        {
            A1 = kv_pack64_be(&key_bytes[0]);
            A2 = kv_pack64_be(&key_bytes[8]);
        }
        else
        { // 32
            A1 = kv_pack64_be(&key_bytes[0]);
            A2 = kv_pack64_be(&key_bytes[8]);
            A3 = kv_pack64_be(&key_bytes[16]);
            A4 = kv_pack64_be(&key_bytes[24]);
        }

        // Пишем как при -H: и в [0], и в [1]
        st->A1[0] = st->A1[1] = A1;
        st->A2[0] = st->A2[1] = A2;
        st->A3[0] = st->A3[1] = A3;
        st->A4[0] = st->A4[1] = A4;

        // Алгоритм + KID (если есть)
        st->payload_algid = (uint8_t)(alg_id & 0xFF);
        st->payload_keyid = (key_id > 0 ? key_id : 0);

        // Сигнализируем, что ключ загружен (как -H).
        st->aes_key_loaded[0] = 1;
        st->aes_key_loaded[1] = 1;

        // Снять мьют, чтобы слышать декодированный голос (как -H).
        opts->dmr_mute_encL = 0;
        opts->dmr_mute_encR = 0;

        // Отключить keyloader (в -H так и сделано)
        st->keyloader = 0;

        break;
    }
    }

    // 4) Форс-режим: не привязываем к Key ID из эфира — применяем ключ «как -H»
    //    (Это устраняет проблему «ключ загружен, но не применяется из-за KID mismatch»)
    st->payload_keyid = 0;
    st->payload_keyidR = 0;

    // 5) Завершающее сообщение
    fprintf(stderr, "[KV-RUNTIME] runtime key applied (forced): alg=0x%02X kid=0 len=%u\n",
            st->payload_algid, key_len);
}
