// -----------------------------------------------------------------------------
// avr_scout.c
// -----------------------------------------------------------------------------
// DMR Scout (Разведчик) для dsd-fme.
//
// Назначение:
//   * Один раз на суперкадр (после демодуляции и FEC) собрать метаданные,
//     выделить «хорошие» серии по errs2, сгруппировать по Slot/ALG_ID/KEY_ID/TG/SRC,
//     и сохранить окна (2..8 SF) для последующего группового перебора ключей.
//   * Никакого дешифрования/kv здесь нет: только подготовка окон и метаданных.
//   * Интеграция с Event_History: события SCOUT_WINDOW добавляются, не ломая
//     остальной пайплайн событий.
//   * В конце работы (avr_scout_flush) печатается сводный отчёт в консоль.
//
// Как использовать:
//   1) Вызвать avr_scout_on_superframe(opts, state) в dmr_ms.c/dmr_bs.c, когда
//      суперкадр уже собран (dmr_stereo_payload и EMB/LC готовы).
//   2) Вызвать avr_scout_flush(opts, state) в конце голоса/потока, чтобы закрыть
//      хвостовую серию и вывести сводный отчёт.
//   3) Дальше avr_kv_batch будет читать подготовленные окна (после реализации I/O).
//
// Важные TODO:
//   * Реализовать сбор 216 бит → 27 байт в scout_pack_27bytes_from_superframe().
//   * Реализовать запись окон на диск в scout_store_window_payload().
// -----------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>

#include "dsd.h"    // dsd_state, dsd_opts, Event_History
#include "avr_kv.h" // общие типы для Scout/KV (win/group/прототипы)

#define SCOUT_CAN_DEC_DBG true

static const char *SC_VERSION = "dsd-fme_new (v3.3 05.11.25)";
// ============================== КОНФИГУРАЦИЯ ===============================
#ifndef AVR_SCOUT_VERBOSE
#define AVR_SCOUT_VERBOSE DEBUG // подробный лог по ходу работы
#endif

// Все опции можно менять через -D в CMake или здесь.

// Включаемые/отключаемые блоки функционала Scout:
#ifndef AVR_SCOUT_CLASSIFY_CRYPT
#define AVR_SCOUT_CLASSIFY_CRYPT 1 // собирать enc/enc_alg/enc_key
#endif
#ifndef AVR_SCOUT_EXTRACT_TG_SRC
#define AVR_SCOUT_EXTRACT_TG_SRC 1 // извлекать TG/SRC (после EMB/LC FEC)
#endif
#ifndef AVR_SCOUT_GROUP_STATS
#define AVR_SCOUT_GROUP_STATS 1 // вести агрегированную статистику групп
#endif
#ifndef AVR_SCOUT_FIND_GOOD_REGIONS
#define AVR_SCOUT_FIND_GOOD_REGIONS 1 // искать «хорошие» серии по errs2
#endif
#ifndef AVR_SCOUT_CAPTURE_WINDOWS
#define AVR_SCOUT_CAPTURE_WINDOWS 1 // сохранять окна (в памяти + опц. на диск)
#endif
#ifndef AVR_SCOUT_DUMP_FILES
#define AVR_SCOUT_DUMP_FILES 0 // писать окна на диск (реализовать I/O)
#endif
#ifndef AVR_SCOUT_DECODE_CLEAR
#define AVR_SCOUT_DECODE_CLEAR 0 // декодировать только открытые участки (ALG==0)
#endif
// Порог «хорошего» SF по errs2:
#ifndef AVR_SCOUT_ERRS2_GOOD_MAX
#define AVR_SCOUT_ERRS2_GOOD_MAX 2
#endif

// Временная опция: считать любой «зашифрованный VC*» хорошим SF
#ifndef AVR_SCOUT_ACCEPT_ALL_ENC_VC
#define AVR_SCOUT_ACCEPT_ALL_ENC_VC 0
#endif

// Создавать окно на flush, даже если серия не была закрыта по качеству
#ifndef AVR_SCOUT_FORCE_WINDOW_ON_FLUSH
#define AVR_SCOUT_FORCE_WINDOW_ON_FLUSH 1
#endif

#ifndef AVR_SCOUT_VALIDATE_LC_IN_UPDATE
#define AVR_SCOUT_VALIDATE_LC_IN_UPDATE 1
#endif

// Управление логом:
#if AVR_SCOUT_VERBOSE
#define SLOG(fmt, ...) fprintf(stderr, "[SCOUT] " fmt, ##__VA_ARGS__)
#else
#define SLOG(fmt, ...) \
    do                 \
    {                  \
    } while (0)
#endif

static struct
{
    scout_run_t run[2]; // серия по слоту
    int ngroups;        // число групп
    avr_scout_group_t groups[AVR_SCOUT_MAX_GROUPS];
    uint32_t sf_idx[2]; // локальные счётчики SF по слотам
    // Кольцевая история 27-байтовых полезных данных по слотам:
    scout_sf_entry_t hist[2][AVR_SCOUT_MAX_SF];
    uint32_t hist_wr[2]; // write-указатель per-slot
    uint32_t last_tg[2];
    uint32_t last_src[2];
    uint8_t last_slot;
    mbe_parms cached_prev_mp[2];
    bool ms_mode;
    // --- дебаунс/эффективные признаки (step 3.A) ---
    uint8_t eff_alg[2]; // текущие «эффективные» alg (стабилизованные)
    uint8_t eff_kid[2]; // текущие «эффективные» kid
    uint32_t eff_tg[2]; // текущие «эффективные» tg

    // счётчики повторов для дебаунса (при одинаковом raw значение ++, иначе =1)
    uint8_t alg_run_cnt[2];
    uint8_t kid_run_cnt[2];
    uint8_t tg_run_cnt[2];
    uint32_t sf_total[2];
    uint32_t sf_good[2];

    uint8_t pi_le_seen[2];  // флаг PI/LE для текущего SF
    uint8_t alg_eff[2];     // эффективный ALG (последний известный)
    uint8_t kid_eff[2];     // эффективный KID (последний известный)
    uint32_t cur_sf_idx[2]; // последний sf_idx по слоту, зафиксированный в on_superframe
    uint32_t cur_wr[2];     // соответствующий индекс в кольце истории
} SC = {0};

static mbe_parms SC_prev_mp[2]; // по слоту

// --- Debounce (per-slot) ALG/KEY/TG, N=2 подряд наблюдения ---
static uint8_t db_raw_alg[2], db_eff_alg[2], db_stab_alg[2], db_eff_alg_valid[2];
static uint8_t db_raw_kid[2], db_eff_kid[2], db_stab_kid[2], db_eff_kid_valid[2];
static uint32_t db_raw_tg[2], db_eff_tg[2];
static uint8_t db_stab_tg[2], db_eff_tg_valid[2];

#ifndef AVR_SCOUT_DEBOUNCE_N
#define AVR_SCOUT_DEBOUNCE_N 2 /* N одинаковых наблюдений (2..3 обычно) */
#endif

#ifndef AVR_SCOUT_DEBOUNCE_TIMEOUT_SF
#define AVR_SCOUT_DEBOUNCE_TIMEOUT_SF 8 /* если за T SF не стабилизовалось — не менять eff */
#endif

// Описание одного SF для анализа окна «6 подряд с errs2 <= 2»
typedef struct scout_sf_stat_s
{
    uint32_t errs2; // число FEC-ошибок во всём SF
    // при желании добавьте сюда smooth, frames и т.д.
} scout_sf_stat_t;

typedef struct scout_group_report_s
{
    uint8_t slot;
    uint8_t alg_id; // 0x00 - открытый, иначе зашифровано
    uint8_t key_id; // если есть
    uint32_t tg;
    uint32_t src;

    // агрегаты по разговору
    uint32_t sf_total;
    uint32_t sf_good;
    uint64_t fec_err_total;

    uint32_t duration_ms; // можно заменить на фактическое по таймстампам
    uint32_t duration_sf; // = sf_total

    // признаки
    int decrypt;     // 0 - open; 1 - decrypted with enkey; 2 - encrypted (needs key)
    int can_decrypt; // 1 - есть смысл подбирать ключ (6 подряд SF с errs2 <= 2), иначе 0
} scout_group_report_t;

// ===================== ВНУТРЕННЕЕ СОСТОЯНИЕ SCOUT =====================

// Возвращает указатель на статический снапшот (без копий, только чтение)
const avr_scout_export_t *avr_scout_export(void)
{
    static avr_scout_export_t E = {0};
    int n = SC.ngroups;
    if (n < 0 || n > AVR_SCOUT_MAX_GROUPS)
        n = 0;
    E.ngroups = n;
    E.groups = SC.groups; // теперь это корректно (указатель)
    E.last_slot = SC.last_slot;
    E.last_series_len[0] = SC.run[0].len_sf;
    E.last_series_len[1] = SC.run[1].len_sf;
    E.last_series_len_valid = (SC.run[0].active || SC.run[1].active) ? 1 : 0;
    return &E;
}
// ===================== ВСПОМОГАТЕЛЬНЫЕ ПРОТОТИПЫ =====================
// Кэш для MI, который мы перехватим из dsd_main.c
static uint32_t g_scout_cached_mi[2] = {0, 0};

// Функция для сохранения MI в кэш
void avr_scout_cache_mi(int slot, unsigned long long int mi)
{
    if (slot < 0 || slot > 1)
        return;
    if (mi != 0)
    {
        g_scout_cached_mi[slot] = mi;
    }
}
// --- Доступ к полям стейта (слотовые accessor’ы) ---
static inline uint8_t scout_slot(const dsd_state *st);
static inline uint8_t scout_algid(const dsd_state *st, uint8_t slot);
static inline uint8_t scout_keyid(const dsd_state *st, uint8_t slot);
static inline uint32_t scout_tg(const dsd_state *st, uint8_t slot);
static inline uint32_t scout_src(const dsd_state *st, uint8_t slot);
static inline uint8_t scout_enc(const dsd_state *st, uint8_t slot);
static inline uint64_t scout_mi(const dsd_state *st, uint8_t slot);
static inline uint8_t scout_errs2_good(const dsd_state *st, uint8_t slot);

// --- Работа с данными суперкадра ---
static int scout_pack_27bytes_from_superframe(const dsd_state *st, uint8_t out27[27]);

// --- Управление сериями (начать/продлить/сбросить/закрыть и сохранить окно) ---
static void scout_series_start(uint8_t slot, uint8_t alg, uint8_t kid, uint32_t tg, uint32_t src, uint32_t sf_idx);
static void scout_series_extend(uint8_t slot);
static void scout_series_abort(uint8_t slot);

// --- Event_History интеграция и I/O окон ---
static void scout_record_event_history(dsd_state *st, uint8_t slot, const char *what, const avr_scout_window_ref_t *wref);
static void scout_store_window_payload(dsd_opts *opts, dsd_state *st, const avr_scout_window_ref_t *wref, const uint8_t payload27[][27]);

// --- Финальный отчёт (в консоль) ---
static void avr_scout_report_summary(bool ms_mode, dsd_state *stat);

// по заданному slot/sf_idx находим 27B в кольцевой истории; возвращаем 0 при успехе
int avr_scout_get_27B_by_sf(uint8_t slot, uint32_t sf_idx, uint8_t out27[27])
{
    if (slot > 1 || !out27)
        return -1;
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i]; // тот же буфер, куда пишем 27B
        if (e->sf_idx == sf_idx)
        {
            memcpy(out27, e->data27[0], 27);
            return 0;
        }
    }
    return -1;
}

// ===================== ДОСТУПЫ К СТЕЙТУ =====================

// Возвращает текущий обслуживаемый TDMA-слот (0 или 1).
static inline uint8_t scout_slot(const dsd_state *st)
{
    return (uint8_t)(st->currentslot & 1);
}

// Возвращает ALG_ID для этого слота (в dsd_state уже разнесено по slot/slotR).
static inline uint8_t scout_algid(const dsd_state *st, uint8_t slot)
{
    return slot ? st->payload_algidR : st->payload_algid;
}

// Возвращает KEY_ID (идентификатор ключа, не сам ключ!) для этого слота.
static inline uint8_t scout_keyid(const dsd_state *st, uint8_t slot)
{
    return slot ? st->payload_keyidR : st->payload_keyid;
}

