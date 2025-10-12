#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // size_t
#include <sys/time.h>
#include "dsd.h" // dsd_state, mbe_parms если нужно где-то ещё

#pragma once
#ifndef AVR_KV_H
#define AVR_KV_H

#include "dsd.h"
#include "mbelib.h"

#define DEBUG 2

// -------------------- Defaults (sane fallbacks) --------------------
#ifndef AVR_KV_NUM_BANDS
// Кол-во полос/параметров в MBE-профиле для расчёта "гладкости".
// Если у тебя уже определено где-то ещё — этот #ifndef не сработает.
#define AVR_KV_NUM_BANDS 56
#endif

// Порог для неисправимых ошибок (errs2), которые делают фрейм невалидным
#ifndef KV_UNCORR_ERRS2_THR
#define KV_UNCORR_ERRS2_THR 2
#endif

#ifndef AVR_KV_MAX_KEY_IDS
// Макс. кол-во KID, которые отслеживаем на слот
#define AVR_KV_MAX_KEY_IDS 256
#endif

// Длина окон (в суперкадрах/кадрах):
#ifndef AVR_SCOUT_MIN_SF
#define AVR_SCOUT_MIN_SF 2 // минимум 2 SF подряд
#endif
#ifndef AVR_SCOUT_MAX_SF
#define AVR_SCOUT_MAX_SF 6 // максимум 6 SF подряд
#endif
/*#ifndef AVR_SCOUT_MIN_SF
#define AVR_SCOUT_MIN_SF AVR_SCOUT_MIN_SF*18 // максимум 8 SF подряд
#endif
#ifndef AVR_SCOUT_MAX_SF
#define AVR_SCOUT_MAX_SF AVR_SCOUT_MAX_SF*18 // максимум 8 SF подряд
#endif
*/

// Ограничители:
#ifndef AVR_SCOUT_MAX_WINS_PER_GROUP
#define AVR_SCOUT_MAX_WINS_PER_GROUP 16 // максимум окон на одну группу
#endif
#ifndef AVR_SCOUT_MAX_GROUPS
#define AVR_SCOUT_MAX_GROUPS 1024 // максимум групп одновременно
#endif

//----------------------------------------------------
// Глобальное состояние Scout на процесс (безопасно для одного потока).
// Максимальный размер истории суперкадров по слоту (для сборки окон по индексам)
// #ifndef AVR_SCOUT_SF_HISTORY
// #define AVR_SCOUT_SF_HISTORY 64 // хватит для окон 2..8 SF с запасом
// #endif
#define AVR_SCOUT_VC_PER_SF 6

typedef struct
{
  // Позиция в потоке
  uint32_t sf_idx; // индекс суперкадра

  // Контекст для smooth/MBE (нужен KV и евристикам)
  mbe_parms prev_mp;

  // Полезная нагрузка для перебора (приоритет №1)
  uint8_t data27[AVR_SCOUT_VC_PER_SF][27]; // 3×AMBE72, упаковано
  uint8_t iv16[AVR_SCOUT_VC_PER_SF][16];   // IV на каждый VC (если применимо)

  // Краткие метки наличия/валидности
  uint8_t have27_mask; // битовая маска доступных 27-байтовых блоков (VC0..)
  uint8_t iv_valid;    // IV валиден (агрегатно для SF)

  // Качество/решения (минимум хранится, остальное — считается снаружи)
  uint8_t good;                  // алиас строгого критерия (равен good_kv)
  uint8_t key_applied_confirmed; // зафиксирован факт применения ключа на этом SF

  // Два уровня «годности» (оставляем для чёткой семантики)
  uint8_t good_scan; // мягкий критерий (учёт в SCAN-статистике)
  uint8_t good_kv;   // строгий критерий (учёт в KV-окнах)
} scout_sf_entry_t;

typedef struct
{
  // Идентификаторы окна/группы
  uint8_t slot;
  uint8_t alg_id, key_id;
  uint32_t tg, src;

  // Позиция и длина окна
  uint32_t start_sf_idx;
  uint8_t len_sf; // 1..8

  // Сводный показатель (для сортировки/отчёта)
  uint8_t quality; // 0..100 агрегат

  // Привязка к внешнему носителю (если используется сохранение)
  uint32_t file_id;
  uint32_t file_off;
} avr_scout_window_ref_t;