// Возвращает TG (Group ID). Если у тебя есть per-slot lasttgR, подставь его здесь.
static inline uint32_t scout_tg(const dsd_state *st, uint8_t slot)
{
    uint32_t v = slot ? st->lasttgR : st->lasttg;
    return v ? v : SC.last_tg[slot];
}
static inline uint32_t scout_src(const dsd_state *st, uint8_t slot)
{
    uint32_t v = slot ? st->lastsrcR : st->lastsrc;
    return v ? v : SC.last_src[slot];
}

// 0 — открыто, 1 — шифр.
static inline uint8_t scout_enc(const dsd_state *st, uint8_t slot)
{
    return (scout_algid(st, slot) == 0x00) ? 0 : 1;
}

// Возвращает MI/IV базу из стейта по слоту (используется как метаданные).
static inline uint64_t scout_mi(const dsd_state *st, uint8_t slot)
{
    return slot ? st->payload_miR : st->payload_mi;
}

// Возвращает 1, если текущий SF «хороший», иначе 0.
static inline uint8_t scout_errs2_good(const dsd_state *st, uint8_t slot)
{
    const uint8_t e2 = slot ? st->errs2R : st->errs2;
    return (e2 <= AVR_SCOUT_ERRS2_GOOD_MAX);
}

// Находит 27B по sf_idx в локальной истории слота; возвращает 0 если найдено.
static int scout_hist_get27(uint8_t slot, uint32_t sf_idx, uint8_t out27[27])
{
    // ищем линейно (история маленькая); можно ускорить по желанию
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == sf_idx)
        {
            memcpy(out27, e->data27, 27);
            return 0;
        }
    }
    return -1;
}

// ===================== РАБОТА С ДАННЫМИ СУПЕРКАДРА =====================
// Упаковывает 216 голосовых бит (108 дибитов) текущего суперкадра в 27 байт.
// Источник: state->dmr_stereo_payload[] как массив дибитов (значения 0..3, 2 бита).
// Порядок набивки: AMBE1(12..47) → AMBE2(48..65, 90..107) → AMBE3(108..143).
// Битовый порядок в байте: "старшие вперёд" (2 бита кладём в текущие старшие позиции).
static int scout_pack_27bytes_from_superframe(const dsd_state *st, uint8_t out27[27])
{
    if (!st || !out27)
        return -1;

    // Собираем список диапазонов дибитов (в порядке передачи)
    struct
    {
        int a, b;
    } ranges[] = {
        {12, 47},   // AMBE #1: 36 дибитов
        {48, 65},   // AMBE #2 (part 1): 18 дибитов
        {90, 107},  // AMBE #2 (part 2): 18 дибитов
        {108, 143}, // AMBE #3: 36 дибитов
    };

    // Обнуляем буфер на результат (27 байт)
    memset(out27, 0, 27);

    // Кладём последовательно 108 дибитов = 216 бит в 27 байт
    int out_byte = 0;
    int bitpos = 8; // свободные биты в текущем байте (начинаем с MSB)
    for (unsigned r = 0; r < (sizeof(ranges) / sizeof(ranges[0])); ++r)
    {
        for (int i = ranges[r].a; i <= ranges[r].b; ++i)
        {
            // каждый элемент dmr_stereo_payload — это ДИБИТ (значение 0..3)
            uint8_t dibit = st->dmr_stereo_payload[i] & 0x03;

            // нам нужно положить 2 бита в текущий out-байт, начиная со старших
            if (bitpos == 0)
            { // если байт закончился — перейти к следующему
                ++out_byte;
                if (out_byte >= 27)
                    goto done; // перестраховка
                out27[out_byte] = 0;
                bitpos = 8;
            }

            // место для 2-х бит — кладём их в позиции (bitpos-2 .. bitpos-1)
            bitpos -= 2;
            out27[out_byte] |= (uint8_t)(dibit << bitpos);

            // Если байт ровно заполнился — на следующем шаге откроется новый.
            if (bitpos == 0)
            {
                ++out_byte;
                if (out_byte >= 27)
                    goto done;
                out27[out_byte] = 0;
                bitpos = 8;
            }
        }
    }

done:
    // out27 заполнен слева-направо крупными кускaми по 2 бита.
    // Если в твоём тестовом AES-коде порядок другой — здесь легко инвертировать.
    return 0;
}

// ===================== ГРУППЫ (по ключу Slot/ALG/KEY/TG/SRC) =====================

// Ищет существующую группу или создаёт новую.
// Возвращает индекс группы (0..ngroups-1) или -1 при переполнении.
static int scout_find_or_make_group_key(uint8_t slot, uint8_t alg, uint8_t kid, uint32_t tg)
{

    for (int i = 0; i < SC.ngroups; ++i)
    {
        avr_scout_group_t *G = &SC.groups[i];
        SLOG("%d. scout_find_or_make_group_key      alg {%d}, kid {%d}, tg {%d}, slot {%d}  !!!\n", i, alg, kid, tg, slot);
        SLOG("%d. scout_find_or_make_group_key G->  alg {%d}, kid {%d}, tg {%d}, slot {%d}, SC.sf_total {%d}  !!!\n\n", i, G->alg_id, G->key_id, G->tg, G->slot, SC.sf_total[slot]);
        if (G->slot == slot)
        {
            if (G->alg_id == alg && G->key_id == kid && G->tg == tg)
            {
                return i;
            }
            if (SC.sf_total[slot] < 3) // для влзможного сбоя
            {
                if (G->tg == tg)
                {
                    if ((G->alg_id == 0 && alg > 0) && (G->key_id == 0 && kid > 0))
                    {
                        SLOG("UPDATE scout_find_or_make_group_key      alg {%d}, kid {%d}, tg {%d}, slot {%d}  !!!\n", alg, kid, tg, slot);
                        SLOG("UPDATE scout_find_or_make_group_key G->  alg {%d}, kid {%d}, tg {%d}, slot {%d}  !!!\n\n", G->alg_id, G->key_id, G->tg, G->slot);
                        G->alg_id = alg;
                        G->key_id = kid;
                        return i;
                    }
                }
            }
        }
    }
    if (SC.ngroups >= AVR_SCOUT_MAX_GROUPS)
        return -1;

    // SLOG("\n\nscout_find_or_make_group_key  SC.ngroups {%d}. alg {%d}, kid {%d}, tg {%d}, slot {%d}  !!!\n", SC.ngroups, alg, kid, tg, slot);

    avr_scout_group_t *G = &SC.groups[SC.ngroups];
    memset(G, 0, sizeof(*G));
    G->slot = slot;
    G->alg_id = alg;
    G->key_id = kid;
    G->tg = tg;
    // src можно не фиксировать как часть ключа, но можно сохранить последнее встреченное:
    G->src = SC.run[slot].src;
    G->flco_err_count = 0;
    G->sf_total = 0;
    G->sf_total_scan = 0;
    G->sf_good = 0;
    G->sf_bad = 0;
    G->Priority = 0;
    G->Synctype = 0;
    G->irr_err = 0;
    return SC.ngroups++;
}

// ===================== УПРАВЛЕНИЕ СЕРИЯМИ ХОРОШИХ SF =====================

// Начинает новую серию хороших SF с заданными признаками группы.
static void scout_series_start(uint8_t slot, uint8_t alg, uint8_t kid, uint32_t tg, uint32_t src, uint32_t sf_idx)
{
    scout_run_t *r = &SC.run[slot];
    r->active = 1;
    r->slot = slot;
    r->alg_id = alg;
    r->key_id = kid;
    r->tg = tg;
    r->src = src;
    r->start_sf_idx = sf_idx;
    r->len_sf = 1;
    r->window_committed = 0;
    SLOG("series start: slot=%u alg=0x%02X kid=%u tg=%u src=%u sf=%u\n",
         slot, alg, kid, tg, src, sf_idx);
}

// Продлевает текущую серию (если ещё не достигнут максимум).
static void scout_series_extend(uint8_t slot)
{
    scout_run_t *r = &SC.run[slot];
    if (r->active && r->len_sf < AVR_SCOUT_MAX_SF)
    {
        r->len_sf++;
        SLOG("!!!! r->active %u. r->len_sf %u < AVR_SCOUT_MAX_SF\n", r->active, r->len_sf);
    }
}

// Сбрасывает текущую серию (без сохранения окна).
static void scout_series_abort(uint8_t slot)
{
    scout_run_t *r = &SC.run[slot];
    memset(r, 0, sizeof(*r));
}

// Завершает серию: сохраняет окно в таблицу групп, пишет Event_History,
// и (опционально) дампит окно на диск. После этого серия сбрасывается.
// одно окно на одну группу ALG_ID/KEY_ID/TG
void scout_series_finalize_and_store(dsd_opts *opts, dsd_state *st, uint8_t slot)
{
    (void)opts;
    (void)st;
    SLOG("scout_series_finalize_and_store!!!\n\n");
#if AVR_SCOUT_CAPTURE_WINDOWS
    scout_run_t *r = &SC.run[slot];

    // если серия уже отдана окном — просто сбросить состояние
    if (r->window_committed)
    {
        r->active = 0;
        r->len_sf = 0;
        return;
    }

    // короткую серию не AVR_SCOUT_MIN_SF
    if (!r->active || r->len_sf < AVR_SCOUT_MIN_SF)
    {
        scout_series_abort(slot);
        return;
    }

    // нормируем длину в рамках [AVR_SCOUT_MIN_SF .. AVR_SCOUT_MAX_SF]
    uint8_t len = r->len_sf;
    if (len > AVR_SCOUT_MAX_SF)
        len = AVR_SCOUT_MAX_SF;

    const uint8_t alg = r->alg_id;
    const uint8_t kid = r->key_id;
    const uint32_t tg = r->tg;
    const uint32_t src = r->src; // для логов/диагностики (НЕ часть ключа группы)

    // найти/создать группу по КЛЮЧУ БЕЗ SRC: (Slot, ALG, KID, TG)
    int gi = scout_find_or_make_group_key(slot, alg, kid, tg);
    /// SLOG("111111 (gi < 0)!!!\n\n");
    if (gi < 0)
    {
        SLOG("scout_series_abort  if (gi < 0)!!!\n\n");
        scout_series_abort(slot);
        return;
    }
    avr_scout_group_t *G = &SC.groups[gi];

    // одно окно на группу — если уже есть, повторно не добавляем
    if (G->nwins >= 1)
    {
        // SLOG("scout_series_abort  G->nwins >= 1!!!\n\n");
        scout_series_abort(slot);
        return;
    }

    // сформировать «ссылочное» окно без payload27 (27B достаём по sf_idx из истории при тесте)
    avr_scout_window_ref_t wref;
    memset(&wref, 0, sizeof(wref));
    wref.slot = slot;
    wref.alg_id = alg;
    wref.key_id = kid;
    wref.tg = tg;
    wref.src = src; // только для отчётов/диагностики
    wref.start_sf_idx = r->start_sf_idx;
    wref.len_sf = len;
    wref.quality = 1; // опциональный агрегат качества серии

    // сохранить окно в группе (строго одно)
    if (G->nwins < AVR_SCOUT_MAX_WINS_PER_GROUP)
    {
        G->win[G->nwins++] = wref;
    }

#if AVR_SCOUT_GROUP_STATS
    // учёт статистики: считаем всю серию «good»
    G->sf_total += len;
    G->sf_good += len;
    G->sf_total_kv += len;
    G->sf_good_kv += len;
#endif

    // финальный сброс состояния серии
    r->window_committed = 1;
    r->active = 0;
    r->len_sf = 0;
    memcpy(&SC_prev_mp[slot & 1], st->prev_mp, sizeof(mbe_parms));
#else
    (void)slot;
#endif
}

// ===================== EVENT_HISTORY И I/O ОКОН =====================
// Добавляет запись в Event_History. Если есть централизованный API
// в dsd_events.c — можно заменить на его вызов в одном месте.
static void scout_record_event_history(dsd_state *st, uint8_t slot, const char *what, const avr_scout_window_ref_t *wref)
{
    if (!st || !st->event_history_s)
        return;

    // Простой кольцевой индекс per-slot (0..254):
    static uint8_t eh_idx[2] = {0, 0};
    uint8_t idx = eh_idx[slot]++;
    if (eh_idx[slot] >= 255)
        eh_idx[slot] = 0;

    Event_History *E = &st->event_history_s->Event_History_Items[idx];
    memset(E, 0, sizeof(*E));

    E->write = 1;
    E->color_pair = 0;
    E->systype = 2;                  // DMR (уточни код, если принят другой)
    E->subtype = 1;                  // «системное/Scout»
    E->sys_id1 = st->dmr_color_code; // CC в sys_id1
    E->gi = 0;                       // 0=group (если есть индикатор индивидуального вызова — подставь)

    const uint8_t alg = scout_algid(st, slot);
    const uint8_t kid = scout_keyid(st, slot);
    E->enc = (alg == 0x00) ? 0 : 1;
    E->enc_alg = alg;
    E->enc_key = kid;
    E->mi = scout_mi(st, slot);

    // E->source_id = wref ? wref->src : scout_src(st, slot);
    E->target_id = wref ? wref->tg : scout_tg(st, slot);

    snprintf(E->event_string, sizeof(E->event_string),
             " slot=%u alg=0x%02X kid=%u tg=%u src=%u start_sf=%u len=%u",
             (unsigned)slot, (unsigned)alg, (unsigned)kid,
             (unsigned)(wref ? wref->tg : 0),
             (unsigned)(wref ? wref->src : 0),
             (unsigned)(wref ? wref->start_sf_idx : 0),
             (unsigned)(wref ? wref->len_sf : 0));

    E->event_time = time(NULL);
    E->kid = kid;
    E->kv_smooth = 0;
    snprintf(E->internal_str, sizeof(E->internal_str), "%s", what ? what : "SCOUT");
}

// Простейший writer окна на диск (заглушка). Здесь можно сделать JSON+BIN,
// Простейшая запись окна на диск: BIN + JSON.
// Путь: ./scout/slotX_algYY_kidZZ_tgNNNN_srcMMMM_sfSSSS_lenLL.win
//       ./scout/slotX_algYY_kidZZ_tgNNNN_srcMMMM_sfSSSS_lenLL.json
// Можно заменить на любую схему (kv_build_result_path и др.) — каркас изолирован.
static void scout_store_window_payload(dsd_opts *opts, dsd_state *st,
                                       const avr_scout_window_ref_t *wref,
                                       const uint8_t payload27[][27])
{
    (void)opts;
    (void)st;
    if (!wref || !payload27)
        return;

    // 1) гарантируем наличие каталога ./scout
    const char *outdir = "./scout";
    struct stat stbuf;
    if (stat(outdir, &stbuf) != 0)
    {
        if (mkdir(outdir, 0755) != 0 && errno != EEXIST)
        {
            fprintf(stderr, " ERROR: mkdir(%s) failed: %s\n", outdir, strerror(errno));
            return;
        }
    }

    // 2) формируем базовое имя файла из метаданных окна
    char base[512];
    snprintf(base, sizeof(base),
             "%s/slot%u_alg%02X_kid%u_tg%u_src%u_sf%u_len%u",
             outdir,
             (unsigned)wref->slot,
             (unsigned)wref->alg_id,
             (unsigned)wref->key_id,
             (unsigned)wref->tg,
             (unsigned)wref->src,
             (unsigned)wref->start_sf_idx,
             (unsigned)wref->len_sf);

    // 3) BIN-файл с полезной нагрузкой (len_sf × 27 байт)
    char binpath[576];
    snprintf(binpath, sizeof(binpath), "%s.win", base);
    FILE *fb = fopen(binpath, "wb");
    if (!fb)
    {
        fprintf(stderr, " ERROR: fopen(%s) failed: %s\n", binpath, strerror(errno));
        return;
    }
    for (uint8_t i = 0; i < wref->len_sf; ++i)
    {
        fwrite(payload27[i], 1, 27, fb);
    }
    fclose(fb);

    // 4) JSON-метаданные окна (минимальный набор)
    char jsonpath[576];
    snprintf(jsonpath, sizeof(jsonpath), "%s.json", base);
    FILE *fj = fopen(jsonpath, "w");
    if (!fj)
    {
        fprintf(stderr, " ERROR: fopen(%s) failed: %s\n", jsonpath, strerror(errno));
        return;
    }
    fprintf(fj,
            "{\n"
            "  \"slot\": %u,\n"
            "  \"alg_id\": %u,\n"
            "  \"key_id\": %u,\n"
            "  \"tg\": %u,\n"
            "  \"src\": %u,\n"
            "  \"start_sf_idx\": %u,\n"
            "  \"len_sf\": %u,\n"
            "  \"quality\": %u,\n"
            "  \"bin\": \"%s\"\n"
            "}\n",
            (unsigned)wref->slot,
            (unsigned)wref->alg_id,
            (unsigned)wref->key_id,
            (unsigned)wref->tg,
            (unsigned)wref->src,
            (unsigned)wref->start_sf_idx,
            (unsigned)wref->len_sf,
            (unsigned)wref->quality,
            binpath);
    fclose(fj);

    SLOG("dumped window: %s (.win + .json)\n", base);
}

// ===================== ФИНАЛЬНЫЙ ОТЧЁТ (В КОНСОЛЬ) =====================
// Печатает сводку по всем группам и окнам, найденным Scout’ом.
static void avr_scout_report_summary(bool ms_mode, dsd_state *stat)
{
    unsigned total_groups = (unsigned)SC.ngroups;
    stat->total_sf[0] = 0;
    stat->total_good[0] = 0;
    stat->total_sf[1] = 0;
    stat->total_good[1] = 0;
    for (int i = 0; i < SC.ngroups; ++i)
    {
        const avr_scout_group_t *G = &SC.groups[i];
        stat->total_sf[G->slot] += G->sf_total;
        stat->total_good[G->slot] += G->sf_good;
    }
    SC.ms_mode = ms_mode;
    if (SC.ngroups > 1)
    {
        if (ms_mode)
        {
            fprintf(stderr, "\n MS SUMMARY: groups=%u, sf_good=%u / sf_total=%u\n",
                    total_groups, (stat->total_good[0] + stat->total_good[1]), (stat->total_sf[0] + stat->total_sf[1]));
        }
        else
        {
            fprintf(stderr, "\n BS SUMMARY: groups=%u, sf_good=%u / sf_total=%u\n",
                    total_groups, (stat->total_good[0] + stat->total_good[1]), (stat->total_sf[0] + stat->total_sf[1]));
        }
    }
    for (int i = 0; i < SC.ngroups; ++i)
    {
        const avr_scout_group_t *G = &SC.groups[i];
        if (ms_mode)
            fprintf(stderr, " MS group[%d] slot=%u alg=0x%02X kid=%u tg=%u src=%u. sf=%u/%u windows=%u\n",
                    i, G->slot, G->alg_id, G->key_id, G->tg, G->src, (unsigned)G->sf_good, (unsigned)G->sf_total, G->nwins);
        else
            fprintf(stderr, " BS group[%d] slot=%u alg=0x%02X kid=%u tg=%u src=%u. sf=%u/%u windows=%u\n",
                    i, G->slot, G->alg_id, G->key_id, G->tg, G->src, (unsigned)G->sf_good, (unsigned)G->sf_total, G->nwins);

        for (uint8_t w = 0; w < G->nwins; ++w)
        {
            const avr_scout_window_ref_t *W = &G->win[w];
            fprintf(stderr,
                    "         win[%u]: start_sf=%u len=%u q=%u\n",
                    (unsigned)w, (unsigned)W->start_sf_idx, (unsigned)W->len_sf, (unsigned)W->quality);
        }
    }
    // fprintf(stderr, " END OF SUMMARY\n\n");
}

// ===================== ПУБЛИЧНЫЕ ФУНКЦИИ SCOUT =====================
// Вызывается ОДИН РАЗ НА СУПЕРКАДР (после того, как MS/BS собрали payload+EMB).
#ifndef AVR_SCOUT_DIAG
#define AVR_SCOUT_DIAG 1
#endif

// Add function
int avr_scout_get_iv_by_sf(uint8_t slot, uint32_t sf_idx, uint8_t iv16[16])
{
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == sf_idx)
        {
            memcpy(iv16, e->iv16[0], 16);
            return 0;
        }
    }
    return -1;
}

int avr_scout_get_pre_window_mbe_parms(uint8_t slot, mbe_parms *out)
{
    if (!out)
        return -1;
    memcpy(out, &SC_prev_mp[slot & 1], sizeof(mbe_parms));
    return 0;
}

int avr_scout_get_pre_window_mbe_parms_old(uint8_t slot, uint32_t sf_idx, mbe_parms *out_mp)
{
    if (slot > 1 || !out_mp) // Убрана проверка sf_idx == 0, она больше не нужна
    {
        return -1;
    }

    // ИЗМЕНЕНИЕ: Ищем состояние, сохраненное ВМЕСТЕ с начальным SF.
    // Оно содержит prev_mp от предыдущего кадра.
    const uint32_t target_sf_idx = sf_idx;

    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == target_sf_idx)
        {
            memcpy(out_mp, &e->prev_mp, sizeof(mbe_parms));
            return 0; // Найдено!
        }
    }

    return -1; // Не найдено
}

static inline uint8_t scout_payload_algid_for_slot(const dsd_state *st, uint8_t slot)
{
    // slot: 0 или 1
    return (slot == 0) ? st->payload_algid : st->payload_algidR;
}

// Помечаем последнюю (текущую) группу слота, если условия выполнены
void scout_mark_key_applied_if_match(dsd_state *st, uint8_t slot)
{
    if (!st || slot > 1)
        return;
    if (!st->aes_key_loaded[slot])
        return;

    // NB: берём «текущий контекст» из SC: последний tg/src, последний добавленный группой slot
    const uint32_t tg = SC.last_tg[slot];

    // Пробегаем группы в обратном порядке и ищем «наиболее свежую» подходящую
    for (int i = SC.ngroups - 1; i >= 0; --i)
    {
        avr_scout_group_t *G = &SC.groups[i];
        if (G->slot != slot)
            continue;
        // if (G->alg_id != alg_cur)
        //    continue;
        const int key_loaded = (st->aes_key_loaded[slot] != 0);
        // Если key_id у вас фиксируется заранее — сравнивайте при желании.
        // Найдём по tg/src (чаще всего в группе это те же)
        if (G->tg == tg && key_loaded) //  && G->src == src
        {
            G->key_applied_confirmed = 1;
            break;
        }
    }
}

void avr_scout_on_alg_seen(uint8_t slot, uint8_t alg_id, uint8_t key_id)
{
    if (slot > 1)
        return;

    // обновляем "эффективные" поля даже при битом LC
    if (alg_id != 0x00)
        SC.alg_eff[slot] = alg_id;
    if (key_id != 0x00)
        SC.kid_eff[slot] = key_id;

    // помечаем, что для этого слота в текущем SF был PI/LE с ALG/KID
    SC.pi_le_seen[slot] = 1;

    // if (SC.verbose)
    fprintf(stderr, "[SCOUT] on_alg_seen: slot=%d alg=0x%02X kid=%d\n", slot, alg_id, key_id);
}