typedef struct
{
  // Ключ группы (SRC хранится для отчётов, в ключ сравнения не входит)
  uint8_t slot;
  uint8_t alg_id, key_id;
  uint32_t tg, src;

  // Базовая статистика (как было — для совместимости)
  uint32_t sf_total, sf_good, sf_bad;

  // Доп. статистика (параллельные контуры)
  uint32_t sf_total_scan; // мягкая статистика SCAN
  uint32_t sf_good_scan;
  uint32_t sf_total_kv; // строгая статистика KV (по окнам/«годным» сериям)
  uint32_t sf_good_kv;

  // Окна, собранные в этой группе
  uint8_t nwins; // ≤16
  avr_scout_window_ref_t win[16];

  // Краткие метрики качества по группе (для отчётов/отбора)
  float smooth_median;
  float var_median;

  // Фиксация факта успешного применения ключа в группе
  uint8_t key_applied_confirmed;

  // Ошибки FLCO FEC по группе
  uint32_t flco_err_count;

  // Наблюдаемость и качество PI/LE (если будешь считать — просто инкрементируй где нужно)
  uint8_t le_seen;            // 0/1
  uint8_t pi_seen;            // 0/1
  double pi_crc_ok_ratio;     // 0..1
  uint32_t mi_unique;         // сколько разных MI за группу (если считаешь)
  uint32_t mi_mismatch_count; // сколько несовпадений MI

  // Smooth/Voice агрегаты по группе (если начнёшь считать — пойдут реальные значения)
  double smooth_avg;
  double smooth_std;
  double smooth_p95;
  uint32_t smooth_gate_hits;
  double voice_confidence; // 0..1

  // Короткие служебные строки статуса
  // char reason[32];
  // char notes[64];
} avr_scout_group_t;
//----------------------------------------------------

// Внутренний трекер «серии» хороших SF по слоту.
typedef struct
{
  uint8_t active;        // 1 — внутри серии, 0 — нет
  uint32_t start_sf_idx; // индекс SF, с которого началась серия
  uint8_t len_sf;        // текущая длина серии (растёт до AVR_SCOUT_MAX_SF)
  // «замороженные» признаки группы для этой серии:
  uint8_t slot;
  uint8_t alg_id;
  uint8_t key_id;
  uint32_t tg;
  uint32_t src;
  uint8_t window_committed; // 0/1
} scout_run_t;

// -------------------- Пакет параметров MBE для одного кадра --------------------
typedef struct avr_kv_mbe_params_s
{
  float w0;                       // основной тон (или Wo — см. mbelib)
  int L;                          // число гармоник
  float log2Ml[AVR_KV_NUM_BANDS]; // амплитуды по полосам (log2 масштабе)
  uint8_t Vl[AVR_KV_NUM_BANDS];   // voiced/unvoiced флаги по полосам
} avr_kv_mbe_params_t;

// -------------------- Контекст Key Validation --------------------
/*
typedef struct avr_kv_ctx_s
{
  // настройки (м.б. перенастроены по ходу)
  float alpha;       // коэффициент EMA
  float thr_smooth;  // порог "гладкости"
  int pos_threshold; // граница подтверждения
  int neg_threshold; // граница отказа

  // состояние на слот/KID
  int confidence[2][AVR_KV_MAX_KEY_IDS];           // счётчик уверенности
  float smoothEMA[2][AVR_KV_MAX_KEY_IDS];          // EMA(гладкость)
  uint8_t have_prev[2][AVR_KV_MAX_KEY_IDS];        // есть ли "предыдущий" валидный кадр
  uint8_t warm_count[2][AVR_KV_MAX_KEY_IDS];       // тёплый старт: сколько валидных VC уже было
  uint8_t bad_consec[2][AVR_KV_MAX_KEY_IDS];       // серия подряд "плохих" кадров
  avr_kv_mbe_params_t prev[2][AVR_KV_MAX_KEY_IDS]; // предыдущие MBE-параметры

  // защёлки слота (итог по слоту до следующего сброса)
  bool confirmed[2]; // слот подтверждён (больше не уменьшаем уверенность)
  bool rejected[2];  // слот отклонён (до границы/сброса)
};
*/
// CSV-строка (мастер-список ключей)
typedef struct
{
  int alg;         // KV_ALG_ARC4/AES128/AES192/AES256
  int kid;         // KID (или -1, если в CSV не задан)
  int key_len;     // 16/24/32 (AES) или N (ARC4)
  int csv_index;   // порядковый номер строки в CSV (для keyOK_<id>.txt)
  uint8_t key[36]; // максимум для AES-256
} kv_csv_row_t;