// Бекфилл текущего SF признаками из PI/LE (когда LC пришёл позже VC*)
void avr_scout_backfill_from_pi(uint8_t slot, uint8_t alg_id, uint8_t key_id, uint32_t tg, uint32_t src, int crc_ok)
{
    if (slot > 1)
        return;

    // Текущий SF по слоту
    const uint32_t sf_idx = SC.cur_sf_idx[slot];
    const uint32_t wr = SC.cur_wr[slot];
    if (sf_idx == 0 || wr >= AVR_SCOUT_MAX_SF)
        return;

    // Обновляем «последние» TG/SRC
    if (tg)
        SC.last_tg[slot] = tg;
    if (src)
        SC.last_src[slot] = src;

    // Подтягиваем «эффективные» ALG/KID из PI/LE
    if (alg_id)
        db_eff_alg[slot] = alg_id, db_eff_alg_valid[slot] = 1;
    if (key_id)
        db_eff_kid[slot] = key_id, db_eff_kid_valid[slot] = 1;
    if (tg)
        db_eff_tg[slot] = tg, db_eff_tg_valid[slot] = 1;

    // Отметим, что текущий SF можно считать пригодным для KV/scan, даже если LC битый
    SC.hist[slot][wr].good_kv = 1;
    if (crc_ok)
        SC.hist[slot][wr].good_scan = 1;
    SC.hist[slot][wr].good = SC.hist[slot][wr].good_kv;

    // Обновим текущий run, чтобы серия стартовала/продолжилась
    scout_run_t *r = &SC.run[slot];
    const uint8_t eff_alg = (alg_id ? alg_id : (r->active ? r->alg_id : 0));
    const uint8_t eff_kid = (key_id ? key_id : (r->active ? r->key_id : 0));
    const uint32_t eff_tg = (tg ? tg : SC.last_tg[slot]);
    const uint32_t eff_src = (src ? src : SC.last_src[slot]);

    // Если серия не активна — стартуем хотя бы по ALG/KID (TG может прийти позже)
    if (!r->active && (eff_alg || eff_tg))
    {
        scout_series_start(slot, eff_alg, eff_kid, eff_tg, eff_src, sf_idx);
        SLOG(" series START (slot=%u) len=1 [backfill PI/LE]\n", slot);
    }
    else if (r->active)
    {
        // Если активна — просто подправим поля серии (без принудительной финализации)
        if (eff_alg && r->alg_id == 0)
            r->alg_id = eff_alg;
        if (eff_kid && r->key_id == 0)
            r->key_id = eff_kid;
        if (eff_tg && r->tg == 0)
            r->tg = eff_tg;
        if (eff_src && r->src == 0)
            r->src = eff_src;
    }

    SLOG(" backfill PI/LE: slot=%u sf=%u alg=0x%02X kid=%u tg=%u src=%u crc_ok=%d\n",
         slot, (unsigned)sf_idx, eff_alg, eff_kid, eff_tg, eff_src, crc_ok);
}

static uint16_t good_streak[2] = {0, 0};

void avr_scout_on_superframe(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    if (!st)
        return;

    // --- 1) Слот и индекс SF ---
    const uint8_t slot = scout_slot(st);
    SC.last_slot = slot;
    const uint8_t alg_raw = scout_algid(st, slot);
    const uint8_t kid_raw = scout_keyid(st, slot);

    if (opts->kv_csv_path[0] != '\0')
    {
        if (alg_raw == 0 || kid_raw == 0)
        {
            SLOG(">>> BREAK slot=%u, alg_raw=%u, kid_raw=%u \n", slot, alg_raw, kid_raw);
            return;
        }
    }

    // Правильно: сначала зафиксировать новый индекс SF...
    uint32_t sf_idx = ++SC.sf_idx[slot];
    st->indx_SF = sf_idx;

    // ...а уже потом при желании обновить "эпоху" (без возврата из функции)
    static uint32_t last_vc18_epoch[2] = {0, 0};
    const uint32_t vc18_epoch = (slot ? (uint32_t)((st->DMRvcR << 16) ^ sf_idx) : (uint32_t)((st->DMRvcL << 16) ^ sf_idx));
    // просто запоминаем — НО НЕ ВОЗВРАЩАЕМСЯ
    last_vc18_epoch[slot] = vc18_epoch;

    const int bad_flco = (slot < 2 && st->flco_fec_err[slot]);
    SLOG("\n >>> ENTER sf=%u slot=%u\n", (unsigned)sf_idx, slot);

    // --- [SCOUT STATS] базовая статистика по SuperFrame ---
    avr_scout_group_t *G = NULL;
    if (SC.ngroups > 0)
    {
        // Найдём активную группу по текущему слоту (последнюю по времени)
        for (int gi = SC.ngroups - 1; gi >= 0; --gi)
        {
            if (SC.groups[gi].slot == slot)
            {
                G = &SC.groups[gi];
                break;
            }
        }
    }
    SC.sf_total[slot]++;
    // --- [SCOUT STATS END] ---

    // --- 2) Кольцевая история: пишем ТОЛЬКО метаданные (без 27B/IV) ---
    const uint32_t wr = SC.hist_wr[slot] % AVR_SCOUT_MAX_SF;
    // scout_sf_entry_t *current_hist_entry = &SC.hist[slot][wr];

    SC.cur_sf_idx[slot] = sf_idx;
    SC.cur_wr[slot] = wr;
    // быстрые флаги на новую запись SF
    SC.hist[slot][wr].good = 0;
    SC.hist[slot][wr].good_kv = 0;
    SC.hist[slot][wr].good_scan = 0;
    SC.hist[slot][wr].key_applied_confirmed = 0;

    // индекс SF и снимок MBE «до текущего кадра»
    SC.hist[slot][wr].sf_idx = sf_idx;

    // для первого SF зафиксируем детерминированный baseline в cached_prev_mp
    if (sf_idx == 1)
    {
        // если у тебя есть явная инициализация mbe — можешь заменить на mbe_initMbeParms(...)
        // memset(&SC.cached_prev_mp[slot], st->prev_mp, sizeof(mbe_parms));
        memset(&SC.cached_prev_mp[slot], 0, sizeof(mbe_parms));
    }
    memcpy(&SC.hist[slot][wr].prev_mp, &SC.cached_prev_mp[slot], sizeof(mbe_parms));
    memcpy(&SC.cached_prev_mp[slot], st->prev_mp, sizeof(mbe_parms));

    // строгий/мягкий флаги «годности»
    uint8_t is_good = scout_errs2_good(st, slot);
    SC.hist[slot][wr].good_kv = is_good;
    // SC.hist[slot][wr].good_scan = (bad_flco == 0);
    SC.hist[slot][wr].good_scan = (bad_flco == 0) || (db_eff_alg_valid[slot] && db_eff_kid_valid[slot]);
    SC.hist[slot][wr].good = SC.hist[slot][wr].good_kv;

    // --- 3) Диагностика заполнения кольца и статуса серии ---
    scout_run_t *r = &SC.run[slot];
    uint32_t fill = (SC.sf_idx[slot] < AVR_SCOUT_MAX_SF) ? SC.sf_idx[slot] : AVR_SCOUT_MAX_SF;

    SLOG("hist slot=%u fill=%u/%u | ready6=%u ready8=%u\n",
         slot, (unsigned)fill, (unsigned)AVR_SCOUT_MAX_SF,
         (unsigned)(fill >= 6), (unsigned)(fill >= 8));

    SLOG("series slot=%u len_sf=%u active=%u | good_ready6=%u good_ready8=%u\n",
         slot, (unsigned)r->len_sf, (unsigned)r->active,
         (unsigned)(r->len_sf >= 6), (unsigned)(r->len_sf >= 8));

    SLOG(" >>> ENTER sf=%u slot=%u pre.active=%u pre.len=%u\n",
         (unsigned)sf_idx, slot, r->active, r->len_sf);

    // --- 4) Сырые признаки из текущего SF ---
    const uint32_t tg_raw = scout_tg(st, slot);
    const uint32_t src_raw = scout_src(st, slot);
    const uint8_t can_use_eff = (db_eff_alg_valid[slot] && db_eff_kid_valid[slot]);

    if (opts->kv_csv_path[0] != '\0') // НЕ МЕНЯТЬ TG
    {
        if (SC.last_tg[slot] == 0)
            SC.last_tg[slot] = tg_raw;
        if (SC.last_src[slot] == 0)
            SC.last_src[slot] = src_raw;
    }
    else
    {
        if (tg_raw)
            SC.last_tg[slot] = tg_raw;
        if (src_raw)
            SC.last_src[slot] = src_raw;
    }

    if (is_good)
        good_streak[slot]++;
    else
        good_streak[slot] = 0;

    // --- 5) Эффективные признаки VC часто несут нули) ---

    uint8_t eff_alg = (alg_raw != 0) ? alg_raw : (r->active ? r->alg_id : alg_raw);
    uint8_t eff_kid = (kid_raw != 0) ? kid_raw : (r->active ? r->key_id : kid_raw);
    uint32_t eff_tg = (tg_raw != 0) ? tg_raw : SC.last_tg[slot];
    if (SC.last_tg[slot] == 0 && tg_raw == 0)
        eff_tg = 0;
    uint32_t eff_src = (src_raw != 0) ? src_raw : SC.last_src[slot];

    // Если алгоритм ARC4 или DES, сохраняем 27 байт и IV из MI прямо здесь.
    // Логика для AES (0x24, 0x25) остается в avr_scout_on_vc, которую dmr_voice вызывает 6 раз.

    if (eff_alg == 0x21 || eff_alg == 0x22)
    {
        // --- ШАГ 1: Запись 27 байт (216 бит) в SC.hist ---
        uint8_t sf27[27];
        if (scout_pack_27bytes_from_superframe(st, sf27) == 0)
        {
            // Пишем именно в индекс VC=0
            memcpy(SC.hist[slot][wr].data27[0], sf27, 27);
            // Устанавливаем флаг наличия данных для VC=0
            SC.hist[slot][wr].have27_mask |= (1 << 0);
        }

        // --- ШАГ 2: Заполнение IV в SC.hist по веткам ALG ---

        uint32_t mi32 = scout_mi(st, slot); // Получаем MI

        // Формируем iv16: первые 4 байта = MI32 (BE), остальные - нули
        SC.hist[slot][wr].iv16[0][0] = (uint8_t)(mi32 >> 24);
        SC.hist[slot][wr].iv16[0][1] = (uint8_t)(mi32 >> 16);
        SC.hist[slot][wr].iv16[0][2] = (uint8_t)(mi32 >> 8);
        SC.hist[slot][wr].iv16[0][3] = (uint8_t)(mi32);
        memset(&SC.hist[slot][wr].iv16[0][4], 0, 12); // Заполняем остаток нулями
    }

    /*
    if(!opts->kv_batch_enable) { // Осторожно - влияет на smoth
        eff_alg = db_eff_alg_valid[slot] ? db_eff_alg[slot] : eff_alg;
        eff_kid = db_eff_kid_valid[slot] ? db_eff_kid[slot] : eff_kid;
        eff_tg = db_eff_tg_valid[slot] ? db_eff_tg[slot] : eff_tg;
    }
    */
    // fallback от PI/LE, когда LC битый и raw==0: берём стабилизованные значения из scout_db_on_pi_or_lc()
    if (eff_alg == 0 && db_eff_alg_valid[slot])
        eff_alg = db_eff_alg[slot];
    if (eff_kid == 0 && db_eff_kid_valid[slot])
        eff_kid = db_eff_kid[slot];
#if AVR_SCOUT_FIND_GOOD_REGIONS
#if AVR_SCOUT_ACCEPT_ALL_ENC_VC
    if (!is_good)
    {
        // const uint8_t enc_ctx = (alg_raw != 0) || (r->active && r->alg_id != 0);
        const uint8_t enc_ctx = (alg_raw != 0) || (eff_alg != 0) || db_eff_alg_valid[slot] || (r->active && r->alg_id != 0);
        if (enc_ctx)
            is_good = 1;
        SC.hist[slot][wr].good_kv = is_good;
        SC.hist[slot][wr].good = is_good;
    }
#endif
#endif

    SLOG(" sf=%u/%u slot=%u alg(raw/eff)=0x%02X/0x%02X kid(raw/eff)=%u/%u "
         "tg(raw/eff)=%u/%u src(raw/eff)=%u/%u good=%u\n",
         (unsigned)sf_idx, st->indx_SF, slot,
         alg_raw, eff_alg, kid_raw, eff_kid,
         tg_raw, eff_tg, src_raw, eff_src, is_good);

    // --- 6) Управление серией (как в твоём варианте: без SRC в условии смены) ---
#if AVR_SCOUT_FIND_GOOD_REGIONS
    //    const uint8_t group_changed = (r->active) && (r->alg_id != eff_alg || r->key_id != eff_kid || r->tg != eff_tg);
    uint32_t prev_tg = r->tg;
    uint8_t tg_stable = 1;
    if (eff_tg != prev_tg)
    {
        uint32_t diff = (eff_tg > prev_tg) ? (eff_tg - prev_tg) : (prev_tg - eff_tg);
        if (eff_tg > 0xFFFF || diff > 10000)
            tg_stable = 0;
    }

    uint8_t group_changed = 0;

    if (r->active && can_use_eff)
    {
        if ((r->alg_id != eff_alg || r->key_id != eff_kid) && eff_tg > 0)
            group_changed = 1;
        else if (r->tg != eff_tg)
            group_changed = (tg_stable == 1);
    }

    if (r->active && group_changed)
    {
        SLOG(" group_changed: was alg=0x%02X kid=%u tg=%u src=%u "
             "→ now alg=0x%02X kid=%u tg=%u src=%u\n",
             r->alg_id, r->key_id, r->tg, r->src,
             eff_alg, eff_kid, eff_tg, eff_src);
    }

    if (!r->active)
    {
        /*
        if (is_good && (eff_tg || eff_alg))
        {
            scout_series_start(slot, eff_alg, eff_kid, eff_tg, eff_src, sf_idx);
            SLOG(" series START (slot=%u) len=1\n", slot);
        }
        else
        {
            SLOG(" series idle (bad SF) slot=%u sf=%u\n", slot, (unsigned)sf_idx);
        }
        */
        // Условие старта: есть TG/ALG ИЛИ есть стабилизированный ALG из PI/LE.
        // При этом SF должен быть "хорошим" (с учётом ACCEPT_ALL_ENC_VC).
        if (is_good && (eff_tg || eff_alg || db_eff_alg_valid[slot]))
        {
            // Если используем стабилизированный ALG, подтянем и KID
            const uint8_t start_alg = (eff_alg != 0) ? eff_alg : db_eff_alg[slot];
            const uint8_t start_kid = (eff_kid != 0) ? eff_kid : db_eff_kid[slot];

            scout_series_start(slot, start_alg, start_kid, eff_tg, eff_src, sf_idx);
            SLOG(" series START (slot=%u) len=1 [tg=%u alg=0x%02X]\n", slot, eff_tg, start_alg);
        }
        else
        {
            // Более корректное сообщение в логе
            SLOG(" series idle (no call info or bad SF) slot=%u sf=%u\n", slot, (unsigned)sf_idx);
        }
    }
    else
    {
        SLOG(" ??? series FINALIZE ? (slot=%u) len=%u, group_changed %d, is_good %d \n", slot, r->len_sf, group_changed, is_good);
        if (!is_good || group_changed || r->len_sf >= AVR_SCOUT_MAX_SF)
        {
            const char *reason = !is_good ? "bad" : (group_changed ? "grp" : "full");
            SLOG(" series FINALIZE (slot=%u) len=%u reason=%s\n",
                 slot, r->len_sf, reason);
            // финализирует и создаст окно при достижении порога
            scout_series_finalize_and_store(opts, st, slot);

            if (is_good)
            {
                scout_series_start(slot, eff_alg, eff_kid, eff_tg, eff_src, sf_idx);
                SLOG(" series RE-START (slot=%u) len=1\n", slot);
            }
        }
        else
        {
            scout_series_extend(slot);
            SLOG(" series EXTEND (slot=%u) len=%u\n", slot, r->len_sf);
        }
    }
#endif // AVR_SCOUT_FIND_GOOD_REGIONS

    // --- 7) Групповые счётчики (ключ без SRC, как у тебя сейчас) ---
#if AVR_SCOUT_GROUP_STATS
    // Не создаём "пустую" группу, если нет ни TG, ни ALG
    if (eff_tg != 0 || eff_alg != 0)
    {
        const int gi = scout_find_or_make_group_key(slot, eff_alg, eff_kid, eff_tg);
        SLOG("const int gi = {%d} !!\n\n", gi);
        if (gi >= 0)
        {
            avr_scout_group_t *G = &SC.groups[gi];
            G->sf_total++;
            if (is_good)
                G->sf_good++;

            G->sf_total_scan++;
            if (SC.hist[slot][wr].good_scan)
                G->sf_good_scan++;
            if (st->Priority3 > 0)
                G->Priority++;
            if (st->irr_err > 0)
                G->irr_err++;
            if (st->irr_err > 0 && (st->Priority1 > 0 || st->Priority2 > 0 || st->Priority3 > 0))
            {
                ippl_add("kEncrypted", "1"); // IPP
                SLOG(" VEDA Priority[%d] irr_err=%u\n", G->Priority, G->irr_err);
            }
            G->Synctype = st->synctype;
            SLOG(" grp[%d] sf_total=%u sf_good=%u\n", gi, (unsigned)G->sf_total, (unsigned)G->sf_good);
        }
        else
        {
            SLOG(" warn: group not found/made (slot=%u)\n", slot);
        }
        st->irr_err = 0;
        st->Priority3 = 0;
    }
#endif // AVR_SCOUT_GROUP_STATS

    SLOG(" <<< EXIT  sf=%u slot=%u post.active=%u post.len=%u\n",
         (unsigned)sf_idx, slot, r->active, r->len_sf);

    st->DMRvcL = 0;       // считать VF 0..17 как в live
    st->bit_counterL = 0; // сбросить битовый счётчик OFB только один раз

    // --- 8) Сдвиг кольца истории — строго в самом конце ---
    SC.hist_wr[slot] = (wr + 1) % AVR_SCOUT_MAX_SF;

    // отметка «ключ применён», если контекст совпал (оставлено как у тебя)
    scout_mark_key_applied_if_match(st, SC.last_slot);
}

// Предполагаем, что в scout_sf_entry_t есть булев флаг good (0/1).
// Если имя иное (is_good/kv_ok), поменяй ниже.
static inline int scout_hist_sf_is_good(uint8_t slot, uint32_t sf_idx)
{
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == sf_idx)
        {
            return e->good ? 1 : 0; // <-- поправь имя поля при необходимости
        }
    }
    return 0; // нет записи — считаем «плохой», чтобы не завышать оценку
}
// вызывать сразу после того, как ты распарсил TLC/VLC и знаешь TG/SRC для слота
void avr_scout_on_lc_update(dsd_state *st, uint8_t slot, uint32_t tg, uint32_t src)
{
    (void)st;
#if AVR_SCOUT_VALIDATE_LC_IN_UPDATE
    // Обновлять last_tg/src только на «годных» суперкадрах и с вменяемыми значениями
    if (!scout_errs2_good(st, slot))
        return; // errs2 фильтр
    if (tg == 0 || src == 0)
        return; // отбрасываем нули
    if ((tg & 0xFFFFFFu) == 0xFFFFFFu)
        return; // отбрасываем 0xFFFFFF (мусор)
    if ((src & 0xFFFFFFu) == 0xFFFFFFu)
        return;
#endif
    // запомнить последние валидные значения для слота (отдельно от dsd_state)
    SC.last_tg[slot] = tg;
    SC.last_src[slot] = src;
}

// Завершает работу Scout по окончании голоса/потока.
void avr_scout_finalize_series(dsd_opts *opts, dsd_state *st)
{
    uint8_t slot = st->currentslot;
    if(opts->kv_csv_path[0]!='0' && SC.sf_idx>=6) {
        return;
    }

    SLOG("\n\n avr_scout_finalize_series %u !!!\n\n", slot);
    if (SC.run[slot].active) 
        scout_series_finalize_and_store(opts, st, slot);
}

// форсируем добавление окна из текущей (незавершённой) серии на заданном слоте
static void scout_force_window_on_flush(dsd_opts *opts, dsd_state *st, uint8_t slot)
{
    (void)opts;
    if (!st)
        return;
#if AVR_SCOUT_FORCE_WINDOW_ON_FLUSH
    scout_run_t *r = &SC.run[slot];
    if (!r->active || r->len_sf == 0)
        return; // серии нет или пустая

    // найдём/создадим группу под признаки незавершённой серии
    // printf("3333 r->alg_id {%d} !!!\n\n", r->alg_id);
    const int gi = scout_find_or_make_group_key(slot, r->alg_id, r->key_id, r->tg);
    if (gi < 0)
        return;

    avr_scout_group_t *G = &SC.groups[gi];
    if (G->nwins >= AVR_SCOUT_MAX_WINS_PER_GROUP)
        return;

    avr_scout_window_ref_t wref;
    wref.slot = slot;
    wref.quality = 1; // минимальный маркер качества; при желании можешь скорректировать
    wref.start_sf_idx = r->start_sf_idx;

    // длина окна: возьмём минимум из фактически набранных SF и 3,
    // чтобы батчу было что проверять даже на коротких клипах
    uint8_t len = (r->len_sf >= 3) ? 3 : (uint8_t)r->len_sf;
    if (len > 8)
        len = 8; // технический максимум на окно
    wref.len_sf = len;
    wref.alg_id = r->alg_id;
    wref.key_id = r->key_id;
    wref.tg = r->tg;
    wref.src = r->src;

    G->win[G->nwins++] = wref;

// если хочешь закрыть серию окончательно — сбрось её здесь
// r->active = 0; r->len_sf = 0;
#endif
}

bool avr_scout_is_series_ready(uint8_t slot)
{
    if (slot > 1)
        return false;
    fprintf(stderr, "[SCOUT] avr_scout_is_series_ready\n");
    // «Готово» = уже создано >=1 окна в любой из групп этого слота
    for (int g = 0; g < SC.ngroups; ++g)
    {
        const avr_scout_group_t *grp = &SC.groups[g];
        fprintf(stderr, "[SCOUT] %d. grp->slot %d.  grp->nwins %d\n", g, grp->slot, grp->nwins);
        if (grp->slot == slot && grp->nwins > 0)
            return true;
    }
    return false;
}

// Закрывает хвостовые серии, и печатает сводный отчёт.
void avr_scout_flush(dsd_opts *opts, dsd_state *st, bool ms_mode)
{
    // сначала попытаемся штатно финализировать серии (закроет длинные)
    if (SC.ngroups < 0 || SC.ngroups > AVR_SCOUT_MAX_GROUPS)
    {
        fprintf(stderr, "[SCOUT] WARN: SC.ngroups out-of-range (%d), resetting internal SC\n", SC.ngroups);
        memset(&SC, 0, sizeof(SC));
        memset(SC_prev_mp, 0, sizeof(SC_prev_mp));
    }

    SLOG("SC.sf_idx[%u] = %d\n", st->currentslot, SC.sf_idx[st->currentslot]);

    if (opts->kv_csv_path[0] != '\0' && ms_mode == 0)
    { // ДЛЯ BS
        if (SC.sf_idx[st->currentslot] < 6)
        {
            SLOG("\n avr_scout_flush STOP ms_mode=%d\n", ms_mode);
            SC.ngroups = 0;
            SC.sf_idx[st->currentslot] = 0;
        }
        else
            return;
    }
    // сначала попытаемся штатно финализировать серии (закроет длинные)
    avr_scout_finalize_series(opts, st);

    st->ms_mode = ms_mode;
    // защищённая запись в внешний стейт
    st->ngroups = (SC.ngroups < 0 || SC.ngroups > AVR_SCOUT_MAX_GROUPS) ? 0 : SC.ngroups;

    // затем, если остались короткие незавершённые — форсируем по одному окну на слот
    scout_force_window_on_flush(opts, st, 0);
    scout_force_window_on_flush(opts, st, 1);
    // и уже потом печатаем сводку (теперь wins не будет 0 на коротком файле)
    avr_scout_report_summary(ms_mode, st);
}
// Отладочный дамп (можно вызывать в любом месте для быстрого просмотра стейта).
void avr_scout_dump_groups(dsd_opts *opts, dsd_state *st)
{
    (void)opts;
    (void)st;
    for (int i = 0; i < SC.ngroups; ++i)
    {
        const avr_scout_group_t *G = &SC.groups[i];
#if AVR_SCOUT_VERBOSE
        SLOG("grp[%d] slot=%u alg=0x%02X kid=%u tg=%u src=%u sf=%u/%u wins=%u\n",
             i, G->slot, G->alg_id, G->key_id, G->tg, G->src,
             (unsigned)G->sf_good, (unsigned)G->sf_total, G->nwins);
#else
        (void)G;
#endif
    }
}

int avr_scout_has_series_with_min_sf(uint8_t slot, uint8_t min_sf)
{
    slot &= 1;
#if AVR_SCOUT_FIND_GOOD_REGIONS
    // 1) проверяем «живую» серию на слоте
    const scout_run_t *r = &SC.run[slot];
    SLOG("???? if (r->active %d && r->len_sf %d >= min_sf %d) \n", r->active, r->len_sf, min_sf);
    if (r->active && r->len_sf >= min_sf)
        return 1;

#endif
#if AVR_SCOUT_GROUP_STATS
    // 2) проверяем уже сохранённые окна в группах
    for (int gi = 0; gi < SC.ngroups; ++gi)
    {
        const avr_scout_group_t *G = &SC.groups[gi];
        SLOG("???? grp[%d] slot=%u alg=0x%02X kid=%u tg=%u src=%u sf=%u/%u wins=%u\n",
             gi, G->slot, G->alg_id, G->key_id, G->tg, G->src,
             (unsigned)G->sf_good, (unsigned)G->sf_total, G->nwins);

        if (G->slot != slot)
            continue;
        for (int w = 0; w < G->nwins; ++w)
        {
            if (G->win[w].len_sf >= min_sf)
                return 1;
        }
    }
#endif
    return 0;
}