/*
typedef struct {
  int ngroups;
  avr_scout_group_t groups[AVR_SCOUT_MAX_GROUPS];
  // diagnostic/last series flags (or provide function instead)
  uint8_t last_series_len_valid;
  uint32_t last_series_len[2];
  uint8_t last_slot;
} avr_scout_export_t;
*/

typedef struct
{
  int ngroups;
  const avr_scout_group_t *groups; // именно указатель
  uint8_t last_slot;
  uint8_t last_series_len_valid;
  uint32_t last_series_len[2];
} avr_scout_export_t;

extern int g_kv_batch_smooth;

#ifdef __cplusplus
extern "C"
{
#endif

  // Построить полный путь "<dir>/<basename>" с учётом -jp (или просто basename, если -jp не задан)
  void kv_build_result_path(const dsd_opts *opts, const char *basename,
                            char *out, size_t outsz);

  // Загрузка и фильтрация с учётом -ja / -jk:
  //   если -ja задан — фильтруем по alg;
  //   если -ja не задан — фильтруем по длине ключа/доп. полю CSV (auto);
  //   если -jk задан — оставляем только совпадающий KID.
  // int kv_csv_load_and_filter(const dsd_opts *opts, kv_csv_row_t **out, int *out_count);
  // int kv_csv_load_and_filter(const char *csv_path, kv_key_t **out_vec, size_t *out_n);
  void kv_write_key_ok_file(const dsd_opts *opts, const dsd_state *state, int kid, int ord);

  // Нормализует строку HEX: убирает всё, кроме [0-9A-Fa-f], UPPERCASE.
  // Обрезает до чётной длины, затем до кратной 16 длины.
  // Если длина >=64 → берём 64; иначе если >=48 → 48; иначе если >=32 → 32; иначе 0 (ошибка).
  // Возвращает 0 при ошибке, иначе длину нормализованной строки (HEX-символов).
  int kv_hex_sanitize_trim_to_aes(const char *in_hex, char *norm_hex, size_t norm_sz,
                                  int *alg_id, int *key_len_bytes);

  // Добавляет пробелы каждые 16 HEX-символов (на вход — компактный HEX без пробелов).
  // Возвращает длину записанной строки (с пробелами) или 0 при ошибке.
  int kv_hex_group16(const char *norm_hex, char *spaced_hex, size_t out_sz);

  // Конвертирует HEX (с пробелами или без) в байты. Возвращает число байт или 0 при ошибке.
  int kv_hex_to_bytes(const char *hex_in, uint8_t *out, size_t out_cap);

  // avr_kv.h — НОВОЕ
  int kv_load_ini_overrides(dsd_opts *opts, dsd_state *s);   // парсит opts->kv_ini_path
  void kv_post_cli_ini_adjust(dsd_opts *opts, dsd_state *s); // автодействия после CLI+INI

  // Запуск перебора на старте передачи (по ALGID/KID из эфира), формирует батч <=10
  void kv_enum_try_begin(dsd_opts *opts, dsd_state *state);

  // Обработчик кадра: считает HIT/MISS по smooth-окну и рулит ротацией кандидатов
  void kv_enum_on_frame(dsd_opts *opts, dsd_state *state);

  // Граница суперкадра (vc==1): сброс локальных счётчиков и принятие решений по лимитам
  void kv_on_superframe_boundary(dsd_opts *opts, dsd_state *state);

  void kv_after_mbe(dsd_opts *opts, dsd_state *state);
  // Окончание входного файла/потока
  void kv_on_stream_end(dsd_opts *opts, dsd_state *state);

  bool getG_enum_active();
  void kv_apply_key_for_name_bytes(dsd_opts *opts, dsd_state *st, uint8_t alg_id, const uint8_t *key_bytes, uint8_t key_len, uint8_t key_id);
  void kv_on_voice_end(dsd_opts *opts, dsd_state *state);
  void kv_init(dsd_opts *opts, dsd_state *st);

  // KV batch init/free (грузим CSV один раз при старте, используем в avr_kv_batch_run)
  void kv_batch_init(dsd_opts *opts, dsd_state *st);
  void kv_batch_free(void);
  void kv_compute_s(mbe_parms *cur_mp, mbe_parms *prev_mp, float *out_s);
  int kv_after_mbe_core_batch(dsd_opts *opts, dsd_state *state);

  void kv_batch_finalize_decision_for_pair(dsd_opts *opts, dsd_state *st,
                                           int slot, uint8_t kid);
  void kv_reset_pair_local(int slot, uint8_t kid);
  void avr_kv_batch_begin(dsd_opts *opts, dsd_state *st);
  void kv_hist_stats(int slot, uint8_t kid, int *T, int *G, int *B);

  // Scout API
  void avr_scout_reset(void);
  void avr_scout_on_superframe(dsd_opts *opts, dsd_state *state);
  bool avr_scout_is_series_ready(uint8_t slot);
  void avr_scout_flush(dsd_opts *opts, dsd_state *state, bool ms_mode);
  void avr_scout_finalize_series(dsd_opts *opts, dsd_state *state);
  void avr_scout_on_lc_update(dsd_state *st, uint8_t slot, uint32_t tg, uint32_t src);
  int avr_scout_get_27B_by_sf(uint8_t slot, uint32_t sf_idx, uint8_t out27[27]);
  int avr_scout_has_series_with_min_sf(uint8_t slot, uint8_t min_sf);
  void scout_series_finalize_and_store(dsd_opts *opts, dsd_state *st, uint8_t slot);
  int avr_scout_get_iv_by_sf(uint8_t slot, uint32_t sf_idx, uint8_t iv16[16]);
  int avr_scout_get_pre_window_mbe_parms_old(uint8_t slot, uint32_t sf_idx, mbe_parms *out_mp);
  int avr_scout_get_pre_window_mbe_parms(uint8_t slot, mbe_parms *out_mp);
  void avr_scout_on_vc(const dsd_state *st, const uint8_t enc27[27], const uint8_t iv16[16]);
  int scout_pack_27bytes_from_frames(char fr1[4][24],
                                     char fr2[4][24],
                                     char fr3[4][24],
                                     uint8_t out27[27]);
  int avr_scout_get_6x27B_and_6xIV_by_sf(uint8_t slot, uint32_t sf_idx,
                                         uint8_t out27[AVR_SCOUT_VC_PER_SF][27],
                                         uint8_t outiv[AVR_SCOUT_VC_PER_SF][16],
                                         uint8_t *mask);
  void avr_scout_write_json_summary_ext(const char *path, dsd_opts *opts, dsd_state *state);
  void scout_db_on_pi_or_lc(uint8_t slot, uint8_t alg, uint8_t kid, int has_tg, uint32_t tg);

  // Scout: учёт PI/LE/MI для статистики (не влияет на окна/перебор)
  void avr_scout_stat_pi_observe(uint8_t slot, int crc_ok, uint32_t mi);
  void avr_scout_stat_le_observe(uint8_t slot, int crc_ok);
  
  // === KV Batch (групповой перебор) ===
  // KV batch
  void avr_kv_batch_run(dsd_opts *opts, dsd_state *st);
  // Экспорт групп/окон для батча
  const avr_scout_export_t *avr_scout_export(void);
  size_t kv_fetch_window_payload(uint8_t slot, const avr_scout_window_ref_t *wref,
                                 uint8_t *dst, size_t dst_cap);

  void kv_batch_maybe_apply_single_key(dsd_opts *opts, dsd_state *st);

  // Загрузка «freq keys» из CSV (любой столбец может быть пустым, кроме enkey)
  int kv_load_freq_keys_from_csv(const char *path, bf_store_t *store);

  // уже декларация apply:
  int kv_apply_runtime_key_from_bytes(dsd_opts *opts, dsd_state *st,
                                      int alg_id, const uint8_t *key_bytes,
                                      size_t key_len, int kid);

  int kv_eval_window_smooth(dsd_opts *opts, dsd_state *st, int slot,
                            const uint8_t *dec27, size_t nsf,
                            float *avg_smooth, int *ok_frames, int *bad_errs2);
#ifdef __cplusplus
}
#endif

#endif // AVR_KV_H