// helper: записать один дибит (0..107) в out27
static inline void scout_put_dibit(uint8_t out27[27], int k, uint8_t dibit)
{
    int byte = k >> 2;              // каждые 4 дибита = 1 байт
    int shift = 6 - ((k & 3) << 1); // позиции 6,4,2,0
    out27[byte] |= (uint8_t)((dibit & 0x3) << shift);
}

// helper: извлечь дибит из AMBE-кадра по таблицам rW/rX/rY/rZ
static inline uint8_t scout_get_dibit(char fr[4][24], int i)
{
    extern const int rW[36], rX[36], rY[36], rZ[36];
    uint8_t b1 = (uint8_t)(fr[rW[i]][rX[i]] & 1);
    uint8_t b0 = (uint8_t)(fr[rY[i]][rZ[i]] & 1);
    return (uint8_t)((b1 << 1) | b0);
}

int scout_pack_27bytes_from_frames(char fr1[4][24],
                                   char fr2[4][24],
                                   char fr3[4][24],
                                   uint8_t out27[27])
{
    if (!fr1 || !fr2 || !fr3 || !out27)
        return -1;
    memset(out27, 0, 27);

    for (int i = 0; i < 36; i++)
        scout_put_dibit(out27, 0 + i, scout_get_dibit(fr1, i));

    for (int i = 0; i < 18; i++)
        scout_put_dibit(out27, 36 + i, scout_get_dibit(fr2, i));

    for (int i = 18; i < 36; i++)
        scout_put_dibit(out27, 54 + (i - 18), scout_get_dibit(fr2, i));

    for (int i = 0; i < 36; i++)
        scout_put_dibit(out27, 72 + i, scout_get_dibit(fr3, i));

    return 0;
}

/* Внутренний поиск по sf_idx. Возвращает индекс в кольце или -1. */
static int scout_find_hist_index_by_sf(uint8_t slot, uint32_t sf_idx)
{
    slot &= 1;
    for (int i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const int idx = i; // линейный просмотр; при желании сделайте обратный от hist_wr
        if (SC.hist[slot][idx].sf_idx == sf_idx)
            return idx;
    }
    return -1;
}

int avr_scout_get_6x27B_and_6xIV_by_sf(uint8_t slot, uint32_t sf_idx,
                                       uint8_t out27[AVR_SCOUT_VC_PER_SF][27],
                                       uint8_t outiv[AVR_SCOUT_VC_PER_SF][16],
                                       uint8_t *mask)
{
    slot &= 1;
    const int idx = scout_find_hist_index_by_sf(slot, sf_idx);
    if (idx < 0)
    {
        if (mask)
            *mask = 0;
        return -1; // записи с таким sf_idx нет
    }

    const scout_sf_entry_t *E = &SC.hist[slot][idx];
    const uint8_t have = E->have27_mask;

    if (out27)
    {
        for (int b = 0; b < AVR_SCOUT_VC_PER_SF; ++b)
        {
            if (have & (1u << b))
                memcpy(out27[b], E->data27[b], 27);
            else
                memset(out27[b], 0, 27); // или оставьте как есть
        }
    }
    if (outiv)
    {
        for (int b = 0; b < AVR_SCOUT_VC_PER_SF; ++b)
        {
            if (have & (1u << b))
                memcpy(outiv[b], E->iv16[b], 16);
            else
                memset(outiv[b], 0, 16);
        }
    }
    if (mask)
        *mask = have;

    /* полный комплект — 0x3F; если не полный, возвращаем 1 как предупреждение */
    return (have == 0x3F) ? 0 : 1;
}

int avr_scout_get_vc27_by_sf(uint8_t slot, uint32_t sf_idx, int vc_idx,
                             uint8_t out27[27], uint8_t outiv[16])
{
    slot &= 1;
    if (vc_idx < 0 || vc_idx >= AVR_SCOUT_VC_PER_SF)
        return -2;

    const int idx = scout_find_hist_index_by_sf(slot, sf_idx);
    if (idx < 0)
        return -1;

    const scout_sf_entry_t *E = &SC.hist[slot][idx];
    if ((E->have27_mask & (1u << vc_idx)) == 0)
        return -2;

    if (out27)
        memcpy(out27, E->data27[vc_idx], 27);
    if (outiv)
        memcpy(outiv, E->iv16[vc_idx], 16);
    return 0;
}

void avr_scout_on_vc(const dsd_state *st, const uint8_t enc27[27], const uint8_t iv16[16])
{
    if (!st || !enc27 || !iv16)
        return;

    const uint8_t slot = scout_slot(st);
    const uint32_t wr = SC.hist_wr[slot] % AVR_SCOUT_MAX_SF;
    scout_sf_entry_t *E = &SC.hist[slot][wr];

    // индекс VC* внутри текущего KV-SF: каждые 3 VF = 1 VC*, используем DMRvc % 18
    // Вызываем мы на третьем VF тройки, т.е. DMRvc указывает на этот VF, нам нужен индекс 0..5:
    int vc_in_sf = ((st->DMRvcL % 18) / 3);
    if (vc_in_sf < 0 || vc_in_sf >= AVR_SCOUT_VC_PER_SF)
        vc_in_sf = 0;

    memcpy(E->data27[vc_in_sf], enc27, 27);
    memcpy(E->iv16[vc_in_sf], iv16, 16);
    E->have27_mask |= (uint8_t)(1u << vc_in_sf);
}

// ---------- формат HEX для полей alg_id/key_id ----------
static inline void hex_u8_str(char out[6], uint8_t v)
{
    // "0x" + 2 HEX + '\0' = 5
    snprintf(out, 6, "0x%02X", (unsigned)v);
}

// ---------- проверка условия can_decrypt ----------
static int has_six_consecutive_sf_le2(const scout_sf_stat_t *sf, uint32_t n)
{
    if (!sf || n < 6)
        return 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (sf[i].errs2 <= 2)
        {
            if (++run >= 6)
                return 1;
        }
        else
        {
            run = 0;
        }
    }
    return 0;
}

// ---------- вспомогательный поиск по кольцевой истории SC.hist ----------
static inline const scout_sf_entry_t *scout_hist_find_by_sf(uint8_t slot, uint32_t sf_idx)
{
    // Пробегаем всю кольцевую историю слота и ищем запись по sf_idx.
    // Ожидается, что scout_sf_entry_t имеет поле .sf_idx (как в вашем примере с prev_mp).
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == sf_idx)
        {
            return e;
        }
    }
    return NULL;
}

#define DMR_MS_PER_SUPERFRAME 540
/*
 * ГЛАВНАЯ ФУНКЦИЯ, КОТОРУЮ ВЫ ПРОСИЛИ ПЕРЕДЕЛАТЬ:
 * Никаких SC в параметрах — берём всё из глобального SC по индексу группы gi.
 *
 * Аргументы:
 *   gi             — индекс группы в SC.groups[gi]
 *   R              — выходной агрегированный отчёт (паспорт + суммарики)
 *   out_sf_buf     — выход: динамически выделенный массив по-SF errs2 (для can_decrypt)
 *   out_sf_count   — выход: размер массива
 *   out_key_applied— выход: флаг «ключ был применён/подтверждён» (см. комментарий внутри)
 */
static void fill_report_from_internal_group(int gi,
                                            scout_group_report_t *R,
                                            scout_sf_stat_t **out_sf_buf,
                                            uint32_t *out_sf_count,
                                            int *out_key_applied)
{
    memset(R, 0, sizeof(*R));
    *out_sf_buf = NULL;
    *out_sf_count = 0;
    *out_key_applied = 0;

    if (gi < 0 || gi >= SC.ngroups)
        return;

    const avr_scout_group_t *G = &SC.groups[gi];

    // 1) Паспорт группы
    R->slot = G->slot;
    R->alg_id = G->alg_id;
    R->key_id = G->key_id;
    R->tg = G->tg;
    R->src = G->src;

    // 2) Суммарики по группе
    R->sf_total = G->sf_total;
    R->sf_good = G->sf_good;
    R->fec_err_total = 0; // посчитаем дальше из истории по SF
    R->duration_sf = G->sf_total;
    R->duration_ms = G->sf_total * (uint32_t)DMR_MS_PER_SUPERFRAME;

    // 3) Соберём errs2 по всем SF группы в порядке возрастания sf_idx, идя по окнам
    //    ПРЕДПОЛОЖЕНИЕ по полям окна (поправьте имена, если отличаются):
    //    avr_scout_window_ref_t { uint32_t start_sf_idx; uint8_t len_sf; }
    uint32_t expected = G->sf_total;
    if (expected == 0)
    {
        // Пустая группа — ничего больше не делаем
        return;
    }

    scout_sf_stat_t *buf = (scout_sf_stat_t *)calloc(expected, sizeof(scout_sf_stat_t));
    if (!buf)
    {
        fprintf(stderr, "[SCOUT] WARN: no memory for sf_buf (group %d)\n", gi);
        return;
    }

    uint32_t idx = 0;
    for (uint8_t w = 0; w < G->nwins; ++w)
    {
        const avr_scout_window_ref_t *W = &G->win[w];

        // ----- ВАЖНО -----
        // Если у вас другие имена полей, замените здесь:
        //   W->start_sf_idx  -> стартовый sf_idx окна
        //   W->len_sf        -> длина окна в SF
        uint32_t start_sf_idx = W->start_sf_idx; // <--- замените, если у вас другое имя
        uint32_t len_sf = W->len_sf;             // <--- замените, если у вас другое имя

        for (uint32_t k = 0; k < len_sf; ++k)
        {
            if (idx >= expected)
                break; // защита от переполнения

            uint32_t sf_i = start_sf_idx + k;
            const scout_sf_entry_t *E = scout_hist_find_by_sf(G->slot, sf_i);
            uint32_t errs2_val = 0;

            if (E)
            {
                // ----- ВАЖНО -----
                // Если в вашем scout_sf_entry_t поле называется иначе — поправьте здесь:
                // предполагаем: E->errs2 (uint32_t/uint16_t)
                errs2_val = 0; // (uint32_t)E->errs2;
            }
            else
            {
                // Если запись не найдена (отброшена из кольца) — errs2 неизвестен.
                // По умолчанию 0; при желании можно поставить 3+, чтобы это портило can_decrypt.
                errs2_val = 0;
            }

            buf[idx].errs2 = errs2_val;
            R->fec_err_total += errs2_val;
            ++idx;
        }
    }

    // Если окна не покрыли sf_total (например, часть SF ушла из кольца),
    // подровняем размер до фактически собранного
    *out_sf_count = idx;

    // 4) Флаг «ключ применён/подтверждён».
    //    Здесь аккуратная заглушка: если у вас есть реальный флаг подтверждения применения ключа
    //    (например, «успешная валидация по smooth/CRC/BER»), подставьте его сюда.
    //
    //    Примеры источников:
    //    - флаг из kv-подсистемы (например, G->kv_applied_confirmed == 1)
    //    - или эвристика: (G->alg_id != 0 && G->sf_good > 0 && G->var_median < THR)
    //
    // *out_key_applied = G->kv_applied_confirmed;  // если есть такой флаг
    *out_key_applied = 0; // по умолчанию 0 — дальше decrypt станет 2 для шифрованных групп

    // 5) Готово — отдаём буфер
    *out_sf_buf = buf;
}

// быстрый фолбэк: если все SF в группе — good, то правило 6 подряд точно выполняется
static inline int scout_can_decrypt_fallback(const avr_scout_group_t *G)
{
    if (G->alg_id == 0x00)
        return 0; // открытая — 0
    if (G->sf_good >= AVR_SCOUT_VC_PER_SF)
        return 1; // достаточно good-SF в принципе
    return 0;
}

// Правила для can_decrypt:
// - если группа не зашифрована (alg_id == 0x00), то всегда 0
// - иначе: 1, если есть 6 подряд SF с errs2 <= 2, иначе 0
static int compute_can_decrypt(uint8_t alg_id,
                               const scout_sf_stat_t *sf, uint32_t n)
{
    if (alg_id == 0x00)
        return 0;
    return has_six_consecutive_sf_le2(sf, n) ? 1 : 0;
}

// Возвращает 1, если в группе gi есть 6 подряд SF с errs2 <= 2, иначе 0.
// Если группа открытая (alg_id == 0x00) — всегда 0.
#ifndef SCOUT_SF_IS_GOOD
// при необходимости поменяй на ((e)->is_good) или ((e)->kv_ok)
#define SCOUT_SF_IS_GOOD(e) ((e)->good)
#endif

static int scout_can_decrypt_for_group2(int gi)
{
    if (gi < 0 || gi >= SC.ngroups)
        return 0;

    const avr_scout_group_t *G = &SC.groups[gi];
    if (G->alg_id == 0x00)
        return 0;

    // меньше 6 SF – сразу нет
    if (G->sf_total < AVR_SCOUT_MAX_SF)
        return 0;

    uint32_t run = 0;

    for (uint8_t w = 0; w < G->nwins; ++w)
    {
        const avr_scout_window_ref_t *W = &G->win[w];
        const uint32_t start_sf_idx = W->start_sf_idx;
        const uint32_t len_sf = W->len_sf;

        for (uint32_t k = 0; k < len_sf; ++k)
        {
            const uint32_t sf_i = start_sf_idx + k;

            int have_hist = 0, is_good = 0;

            // ищем sf_i в истории слота
            for (uint32_t j = 0; j < AVR_SCOUT_MAX_SF; ++j)
            {
                const scout_sf_entry_t *e = &SC.hist[G->slot][j];
                if (e->sf_idx == sf_i)
                {
                    have_hist = 1;
                    is_good = SCOUT_SF_IS_GOOD(e) ? 1 : 0;
                    break;
                }
            }

            // строго: отсутствие записи = обрыв последовательности
            if (!have_hist || !is_good)
            {
                run = 0;
            }
            else
            {
                if (++run >= AVR_SCOUT_MAX_SF)
                    return 1;
            }
        }
    }
    return 0;
}

static int scout_can_decrypt_for_group(int gi)
{
    if (gi < 0 || gi >= SC.ngroups)
        return 0;
    const avr_scout_group_t *G = &SC.groups[gi];
    if (G->alg_id == 0x00)
        return 0;
    // КОРОТКИЕ ГРУППЫ: если меньше 6 SF, то "можно подбирать",
    // только если все имеющиеся SF хорошие
    if (G->sf_good < AVR_SCOUT_VC_PER_SF)
    {
        int ok = (G->sf_total > 0 && G->sf_good == G->sf_total) ? 1 : 0;
#if SCOUT_CAN_DEC_DBG
        fprintf(stderr, "[SCOUT] can_decrypt(short) gi=%d slot=%u sf_total=%u sf_good=%u -> %d\n",
                gi, G->slot, G->sf_total, G->sf_good, ok);
#endif
        // return ok;
        return false;
    }
    // Попробуем по окнам пройтись покадрово (если good сохранён в hist)
    uint32_t run = 0;
    for (uint8_t w = 0; w < G->nwins; ++w)
    {
        const avr_scout_window_ref_t *W = &G->win[w];
        const uint32_t start_sf_idx = W->start_sf_idx;
        const uint32_t len_sf = W->len_sf;
        for (uint32_t k = 0; k < len_sf; ++k)
        {
            const uint32_t sf_i = start_sf_idx + k;
            // здесь нужен флаг e->good в SC.hist; если его нет — сразу к фолбэку
            int have_hist = 0, is_good = 0;
            for (uint32_t j = 0; j < AVR_SCOUT_MAX_SF; ++j)
            {
                const scout_sf_entry_t *e = &SC.hist[G->slot][j];
                if (e->sf_idx == sf_i)
                {
                    have_hist = 1;
                    is_good = e->good ? 1 : 0;
                    break;
                }
            }
            if (!have_hist)
            {
                // истории не хватает — переходим к фолбэку по агрегатам
                return scout_can_decrypt_fallback(G);
            }
            if (is_good)
            {
                if (++run >= AVR_SCOUT_VC_PER_SF)
                    return 1;
            }
            else
            {
                run = 0;
            }
        }
    }
    // Не нашли подряд — используем фолбэк (например, твой кейс 42/42)
    return scout_can_decrypt_fallback(G);
}

// ---------- Печать одной группы в JSON (с «quality» в процентах) ----------
static void json_write_one_group_ext(FILE *f, const avr_scout_group_t *G, int gi, int need_comma, int mfid, int veda)
{
    if (G->tg == 0)
        return;
    char alg_hex[AVR_SCOUT_VC_PER_SF], key_hex[AVR_SCOUT_VC_PER_SF];
    hex_u8_str(alg_hex, G->alg_id);
    hex_u8_str(key_hex, G->key_id);

    // quality_* для SCAN и KV
    uint32_t quality_kv = 0;
    if (G->sf_total_kv)
    {
        double q = floor(0.5 + (100.0 * (double)G->sf_good_kv / (double)G->sf_total_kv));
        if (q < 0.0)
            q = 0.0;
        if (q > 100.0)
            q = 100.0;
        quality_kv = (uint32_t)q;
    }

    uint32_t sf_bad = 0;
    if (G->sf_total > G->sf_good)
        sf_bad = G->sf_total - G->sf_good;
    int curr_alg = 0;
    if (G->alg_id == 0x00 && mfid == -1)
    {
        if (G->sf_good > 4)
        {
            if (veda)
            {
                if ((G->Priority * 10 > G->sf_good * 8) || (G->irr_err * 10 > G->sf_good * 8))
                    curr_alg = 0xEF;
            }
            else
            {
                if ((G->Priority * 10 > G->sf_good * 8) && (G->irr_err * 10 > G->sf_good * 8))
                    curr_alg = 0xEF;
            }
        }
    }
    // alg_name
    const char *alg_name =
        (G->alg_id == 0x25) ? "AES-256" : (G->alg_id == 0x24)                   ? "AES-128"
                                      : (G->alg_id == 0x21)                     ? "RC4"
                                      : (G->alg_id == 0x22)                     ? "DES64"
                                      : (G->alg_id == 0x23)                     ? "AES-192"
                                      : (G->alg_id == 0x26)                     ? "Kirisan BP"
                                      : (G->alg_id == 0xFC)                     ? "TYT BP"
                                      : (G->alg_id == 0xFD)                     ? "TYT PC4"
                                      : (G->alg_id == 0xFE)                     ? "TYT EP"
                                      : (G->alg_id == 0xD1)                     ? "XOR"
                                      : (G->alg_id == 0xD2)                     ? "LIRA"
                                      : (curr_alg == 0xEF || G->alg_id == 0xEF) ? "VEDA"
                                      : (G->alg_id == 0x00)                     ? "Clear"
                                                                                : "Unknown";
    // int quality_percent = 0;
    uint32_t duration_ms = G->sf_total * 540; // DMR_MS_PER_SUPERFRAME = 540 мс

    // ---- собрать reason (только из реальных полей) ----
    // ---- Собрать reason из РЕАЛЬНЫХ полей G (без новых счётчиков) ----
    char reason[192];
    reason[0] = '\0';

    // 0) Базовые “нет окна/нет KV”
    if (G->nwins == 0 && G->sf_total_kv == 0)
    {
        strcat(reason, (reason[0] ? "; " : ""));
        strcat(reason, "no_window");
    }

    // 1) Заголовки PI/LE
    if (!G->pi_seen && G->le_seen)
    {
        strcat(reason, (reason[0] ? "; " : ""));
        strcat(reason, "le_only");
    }
    else if (!G->pi_seen && !G->le_seen)
    {
        strcat(reason, (reason[0] ? "; " : ""));
        strcat(reason, "no_headers");
    }
    else if (G->pi_seen && G->pi_crc_ok_ratio == 0.0)
    {
        strcat(reason, (reason[0] ? "; " : ""));
        strcat(reason, "pi_crc_0");
    }
    else if (G->pi_seen && G->pi_crc_ok_ratio > 0.0 && G->pi_crc_ok_ratio < 0.8)
    {
        strcat(reason, (reason[0] ? "; " : ""));
        strcat(reason, "pi_crc_low");
    }

    // 3) Качество KV против SCAN
    int q_scan = (G->sf_total_scan > 0) ? (int)floor(0.5 + 100.0 * (double)G->sf_good_scan / (double)G->sf_total_scan) : 0;
    int q_kv = (G->sf_total_kv > 0) ? (int)floor(0.5 + 100.0 * (double)G->sf_good_kv / (double)G->sf_total_kv) : 0;
    // «короткий участок» — мягкий сигнал, только если при этом скан-качество 100%
    uint32_t quality_scan = (G->sf_total_scan > 0) ? (int)floor(0.5 + 100.0 * (double)G->sf_good_scan / (double)G->sf_total_scan) : 0;
    uint32_t quality = (G->sf_total > 6) ? (uint32_t)lround(100.0 * (double)G->sf_good / (double)G->sf_total) : 0;

    // decrypt: 0=open; 1=ключ применён; 2=encrypted, ключ не применён
    int decrypt = (G->alg_id == 0x00 && curr_alg == 0) ? 0 : (G->key_applied_confirmed ? 1 : 2);
    // can_decrypt — строго по твоей функции:
    // int can_decrypt = scout_can_decrypt_for_group(gi);
    int can_decrypt = ((mfid == -1 || quality < 70) ? 0 : scout_can_decrypt_for_group(gi));

    // Печать JSON (поля и порядок — как в твоём примере)
    fprintf(f,
            "    {\n"
            "      \"slot\": %u,\n"
            "      \"alg_id\": \"%s\",\n"
            "      \"alg_name\": \"%s\",\n"
            "      \"key_id\": \"%s\",\n"
            "      \"tg\": %u,\n"
            "      \"src\": %u,\n"
            "\n"
            "      \"duration_ms\": %u,\n"
            "      \"duration_sf\": %u,\n"
            "      \"sf_good\": %u,\n"
            "      \"sf_bad\": %u,\n"
            "      \"quality\": %d,\n"
            "\n"
            "      \"nwins\": %u,\n"
            "      \"key_applied_confirmed\": %u,\n"
            "\n"
            "      \"sf_total_kv\": %u,\n"
            "      \"sf_good_kv\": %u,\n"
            "      \"quality_kv\": %u,\n"
            "      \"Synctype\": %u,\n"
            "      \"Priority\": %u,\n"
            "      \"irr_err\": %u,\n"
            "\n"
            "      \"flco_err_count\": %u,\n"
            "      \"le_seen\": %s,\n"
            "      \"pi_seen\": %s,\n"
            "      \"pi_crc_ok_ratio\": %.2f,\n"
            "      \"mi_unique\": %u,\n"
            "      \"mi_mismatch_count\": %u,\n"
            "\n"
            "      \"smooth_avg\": %.2f,\n"
            "      \"smooth_std\": %.2f,\n"
            "      \"smooth_median\": %.2f,\n"
            "      \"smooth_p95\": %.2f,\n"
            "      \"smooth_gate_hits\": %u,\n"
            "      \"voice_confidence\": %.2f,\n"
            // "\n"
            // "      \"notes\": \"%s\"\n"
            "\n"
            // "      \"reason\": \"%s\",\n"
            "      \"decrypt\": %d,\n"
            "      \"can_decrypt\": %d\n"
            "    }%s\n",
            (unsigned)G->slot,
            alg_hex,
            alg_name,
            key_hex,
            (unsigned)G->tg,
            (unsigned)G->src,

            (unsigned)duration_ms,
            (unsigned)G->sf_total,
            (unsigned)G->sf_good,
            (unsigned)sf_bad,
            (unsigned)quality,

            (unsigned)G->nwins,
            (unsigned)G->key_applied_confirmed,

            (unsigned)G->sf_total_kv,
            (unsigned)G->sf_good_kv,
            (unsigned)quality_kv,
            (unsigned)G->Synctype,
            (unsigned)G->Priority,
            (unsigned)G->irr_err,

            (unsigned)G->flco_err_count,
            G->le_seen ? "true" : "false",
            G->pi_seen ? "true" : "false",
            (G->pi_crc_ok_ratio < 0.0 ? 0.0 : (G->pi_crc_ok_ratio > 1.0 ? 1.0 : G->pi_crc_ok_ratio)),
            (unsigned)G->mi_unique,
            (unsigned)G->mi_mismatch_count,

            G->smooth_avg,
            G->smooth_std,
            G->smooth_median,
            G->smooth_p95,
            (unsigned)G->smooth_gate_hits,
            (G->voice_confidence < 0.0 ? 0.0 : (G->voice_confidence > 1.0 ? 1.0 : G->voice_confidence)),
            // G->notes[0]  ? G->notes  : "",
            // reason,
            decrypt,
            can_decrypt,
            need_comma ? "," : "");
}

// ---------- Главная функция записи JSON в корень программы ----------
// Запись расширенного JSON-отчёта разведчика в корень программы
// Требует: fill_report_from_internal_group(), compute_decrypt(), compute_can_decrypt(),
//          json_write_one_group() и глобальный SC.
// Теперь функция принимает opts/st — чтобы вычислить decrypt по состоянию

void avr_scout_write_json_summary_ext(const char *path, dsd_opts *opts, dsd_state *stat)
{
    (void)stat; // пока не используем доп. поля из state

    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "[SCOUT] ERROR: cannot open %s for write\n", path);
        return;
    }

    int Brand = 0;
    // slots_used и flco_err_total считаем по группам, без новых полей в SC
    unsigned slots_mask = 0;
    uint32_t flco_err_total = 0;
    int valid_pairs_cnt = 0; // Количество пар, не содержащих нулей
    int unique_cnt = 0;      // Количество уникальных пар среди валидных
    for (int gi = 0; gi < SC.ngroups; ++gi)
    {
        const avr_scout_group_t *G = &SC.groups[gi];
        slots_mask |= (1u << (G->slot & 1));
        flco_err_total += G->flco_err_count;
        if (G->alg_id > 0)
        {
            if (G->alg_id < 0x21 || G->alg_id > 0x25)
            {
                if (G->alg_id < 0xFC)
                    stat->analyzer = 1;
                if (G->alg_id == 0x26)
                {
                    Brand = 3; // kirisan
                }
                else if (G->alg_id == 0xD2)
                    Brand = 2; // LIRA
            }
            else
            {
                if (G->Synctype < 11)
                {
                    stat->analyzer = 3;
                }
            }
        }
        if (G->alg_id == 0x00 && stat->dmr_mfid == -1 && G->sf_good > 4)
        {
            if ((G->Priority * 10 > G->sf_good * 8) && (G->irr_err * 10 > G->sf_good * 8))
            {
                Brand = 1;
            }
        }

        if (Brand == 0 && stat->dmr_mfid == -1)
        {
            if (G->tg > 1000000 && G->src > 1000000)
            {
                valid_pairs_cnt++; // Нашли валидную пару для статистики

                int is_duplicate = 0;

                // Вложенный цикл для поиска дубликатов
                for (int j = 0; j < SC.ngroups; ++j)
                {
                    if (gi == j)
                        continue; // Не сравниваем строку саму с собой

                    // Если нашли совпадение пары в другой строке
                    if (SC.groups[j].tg == G->tg && SC.groups[j].src == G->src)
                    {
                        is_duplicate = 1;
                        break;
                    }
                }

                // Если дубликатов не найдено - считаем пару уникальной
                if (is_duplicate == 0)
                {
                    unique_cnt++;
                }
            }
        }
    }
    fprintf(stderr, "!!!! SC.ngroups %d, valid_pairs_cnt %d\n", SC.ngroups, valid_pairs_cnt);
    // Условие 2: Должна быть хотя бы одна валидная пара (valid_pairs_cnt > 0), чтобы не делить на 0
    if (Brand == 0 && stat->dmr_mfid == -1)
    {
        fprintf(stderr, " SC.ngroups %d, valid_pairs_cnt %d\n", SC.ngroups, valid_pairs_cnt);
        if (SC.ngroups >= 2 && valid_pairs_cnt > 0)
        {
            if (unique_cnt * 10 >= valid_pairs_cnt * 9)
            {
                Brand = 1;
            }
        }
    }
    if (Brand == 1)
    {
        sprintf(stat->dmr_branding, "%s", "VEDA");
        stat->analyzer = 3;
    }
    else if (Brand == 2)
    {
        sprintf(stat->dmr_branding, "%s", "LIRA");
        stat->analyzer = 3;
    }
    else if (stat->dmr_mfid == 0x0A || Brand == 3)
    {
        sprintf(stat->dmr_branding, "%s", "Kirisun");
        stat->analyzer = 2;
    }
    else if (stat->dmr_mfid == -1)
    {
        sprintf(stat->dmr_branding, "%s", "TYT");
    }
    else if (stat->dmr_mfid == 0x06)
        printf(stat->dmr_branding, "%s", "Motorola");
    else if (stat->dmr_mfid == 0x08)
        sprintf(stat->dmr_branding, "%s", "Hytera");
    else if (stat->dmr_mfid == 0x20)
    {
        sprintf(stat->dmr_branding, "%s", "Kenwood");
        stat->analyzer = 2;
    }
    else if (stat->dmr_mfid == 0x58)
    {
        sprintf(stat->dmr_branding, "%s", "Tait");
        stat->analyzer = 2;
    }
    else if (stat->dmr_mfid == 0x68)
        sprintf(stat->dmr_branding, "%s", "Hytera");
    else if (stat->dmr_mfid == 0x77)
    {
        sprintf(stat->dmr_branding, "%s", "Vertex Standard");
        stat->analyzer = 2;
    }
    // --- Корень ---
    fputs("{\n", f);
    fprintf(f, "  \"version\": \"%s\",\n", SC_VERSION); // задаёшь вручную
    fprintf(f, "  \"input_file\": \"%s\",\n", opts ? (opts->audio_in_dev ? opts->audio_in_dev : "") : "");
    fprintf(f, "  \"mode\": \"%s\",\n", SC.ms_mode ? "MS" : "BS");
    // needs to be analyzed
    // slots_used
    fprintf(f, "  \"need_analyzed\": %d,\n", stat->analyzer);

    fputs("  \"slots_used\": [", f);
    int first = 1;
    for (int s = 0; s < 2; ++s)
    {
        if (slots_mask & (1u << s))
        {
            if (!first)
                fputs(", ", f);
            fprintf(f, "%d", s);
            first = 0;
        }
    }
    fputs("],\n", f);

    fprintf(f, "  \"groups_total\": %d,\n", SC.ngroups);
    fprintf(f, "  \"flco_err_total\": %u,\n", (unsigned)flco_err_total);

    int quality_percent_all = 0;
    int quality_percent = 0;

    unsigned int total_sf = stat->total_sf[0] + stat->total_sf[1];
    unsigned int good_sf = stat->total_good[0] + stat->total_good[1];

    if (stat->total_sf[0] > 0)
    {
        double q = floor(0.5 + (100.0 * (double)stat->total_good[0] / (double)stat->total_sf[0]));
        if (q < 0.0)
            q = 0.0;
        if (q > 100.0)
            q = 100.0;
        quality_percent = (int)q;
        fprintf(f, "  \"SF Total [0]\": %u,\n", stat->total_sf[0]);
        fprintf(f, "  \"SF Good  [0]\": %u,\n", stat->total_good[0]);
        fprintf(f, "  \"Quality  [0], %%\": %d,\n", quality_percent);
    }

    if (stat->total_sf[1] > 0)
    {
        double q = floor(0.5 + (100.0 * (double)stat->total_good[1] / (double)stat->total_sf[1]));
        if (q < 0.0)
            q = 0.0;
        if (q > 100.0)
            q = 100.0;
        quality_percent = (int)q;
        fprintf(f, "  \"SF Total [1]\": %u,\n", stat->total_sf[1]);
        fprintf(f, "  \"SF Good  [1]\": %u,\n", stat->total_good[1]);
        fprintf(f, "  \"Quality  [1], %%\": %d,\n", quality_percent);
    }
    fprintf(f, "  \"Manufacturer ID \": %d,\n", stat->dmr_mfid);

    if (stat->dmr_branding[0])
        fprintf(f, "  \"Branding\": \"%s\",\n", stat->dmr_branding);

    if (stat->dmr_branding_sub[0])
        fprintf(f, "  \"Branding_sub    \": \"%s\",\n", stat->dmr_branding_sub);

    if (total_sf > 0)
    {
        double q_all = floor(0.5 + (100.0 * (double)good_sf / (double)total_sf));
        if (q_all < 0.0)
            q_all = 0.0;
        if (q_all > 100.0)
            q_all = 100.0;
        quality_percent_all = (int)q_all;
    }
    else
    {
        quality_percent_all = 0;
    }
    if (SC.sf_total[0] > 0 && SC.sf_total[1] > 0)
        fprintf(f, "  \"Quality All, %%\": %d,\n", quality_percent_all);

    // --- groups[] ---
    fputs("  \"groups\": [\n", f);
    for (int gi = 0; gi < SC.ngroups; ++gi)
    {
        json_write_one_group_ext(f, &SC.groups[gi], gi, (gi + 1 < SC.ngroups), stat->dmr_mfid, Brand);
    }
    fputs("  ]\n", f);
    fputs("}\n", f);
    fclose(f);

    fprintf(stderr, "[SCOUT] JSON saved: %s (groups=%d)\n", path, SC.ngroups);
}

// Явная очистка внутреннего стейта Scout — вызывать при старте файла/потока.
void avr_scout_reset(void)
{
    memset(&SC, 0, sizeof(SC));
    memset(SC_prev_mp, 0, sizeof(SC_prev_mp));
}

//===============================================================================
// найти «последнюю» группу данного слота (без создания, без переключений)
static int scout_last_group_idx_for_slot(uint8_t slot)
{
    for (int gi = SC.ngroups - 1; gi >= 0; --gi)
        if (SC.groups[gi].slot == slot)
            return gi;
    return -1;
}

// --- Scout debounce hook: вызывать с CRC-валидных PI/LC ---
void scout_db_on_pi_or_lc(uint8_t slot, uint8_t alg, uint8_t kid, int has_tg, uint32_t tg)
{
    // ALG
    if (alg)
    {
        if (db_raw_alg[slot] == alg)
            db_stab_alg[slot]++;
        else
        {
            db_raw_alg[slot] = alg;
            db_stab_alg[slot] = 1;
        }
        if (db_stab_alg[slot] >= 2)
        {
            db_eff_alg[slot] = db_raw_alg[slot];
            db_eff_alg_valid[slot] = 1;
        }
    }

    // KEY
    if (kid)
    {
        if (db_raw_kid[slot] == kid)
            db_stab_kid[slot]++;
        else
        {
            db_raw_kid[slot] = kid;
            db_stab_kid[slot] = 1;
        }
        if (db_stab_kid[slot] >= 2)
        {
            db_eff_kid[slot] = db_raw_kid[slot];
            db_eff_kid_valid[slot] = 1;
        }
    }

    // TG (заготовка — активируется, когда подключим LC)
    if (has_tg && tg < 10000)
    {
        if (db_raw_tg[slot] == tg)
            db_stab_tg[slot]++;
        else
        {
            db_raw_tg[slot] = tg;
            db_stab_tg[slot] = 1;
        }
        if (db_stab_tg[slot] >= 2)
        {
            db_eff_tg[slot] = db_raw_tg[slot];
            db_eff_tg_valid[slot] = 1;
        }
    }
}

void avr_scout_stat_le_observe(uint8_t slot, int crc_ok)
{
    if (!crc_ok)
        return;
    int gi = scout_last_group_idx_for_slot(slot);
    if (gi < 0)
        return;

    avr_scout_group_t *G = &SC.groups[gi];
    G->le_seen = 1;
}

int avr_scout_get_mi_by_sf(uint8_t slot, uint32_t sf_idx, uint32_t *mi_out)
{
    if (slot > 1 || !mi_out)
        return -1;
    for (uint32_t i = 0; i < AVR_SCOUT_MAX_SF; ++i)
    {
        const scout_sf_entry_t *e = &SC.hist[slot][i];
        if (e->sf_idx == sf_idx)
        {
            *mi_out = e->mi32;
            return 0;
        }
    }
    return -1;
}