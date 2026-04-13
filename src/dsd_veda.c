#include "dsd_veda.h"

static int veda_get_live_ids(const dsd_state *state, int slot, uint32_t *id24_a, uint32_t *id24_b);
static void veda_refresh_profile_from_live_ids(dsd_state *state, int slot);


// Ротация вправо (из вашего реверса sub_8005DE4)
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// Основная перестановка VEDA-Permute-384 (sub_8005DE4)
void veda_permute_384(uint32_t *state, uint8_t domain) {
    ((uint8_t*)state)[47] ^= domain;
    for (int round = 24; round > 0; --round) {
        for (int i = 0; i < 4; ++i) {
            uint32_t v5 = state[i];
            uint32_t v6 = state[i + 8];
            uint32_t v7 = ROR32(state[i + 4], 23);
            state[i]     = v6 ^ v7 ^ (8 * (v7 & ROR32(v5, 8)));
            state[i + 8] = (2 * v6) ^ ROR32(v5, 8) ^ (4 * (v6 & v7));
            state[i + 4] = v7 ^ ROR32(v5, 8) ^ (2 * (v6 | ROR32(v5, 8)));
        }
        uint32_t mod4 = round & 3;
        if (mod4 == 1 || mod4 == 2) {
            uint32_t v8 = state[0], v9 = state[1], v10 = state[3];
            state[0] = state[2]; state[1] = v10; state[2] = v8; state[3] = v9;
        } else if (mod4 == 3) {
            uint32_t v11 = state[0], v12 = state[1], v13 = state[2], v14 = state[3];
            state[0] = v14; state[1] = v13; state[2] = v12; state[3] = v11;
        } else {
            uint32_t v11 = state[0], v12 = state[1];
            state[0] = v12 ^ (round | 0x9E377900);
            state[1] = v11;
            uint32_t tmp = state[2]; state[2] = state[3]; state[3] = tmp;
        }
    }
}

// Инициализация шифра сессионным ключом (аналог sub_8006372)
void veda_stream_init(dsd_state *state, int slot, uint8_t *session_key) {
    uint8_t *s8 = (uint8_t*)state->veda_crypto_state[slot];
    memset(state->veda_crypto_state[slot], 0, 48);
    memcpy(s8, "\x06sbx256\x08PrssEntr", 16);

    veda_permute_384(state->veda_crypto_state[slot], 1);
    for(int i=0; i<4; i++) state->veda_crypto_state[slot][i] ^= ((uint32_t*)session_key)[i];
    veda_permute_384(state->veda_crypto_state[slot], 253);
    for(int i=0; i<4; i++) state->veda_crypto_state[slot][i] ^= ((uint32_t*)session_key)[i+4];
    veda_permute_384(state->veda_crypto_state[slot], 253);
    
    state->veda_pos[slot] = 0;
    state->veda_stream_valid[slot] = 0;
}

// Применение Tweak (MI)
void veda_apply_mi(dsd_state *state, int slot, uint64_t mi) {
    state->veda_crypto_state[slot][0] ^= (uint32_t)(mi & 0xFFFFFFFF);
    state->veda_crypto_state[slot][1] ^= (uint32_t)(mi >> 32);
    veda_permute_384(state->veda_crypto_state[slot], 2);
    state->veda_pos[slot] = 0;
    state->veda_stream_valid[slot] = 1;
}


void handle_veda_kx_packet(dsd_opts *opts, dsd_state *state, uint8_t *payload) {
    hydro_kx_session_keypair kp; // Структура из твоего grep (содержит rx и tx)
    hydro_kx_keypair static_kp;  // Нам нужен объект для ключа

    // Инициализация библиотеки (обязательно один раз)
    static int hydro_ready = 0;
    if (!hydro_ready) {
        if (hydro_init() != 0) return;
        hydro_ready = 1;
    }

    // Готовим наш мастер-ключ. 
    // В протоколе "N" мастер-ключ из CPS используется и как PSK, и как Static Secret Key.
    memcpy(static_kp.sk, opts->veda_master_key, 32);
    // Генерируем публичную часть из секретной (нужно для библиотеки)
    hydro_kx_keygen_deterministic(&static_kp, opts->veda_master_key);

    // ВЫЗОВ ПРАВИЛЬНОЙ ФУНКЦИИ ИЗ ТВОЕГО GREP: hydro_kx_n_2
    // kp - куда писать ключи
    // payload - 48 байт из эфира
    // PSK - наш мастер ключ
    // &static_kp - наш объект ключа
    if (hydro_kx_n_2(&kp, payload, opts->veda_master_key, &static_kp) == 0) {
        int slot = state->currentslot & 1;
        
        // Копируем RX ключ (первые 32 байта из сессионной пары)
        memcpy(state->veda_session_key[slot], kp.rx, 32);
        
        /* аналог session_key_valid из дампа */
        state->veda_state_valid[slot] = 1;
        state->veda_stream_valid[slot] = 0;
        state->veda_mi_applied[slot] = 0;
        state->veda_last_applied_mi[slot] = 0;

        // veda_stream_init(state, slot, state->veda_session_key[slot]);
        
        if (opts->veda_debug) {
            fprintf(stderr, "\n[VEDA] Session Key Derived for Slot %d: ", slot + 1);
            for(int i=0; i<32; i++) fprintf(stderr, "%02X", kp.rx[i]);
            fprintf(stderr, "\n");
        }
    } else if (opts->veda_debug) {
        fprintf(stderr, "\n[VEDA] KX Handshake Failed (Bad Master Key or Packet)\n");
    }
}


// Единая функция получения бита гаммы с Feedback
// Единая функция получения бита гаммы с Feedback
static uint8_t veda_get_gamma_bit_with_feedback(dsd_state *state, int slot, uint8_t cipher_bit) {
    if (state->veda_pos[slot] >= 128) { // 16 байт
        veda_permute_384(state->veda_crypto_state[slot], 2);
        state->veda_pos[slot] = 0;
    }

    uint8_t *s8 = (uint8_t*)state->veda_crypto_state[slot];
    int byte_idx = state->veda_pos[slot] >> 3;
    int bit_idx = 7 - (state->veda_pos[slot] & 7); // MSB First

    uint8_t gamma_bit = (s8[byte_idx] >> bit_idx) & 1;
    uint8_t plain_bit = cipher_bit ^ gamma_bit;

    // ОБРАТНАЯ СВЯЗЬ (Feedback): 
    // В режиме дешифрования состояние обновляется ВХОДНЫМ битом (cipher_bit)
    if (cipher_bit) s8[byte_idx] |= (1 << bit_idx);
    else s8[byte_idx] &= ~(1 << bit_idx);

    state->veda_pos[slot]++;
    return plain_bit;
}

// Дешифрование ровно 72 бит фрейма (DMR TCH стандарт)
void veda_decrypt_ambe(dsd_state *state, int slot, char ambe_fr[4][24]) {
    if (!state->veda_stream_valid[slot]) return;    

    // Проходим по 72 битам (3 блока по 24 бита в матрице ambe_fr)
    // dsd-fme использует 4x24, но DMR Voice - это 72 бита. 
    // Обычно это ambe_fr[0], [1], [2].
    for (int i = 0; i < 3; i++) { // Используем 3 строки по 24 бита = 72
        for (int j = 0; j < 24; j++) {
            ambe_fr[i][j] = veda_get_gamma_bit_with_feedback(state, slot, ambe_fr[i][j] & 1);
        }
    }
}

void veda_prepare_voice_ctx(dsd_opts *opts, dsd_state *state, int slot, uint64_t mi)
{
    if (!opts || !state || slot < 0 || slot > 1)
        return;

    if (mi == 0)
        return;

    if (!state->veda_state_valid[slot])
        return;

    if (state->veda_stream_valid[slot] && state->veda_mi_applied[slot] &&
        state->veda_last_applied_mi[slot] == mi)
        return;

    /*
      Важно:
      tweak надо применять к свежему stream context,
      а не наслаивать поверх уже идущего keystream.
    */
    veda_stream_init(state, slot, state->veda_session_key[slot]);
    veda_apply_mi(state, slot, mi);

    state->veda_last_applied_mi[slot] = mi;
    state->veda_mi_applied[slot] = 1;

    if (opts->veda_debug)
    {
        fprintf(stderr,
                "\n[VEDA] MI APPLIED slot=%d mi=%016llX\n",
                slot + 1,
                (unsigned long long)mi);
    }
}

int veda_try_decrypt_voice_triplet(dsd_opts *opts,
                                   dsd_state *state,
                                   int slot,
                                   char ambe_fr[4][24],
                                   char ambe_fr2[4][24],
                                   char ambe_fr3[4][24])
{
    uint64_t eff_mi;

    if (!opts || !state || slot < 0 || slot > 1)
        return 0;

    if (!opts->isVEDA)
        return 0;

    if (!state->veda_state_valid[slot] && opts->veda_manual_set)
    {
        memcpy(state->veda_session_key[slot], opts->veda_manual_session_key, 32);
        state->veda_state_valid[slot] = 1;
        state->veda_stream_valid[slot] = 0;
        state->veda_mi_applied[slot] = 0;
        state->veda_last_applied_mi[slot] = 0;
    }

    eff_mi = veda_get_effective_mi(state, slot);

    if (!state->veda_state_valid[slot])
        veda_try_session_bridge(opts, state, slot);

    if (!(state->veda_state_valid[slot] && eff_mi != 0))
        return 0;

    veda_prepare_voice_ctx(opts, state, slot, eff_mi);

    veda_decrypt_ambe(state, slot, ambe_fr);
    veda_decrypt_ambe(state, slot, ambe_fr2);
    veda_decrypt_ambe(state, slot, ambe_fr3);

    return 1;
}

void veda_debug_voice_wait(dsd_opts *opts,
                           dsd_state *state,
                           int slot,
                           int sf_cur,
                           int sf_total)
{
    uint64_t eff_mi;
    uint64_t payload_mi;
    const char *reason;
    uint8_t reason_code;
    static int last_sf[2] = { -1, -1 };
    static uint8_t last_reason[2] = { 0, 0 };

    if (!opts || !state || slot < 0 || slot > 1)
        return;

    if (!opts->isVEDA || !opts->veda_debug)
        return;

    eff_mi = veda_get_effective_mi(state, slot);
    if (state->veda_state_valid[slot] && eff_mi != 0)
        return;

    payload_mi = (slot == 0) ? state->payload_mi : state->payload_miR;
    reason = state->veda_state_valid[slot] ? "WAIT_MI" : "WAIT_SESSION";
    reason_code = state->veda_state_valid[slot] ? 2 : 1;

    if (last_sf[slot] == sf_cur && last_reason[slot] == reason_code)
        return;

    last_sf[slot] = sf_cur;
    last_reason[slot] = reason_code;

    fprintf(stderr,
            "\n[VEDA] %s slot=%d payload_mi=%016llX eff_mi=%016llX\n",
            reason,
            slot + 1,
            (unsigned long long)payload_mi,
            (unsigned long long)eff_mi);

    veda_trace_baseline(opts, state, slot, "PREVOICE", sf_cur, sf_total);
}

uint64_t veda_get_effective_mi(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return 0;

    if (state->payload_mi != 0 && slot == 0)
        return state->payload_mi;

    if (state->payload_miR != 0 && slot == 1)
        return state->payload_miR;

    if (state->veda_vendor_mi_valid[slot])
    {
        uint64_t x = (uint64_t)state->veda_vendor_mi32[slot];
        return (x << 32) | x;   /* первая рабочая гипотеза */
    }

    return 0;
}


int veda_session_key_valid(const dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return 0;

    /*
      ВАЖНО:
      По дампу session_key_valid и stream-init логически разделены.
      В текущем dsd_new пока нет отдельного флага "session key present",
      поэтому на этом этапе используем текущий runtime-ready флаг.
      Это слой терминов, а не смена логики.
    */
    return state->veda_state_valid[slot] ? 1 : 0;
}

int veda_stream_ctx_valid(const dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return 0;

    return state->veda_stream_valid[slot] ? 1 : 0;
}

uint8_t *veda_session_material_ptr(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return NULL;

    return state->veda_session_key[slot];
}

const uint8_t *veda_session_material_cptr(const dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return NULL;

    return state->veda_session_key[slot];
}

void veda_trace_baseline(dsd_opts *opts,
                         dsd_state *state,
                         int slot,
                         const char *tag,
                         int sf_cur,
                         int sf_total)
{
    uint64_t eff_mi;
    uint32_t tg = 0;
    uint32_t src = 0;

    if (!opts || !state || slot < 0 || slot > 1)
        return;

    if (!opts->veda_debug)
        return;

    eff_mi = veda_get_effective_mi(state, slot);

    if (slot == 0)
    {
        tg  = (uint32_t)state->lasttg;
        src = (uint32_t)state->lastsrc;
    }
    else
    {
        tg  = (uint32_t)state->lasttgR;
        src = (uint32_t)state->lastsrcR;
    }

    fprintf(stderr,
        "\n[VEDA BASE] tag=%s slot=%d "
        "sf=%d/%d tg=%u src=%u "
        "sess_valid=%u stream_valid=%u kx_pos=%d "
        "vendor_mi_valid=%u vendor_mi32=%08X eff_mi=%016llX "
        "last_hdr_valid=%u last_hdr_src=%u "
        "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X "
        "sm=%u len_lo=%u len_hi=%u f9_count=%u\n",
        tag ? tag : "?",
        slot + 1,
        sf_cur,
        sf_total,
        tg,
        src,
        (unsigned)veda_session_key_valid(state, slot),
        (unsigned)veda_stream_ctx_valid(state, slot),
        state->veda_kx_pos[slot],
        (unsigned)state->veda_vendor_mi_valid[slot],
        (unsigned)state->veda_vendor_mi32[slot],
        (unsigned long long)eff_mi,
        (unsigned)state->veda_last_hdr_valid[slot],
        (unsigned)state->veda_last_hdr_src[slot],
        (unsigned)state->veda_last_b0[slot],
        (unsigned)state->veda_last_b1[slot],
        (unsigned)state->veda_last_w2[slot],
        (unsigned)state->veda_last_w4[slot],
        (unsigned)state->veda_last_w6[slot],
        (unsigned)state->veda_sm[slot],
        (unsigned)state->veda_len_lo[slot],
        (unsigned)state->veda_len_hi[slot],
        (unsigned)state->veda_f9_lc_count[slot]);
}


static const char *veda_candidate_source_name(uint8_t source_type)
{
    switch (source_type)
    {
    case VEDA_CAND_MBC05:  return "MBC05";
    case VEDA_CAND_VLC01:  return "VLC01";
    case VEDA_CAND_VC_EMB: return "VC_EMB";
    case VEDA_CAND_TLC02:  return "TLC02";
    case VEDA_CAND_TLC_F9: return "TLC_F9";
    default:               return "NONE";
    }
}

static int veda_mem_has_pattern(const uint8_t *buf, uint8_t buf_len,
                                const uint8_t *pat, uint8_t pat_len)
{
    uint8_t i;

    if (!buf || !pat || pat_len == 0 || buf_len < pat_len)
        return 0;

    for (i = 0; i <= (uint8_t)(buf_len - pat_len); i++)
    {
        if (memcmp(buf + i, pat, pat_len) == 0)
            return 1;
    }

    return 0;
}

static int veda_candidate_payload_eq(const veda_session_candidate_t *cand,
                                     const uint8_t *payload,
                                     uint8_t payload_len)
{
    if (!cand || !cand->valid || !payload)
        return 0;

    if (cand->payload_len != payload_len)
        return 0;

    return (memcmp(cand->raw_payload, payload, payload_len) == 0) ? 1 : 0;
}

static void veda_store_ref_candidate(veda_session_candidate_t *dst,
                                     uint8_t source_type,
                                     const uint8_t *payload,
                                     uint8_t payload_len,
                                     uint16_t seq_in_session,
                                     uint16_t timestamp_sf)
{
    if (!dst || !payload)
        return;

    memset(dst, 0, sizeof(*dst));
    dst->valid = 1;
    dst->source_type = source_type;
    dst->payload_len = payload_len;
    dst->seq_in_session = seq_in_session;
    dst->timestamp_sf = timestamp_sf;
    memcpy(dst->raw_payload, payload, payload_len);
}

static const char *veda_candidate_role_name(uint8_t source_type,
                                            int same_ref_mbc,
                                            int same_ref_vlc)
{
    switch (source_type)
    {
    case VEDA_CAND_MBC05:
        return "PREVOICE_MBC";

    case VEDA_CAND_VLC01:
        return "PREVOICE_VLC";

    case VEDA_CAND_VC_EMB:
        return same_ref_vlc ? "VOICE_MIRROR_VLC" : "VOICE_EMB_NEW";

    case VEDA_CAND_TLC_F9:
        return "TAIL_F9";

    case VEDA_CAND_TLC02:
        if (same_ref_vlc)
            return "TAIL_EQ_VLC";
        if (same_ref_mbc)
            return "TAIL_EQ_MBC";
        return "TAIL_TLC";

    default:
        return "OTHER";
    }
}

void veda_clear_candidate(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return;

    memset(&state->veda_candidate[slot], 0, sizeof(state->veda_candidate[slot]));
    memset(&state->veda_ref_mbc[slot], 0, sizeof(state->veda_ref_mbc[slot]));
    memset(&state->veda_ref_vlc[slot], 0, sizeof(state->veda_ref_vlc[slot]));
    memset(&state->veda_path[slot], 0, sizeof(state->veda_path[slot]));
}

static const char *veda_path_pattern_name(const veda_path_state_t *ps)
{
    if (!ps)
        return "NONE";

    if (ps->saw_mbc && ps->saw_vlc && ps->saw_voice && ps->tail_kind == 1)
        return "MBC_VLC_VOICE_TAIL_F9";

    if (ps->saw_mbc && ps->saw_vlc && ps->saw_voice && ps->tail_kind == 2)
        return "MBC_VLC_VOICE_TAIL_TLC";

    if (!ps->saw_mbc && ps->saw_vlc && ps->saw_voice && ps->tail_kind == 1)
        return "VLC_VOICE_TAIL_F9";

    if (!ps->saw_mbc && ps->saw_vlc && ps->saw_voice && ps->tail_kind == 2)
        return "VLC_VOICE_TAIL_TLC";

    if (ps->saw_mbc && ps->saw_vlc && ps->saw_voice)
        return "MBC_VLC_VOICE";

    if (!ps->saw_mbc && ps->saw_vlc && ps->saw_voice)
        return "VLC_VOICE";

    if (ps->saw_mbc && ps->saw_vlc)
        return "MBC_VLC";

    if (ps->saw_vlc)
        return "VLC_ONLY";

    if (ps->saw_mbc)
        return "MBC_ONLY";

    return "NONE";
}

static void veda_emit_path_summary(dsd_opts *opts,
                                   dsd_state *state,
                                   int slot,
                                   const char *reason)
{
    veda_path_state_t *ps;

    if (!opts || !state || slot < 0 || slot > 1)
        return;

    if (!opts->veda_debug)
        return;

    ps = &state->veda_path[slot];
    if (!ps->session_no)
        return;

    fprintf(stderr,
        "\n[VEDA PATH] slot=%d sess=%u reason=%s pattern=%s "
        "sf=%u..%u mbc_seq=%u vlc_seq=%u voice_seq=%u tail_seq=%u tail=%u "
        "db06=%u db07=%u mbc48=%u kx_try=%u\n",
        slot + 1,
        (unsigned)ps->session_no,
        reason ? reason : "none",
        veda_path_pattern_name(ps),
        (unsigned)ps->start_sf,
        (unsigned)ps->last_sf,
        (unsigned)ps->mbc_seq,
        (unsigned)ps->vlc_seq,
        (unsigned)ps->voice_seq,
        (unsigned)ps->tail_seq,
        (unsigned)ps->tail_kind,
        (unsigned)state->veda_seen_db06[slot],
        (unsigned)state->veda_seen_db07[slot],
        (unsigned)state->veda_seen_mbc48[slot],
        (unsigned)state->veda_kx_try_count[slot]);
}

static void veda_path_note_candidate(dsd_opts *opts,
                                     dsd_state *state,
                                     int slot,
                                     const veda_session_candidate_t *cand)
{
    veda_path_state_t *ps;

    if (!opts || !state || !cand || slot < 0 || slot > 1)
        return;

    ps = &state->veda_path[slot];

    if (!ps->active)
    {
        if (cand->source_type == VEDA_CAND_MBC05 ||
            cand->source_type == VEDA_CAND_VLC01)
        {
            memset(ps, 0, sizeof(*ps));
            ps->active = 1;
            ps->stage = VEDA_PATH_PREVOICE;
            
            if (state->veda_path_counter[slot] > 1000)
                state->veda_path_counter[slot] = 0;            
            state->veda_path_counter[slot]++;
            if (state->veda_path_counter[slot] == 0)
                state->veda_path_counter[slot] = 1;
            ps->session_no = state->veda_path_counter[slot];
            ps->start_sf = cand->timestamp_sf;
            ps->last_sf = cand->timestamp_sf;
        }
        else
        {
            return;
        }
    }

    ps->last_sf = cand->timestamp_sf;

    switch (cand->source_type)
    {
    case VEDA_CAND_MBC05:
        if (!ps->saw_mbc)
            ps->mbc_seq = cand->seq_in_session;
        ps->saw_mbc = 1;
        if (ps->stage < VEDA_PATH_PREVOICE)
            ps->stage = VEDA_PATH_PREVOICE;
        break;

    case VEDA_CAND_VLC01:
        if (!ps->saw_vlc)
            ps->vlc_seq = cand->seq_in_session;
        ps->saw_vlc = 1;
        if (ps->stage < VEDA_PATH_PREVOICE)
            ps->stage = VEDA_PATH_PREVOICE;
        break;

    case VEDA_CAND_VC_EMB:
        if (!ps->saw_voice)
        {
            ps->voice_seq = cand->seq_in_session;

            /*
              Для session summary хотим диапазон реального voice-сеанса,
              а не ранний prevoice sf=0 от VLC/MBC.
              Поэтому первый VC_EMB фиксирует старт voice-range.
            */
            if (cand->timestamp_sf != 0)
                ps->start_sf = cand->timestamp_sf;
        }
        ps->saw_voice = 1;
        ps->stage = VEDA_PATH_INVOICE;
        break;

    case VEDA_CAND_TLC_F9:
        ps->tail_kind = 1;
        ps->tail_seq = cand->seq_in_session;
        ps->stage = VEDA_PATH_TAIL;
        veda_emit_path_summary(opts, state, slot, "tail");
        ps->active = 0;
        break;

    case VEDA_CAND_TLC02:
        ps->tail_kind = 2;
        ps->tail_seq = cand->seq_in_session;
        ps->stage = VEDA_PATH_TAIL;
        veda_emit_path_summary(opts, state, slot, "tail");
        ps->active = 0;
        break;

    default:
        break;
    }
}

void veda_note_candidate(dsd_opts *opts,
                         dsd_state *state,
                         int slot,
                         uint8_t source_type,
                         const uint8_t *payload,
                         uint8_t payload_len,
                         int sf_cur)
{
    veda_session_candidate_t *cand;
    veda_session_candidate_t prev;
    static const uint8_t pat_sbx256[]   = {0x06, 0x73, 0x62, 0x78, 0x32, 0x35, 0x36, 0x08};
    static const uint8_t pat_prssentr[] = {'P','r','s','s','E','n','t','r'};
    uint8_t n;
    int i;
    int same_prev = 0;
    int has_sbx256 = 0;
    int has_prssentr = 0;
    int same_ref_mbc = 0;
    int same_ref_vlc = 0;
    const char *role = "OTHER";

    if (!opts || !state || !payload || slot < 0 || slot > 1)
        return;

    if (!opts->isVEDA)
        return;

    cand = &state->veda_candidate[slot];
    prev = *cand;

    n = payload_len;
    if (n > sizeof(cand->raw_payload))
        n = sizeof(cand->raw_payload);

    /* не спамим полностью одинаковым кандидатом подряд */
    if (cand->valid &&
        cand->source_type == source_type &&
        cand->payload_len == n &&
        memcmp(cand->raw_payload, payload, n) == 0)
    {
        return;
    }

    if (prev.valid &&
        prev.payload_len == n &&
        memcmp(prev.raw_payload, payload, n) == 0)
    {
        same_prev = 1;
    }

    has_sbx256 = veda_mem_has_pattern(payload, n, pat_sbx256, sizeof(pat_sbx256));
    has_prssentr = veda_mem_has_pattern(payload, n, pat_prssentr, sizeof(pat_prssentr));

    same_ref_mbc = veda_candidate_payload_eq(&state->veda_ref_mbc[slot], payload, n);
    same_ref_vlc = veda_candidate_payload_eq(&state->veda_ref_vlc[slot], payload, n);

    memset(cand, 0, sizeof(*cand));
    cand->valid = 1;
    cand->source_type = source_type;
    cand->payload_len = n;
    cand->timestamp_sf = (sf_cur < 0) ? 0 : (sf_cur > 0xFFFF ? 0xFFFF : (uint16_t)sf_cur);

    state->veda_candidate_seq[slot]++;
    if (state->veda_candidate_seq[slot] == 0)
        state->veda_candidate_seq[slot] = 1;

    cand->seq_in_session = state->veda_candidate_seq[slot];
    memcpy(cand->raw_payload, payload, n);

    /* запоминаем опорные объекты начала сеанса */
    if (source_type == VEDA_CAND_MBC05 && !state->veda_ref_mbc[slot].valid)
    {
        veda_store_ref_candidate(&state->veda_ref_mbc[slot],
                                 source_type,
                                 payload,
                                 n,
                                 cand->seq_in_session,
                                 cand->timestamp_sf);
    }

    if (source_type == VEDA_CAND_VLC01 && !state->veda_ref_vlc[slot].valid)
    {
        veda_store_ref_candidate(&state->veda_ref_vlc[slot],
                                 source_type,
                                 payload,
                                 n,
                                 cand->seq_in_session,
                                 cand->timestamp_sf);
    }

    role = veda_candidate_role_name(source_type, same_ref_mbc, same_ref_vlc);

    if (opts->veda_debug)
    {
        fprintf(stderr,
                "\n[VEDA CAND] slot=%d seq=%u src=%s sf=%u len=%u "
                "prev=%s same_prev=%u ref_mbc=%u ref_vlc=%u "
                "role=%s has_sbx256=%u has_prssentr=%u bytes=",
                slot + 1,
                (unsigned)cand->seq_in_session,
                veda_candidate_source_name(source_type),
                (unsigned)cand->timestamp_sf,
                (unsigned)cand->payload_len,
                prev.valid ? veda_candidate_source_name(prev.source_type) : "NONE",
                (unsigned)same_prev,
                (unsigned)same_ref_mbc,
                (unsigned)same_ref_vlc,
                role,
                (unsigned)has_sbx256,
                (unsigned)has_prssentr);

        for (i = 0; i < cand->payload_len; i++)
            fprintf(stderr, "%02X", cand->raw_payload[i]);

        fprintf(stderr, "\n");
    }
    veda_path_note_candidate(opts, state, slot, cand);
}

int veda_try_session_bridge(dsd_opts *opts, dsd_state *state, int slot)
{
    veda_path_state_t *ps;
    uint64_t eff_mi;
    const char *likely;

    if (!opts || !state || slot < 0 || slot > 1)
        return 0;

    if (!opts->isVEDA)
        return 0;

    if (state->veda_state_valid[slot])
        return 0;

    ps = &state->veda_path[slot];

    if (!ps->saw_voice && !state->veda_vendor_mi_valid[slot])
        return 0;

    if (state->veda_bridge_notice_done[slot])
        return 0;

    state->veda_bridge_notice_done[slot] = 1;
    state->veda_bridge_probe_count[slot]++;

    eff_mi = veda_get_effective_mi(state, slot);

    likely = "UNKNOWN_PREVOICE_PATH";

    if (ps->saw_voice &&
        state->veda_vendor_mi_valid[slot] &&
        state->veda_kx_try_count[slot] == 0 &&
        state->veda_seen_db03[slot] == 0 &&
        state->veda_seen_db04[slot] == 0 &&
        state->veda_seen_db06[slot] == 0 &&
        state->veda_seen_db07[slot] == 0 &&
        state->veda_seen_mbc48[slot] == 0 &&
        state->veda_svc_hdr_hits[slot] == 0)
    {
        if (state->veda_ref_mbc[slot].valid &&
            state->veda_ref_mbc[slot].payload_len == 12)
        {
            likely = "LIKELY_MIDCALL_OR_TRUNCATED_MBC_START";
        }
        else if (state->veda_ref_vlc[slot].valid &&
             !state->veda_ref_mbc[slot].valid)
        {
            likely = "LIKELY_MIDCALL_VLC_START";
        }
        else
        {
            likely = "UNOBSERVED_SERVICE_PATH";
        }
    }

    if (opts->veda_debug)
    {
        fprintf(stderr,
        "\n[VEDA CASE6 MISS] slot=%d sess=%u cause=NO_VISIBLE_CASE6_PATH "
        "likely=%s pattern=%s svc_hdr=%u reject=%u reject_svc=%u "
        "db06=%u db07=%u mbc48=%u kx_try=%u "
        "mi_valid=%u eff_mi=%016llX ref_mbc_len=%u ref_vlc_len=%u "
        "db03=%u db04=%u svc_db03=%u svc_db04=%u\n",
        slot + 1,
        (unsigned)ps->session_no,
        likely,
        veda_path_pattern_name(ps),
        (unsigned)state->veda_svc_hdr_hits[slot],
        (unsigned)state->veda_reject_probe_count[slot],
        (unsigned)state->veda_reject_svc_hits[slot],
        (unsigned)state->veda_seen_db06[slot],
        (unsigned)state->veda_seen_db07[slot],
        (unsigned)state->veda_seen_mbc48[slot],
        (unsigned)state->veda_kx_try_count[slot],
        (unsigned)state->veda_vendor_mi_valid[slot],
        (unsigned long long)eff_mi,
        (unsigned)state->veda_ref_mbc[slot].payload_len,
        (unsigned)state->veda_ref_vlc[slot].payload_len,
        (unsigned)state->veda_seen_db03[slot],
        (unsigned)state->veda_seen_db04[slot],
        (unsigned)state->veda_seen_svc_db03[slot],
        (unsigned)state->veda_seen_svc_db04[slot]);
    }

    return 0;
}
//======================================================================================
void veda_reset_slot(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return;

    state->veda_sm[slot] = 0;
    state->veda_len_lo[slot] = 0;
    state->veda_len_hi[slot] = 0;
    state->veda_last_sel[slot] = 0;
    state->veda_subst_active[slot] = 0;
    memset(state->veda_tx_buf[slot], 0, sizeof(state->veda_tx_buf[slot]));
    
    state->veda_raw_src_kind[slot] = VEDA_HDRSRC_NONE;

    state->veda_last_hdr_valid[slot] = 0;
    state->veda_last_hdr_src[slot]   = VEDA_HDRSRC_NONE;
    state->veda_last_b0[slot] = 0;
    state->veda_last_b1[slot] = 0;
    state->veda_last_w2[slot] = 0;
    state->veda_last_w4[slot] = 0;
    state->veda_last_w6[slot] = 0;

    state->veda_cmd0[slot] = 0;
    state->veda_cmd1[slot] = 0;  
    
    state->veda_last_applied_mi[slot] = 0;
    state->veda_mi_applied[slot] = 0;

    state->veda_vendor_mi32[slot] = 0;
    state->veda_vendor_mi_valid[slot] = 0;

    memset(state->veda_f9_lc_bytes[slot], 0, sizeof(state->veda_f9_lc_bytes[slot]));
    memset(state->veda_f9_lc_type[slot], 0, sizeof(state->veda_f9_lc_type[slot]));
    state->veda_f9_lc_count[slot] = 0;

    memset(state->veda_session_key[slot], 0, sizeof(state->veda_session_key[slot]));
    memset(state->veda_crypto_state[slot], 0, sizeof(state->veda_crypto_state[slot]));
    memset(state->veda_kx_buffer[slot], 0, sizeof(state->veda_kx_buffer[slot]));

    state->veda_state_valid[slot] = 0;
    state->veda_stream_valid[slot] = 0;
    state->veda_kx_pos[slot] = 0;
    state->veda_pos[slot] = 0;

    state->veda_seen_db06[slot] = 0;
    state->veda_seen_db07[slot] = 0;
    state->veda_seen_mbc48[slot] = 0;
    state->veda_kx_try_count[slot] = 0;    

    veda_clear_candidate(state, slot);
    state->veda_candidate_seq[slot] = 0;

    state->veda_stream_valid[slot] = 0;
    state->veda_pos[slot] = 0;   
    
    state->veda_bridge_notice_done[slot] = 0;
    state->veda_bridge_probe_count[slot] = 0;
    
    state->veda_svc_hdr_hits[slot] = 0;

    state->veda_reject_probe_count[slot] = 0;
    state->veda_reject_svc_hits[slot] = 0;    

    state->veda_seen_db03[slot] = 0;
    state->veda_seen_db04[slot] = 0;
    state->veda_seen_svc_db03[slot] = 0;
    state->veda_seen_svc_db04[slot] = 0;
    
    veda_raw_reset_slot(state, slot);
}

void veda_reset_profile(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return;

    state->veda_id24_a[slot] = 0;
    state->veda_id24_b[slot] = 0;
    state->veda_id24_valid[slot] = 0;
}

void veda_log_subst(dsd_state *state, int slot, int chng)
{
    if (!state || slot < 0 || slot > 1)
        return;

    switch (chng)
    {
    case 2:
        fprintf(stderr,
                "\nVEDA SM slot=%d sm=%u len_lo=%u len_hi=%u\n",
                slot + 1,
                state->veda_sm[slot],
                state->veda_len_lo[slot],
                state->veda_len_hi[slot]);
        break;

    case 3:
        fprintf(stderr,
                "\nVEDA IDS slot=%d id_a=0x%06X (%d) id_b=0x%06X (%d)\n",
                slot + 1,
                state->veda_id24_a[slot] & 0xFFFFFFu, state->veda_id24_a[slot] & 0xFFFFFFu,
                state->veda_id24_b[slot] & 0xFFFFFFu, state->veda_id24_b[slot] & 0xFFFFFFu
            );
        break;

    case 4:
        fprintf(stderr,
                "\nVEDA SUBST slot=%d sel=%u raw_src=%u raw_tgt=%u id24=0x%06X buf=%02X %02X %02X %02X %02X %02X\n",
                slot + 1,
                state->veda_last_sel[slot],
                state->veda_raw_src[slot],
                state->veda_raw_tgt[slot],
                veda_pick_profile_id24(state, slot, state->veda_last_sel[slot]),
                state->veda_tx_buf[slot][0],
                state->veda_tx_buf[slot][1],
                state->veda_tx_buf[slot][2],
                state->veda_tx_buf[slot][3],
                state->veda_tx_buf[slot][4],
                state->veda_tx_buf[slot][5]);
        break;

    case 5:
        fprintf(stderr,
                "\nVEDA WARN slot=%d subst-build-skipped sm=%u len_hi=%u\n",
                slot + 1,
                state->veda_sm[slot],
                state->veda_len_hi[slot]);
        break;
    }
}

static void veda_refresh_profile_from_live_ids(dsd_state *state, int slot)
{
    uint32_t id24_a = 0;
    uint32_t id24_b = 0;

    if (!state || slot < 0 || slot > 1)
        return;

    if (!veda_get_live_ids(state, slot, &id24_a, &id24_b))
        return;

    if (state->veda_id24_valid[slot] &&
        state->veda_id24_a[slot] == id24_a &&
        state->veda_id24_b[slot] == id24_b)
    {
        return;
    }

    veda_set_profile_ids(state, slot, id24_a, id24_b);
}

static int veda_src_prio(uint8_t src_kind)
{
    switch (src_kind)
    {
    case VEDA_HDRSRC_VLC:     return 60;
    case VEDA_HDRSRC_TLC:     return 30;
    case VEDA_HDRSRC_CSBK:    return 40;
    case VEDA_HDRSRC_DHEADER: return 20;
    case VEDA_HDRSRC_UDT:     return 10;
    default:                  return 0;
    }
}

void veda_note_raw_src_tgt_ex(dsd_state *state, int slot,
                              uint32_t source, uint32_t target,
                              uint8_t src_kind)
{
    uint32_t src24;
    uint32_t tgt24;

    if (!state || slot < 0 || slot > 1)
        return;

    src24 = source & 0xFFFFFFu;
    tgt24 = target & 0xFFFFFFu;

    if (src24 == 0 || tgt24 == 0)
        return;

    if (state->veda_raw_src[slot] == 0 ||
        state->veda_raw_tgt[slot] == 0 ||
        veda_src_prio(src_kind) >= veda_src_prio(state->veda_raw_src_kind[slot]))
    {
        state->veda_raw_src[slot]      = src24;
        state->veda_raw_tgt[slot]      = tgt24;
        state->veda_raw_src_kind[slot] = src_kind;
    }

    veda_refresh_profile_from_live_ids(state, slot);
}

void veda_note_raw_src_tgt(dsd_state *state, int slot, uint32_t source, uint32_t target)
{
    veda_note_raw_src_tgt_ex(state, slot, source, target, VEDA_HDRSRC_CSBK);
}

void veda_set_profile_ids(dsd_state *state, int slot, uint32_t id24_a, uint32_t id24_b)
{
    if (!state || slot < 0 || slot > 1)
        return;

    state->veda_id24_a[slot] = id24_a & 0xFFFFFFu;
    state->veda_id24_b[slot] = id24_b & 0xFFFFFFu;
    state->veda_id24_valid[slot] = 1;

    if (state->veda_debug)
        veda_log_subst(state, slot, 3);
}

static int veda_can_try_normalized_b0(uint8_t src_kind, const veda_air_header_t *hdr)
{
    if (!hdr)
        return 0;

    if (src_kind == VEDA_HDRSRC_CSBK)
        return 0;

    if (hdr->b1 < 1 || hdr->b1 > 3)
        return 0;

    if (hdr->w2 == 0 && hdr->w4 == 0 && hdr->w6 == 0)
        return 0;

    return 1;
}

static void veda_store_last_hdr(dsd_state *state, int slot, const veda_air_header_t *hdr, uint8_t src_kind)
{
    if (!state || !hdr || slot < 0 || slot > 1)
        return;

    state->veda_last_hdr_valid[slot] = 1;
    state->veda_last_hdr_src[slot]   = src_kind;
    state->veda_last_b0[slot]        = hdr->b0;
    state->veda_last_b1[slot]        = hdr->b1;
    state->veda_last_w2[slot]        = hdr->w2;
    state->veda_last_w4[slot]        = hdr->w4;
    state->veda_last_w6[slot]        = hdr->w6;

    state->veda_cmd0[slot] = hdr->b0;
    state->veda_cmd1[slot] = hdr->b1;
}

int veda_try_handle_header(dsd_opts *opts, dsd_state *state, int slot,
                           const veda_air_header_t *hdr,
                           uint8_t src_kind)
{
    int rc;
    int can_norm = 0;
    int norm_attempted = 0;
    int norm_changed = 0;
    int norm_rc = 0;
    veda_air_header_t norm;

    if (!opts || !state || !hdr || slot < 0 || slot > 1)
        return -1;

    if (!opts->isVEDA)
        return 0;

    uint8_t svc_class = (((hdr->b0 & 0x60) == 0x20) ? 1 : 0);

    if (svc_class)
        state->veda_svc_hdr_hits[slot]++;        

    if (state->veda_debug)
    {
    fprintf(stderr,
            "\n[VEDA HTRY] slot=%d src=%u svc=%u "
            "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X "
            "sm=%u len_lo=%u len_hi=%u raw_src=%u raw_tgt=%u\n",
            slot + 1,
            (unsigned)src_kind,
            (unsigned)svc_class,
            (unsigned)hdr->b0,
            (unsigned)hdr->b1,
            (unsigned)hdr->w2,
            (unsigned)hdr->w4,
            (unsigned)hdr->w6,
            (unsigned)state->veda_sm[slot],
            (unsigned)state->veda_len_lo[slot],
            (unsigned)state->veda_len_hi[slot],
            (unsigned)state->veda_raw_src[slot],
            (unsigned)state->veda_raw_tgt[slot]);
    }

    veda_store_last_hdr(state, slot, hdr, src_kind);

    rc = veda_control_header_handler(opts, state, slot, hdr);

    can_norm = veda_can_try_normalized_b0(src_kind, hdr);

    if (rc == 0 && can_norm)
    {
        norm = *hdr;
        norm.b0 = (uint8_t)((norm.b0 & 0x9Fu) | 0x20u);
        norm_changed = (norm.b0 != hdr->b0);

        if (norm_changed)
        {
            norm_attempted = 1;
            norm_rc = veda_control_header_handler(opts, state, slot, &norm);

            if (norm_rc != 0)
            {
                veda_store_last_hdr(state, slot, &norm, src_kind);
                rc = norm_rc;

                if (state->veda_debug)
                {
                    fprintf(stderr,
                            "\nVEDA HDR slot=%d src=%u rc=%d norm=1 "
                            "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X\n",
                            slot + 1, src_kind, rc,
                            norm.b0, norm.b1, norm.w2, norm.w4, norm.w6);
                }

                return rc;
            }
        }
    }

    if (state->veda_debug)
    {
        if (rc != 0)
        {
            fprintf(stderr,
                    "\nVEDA HDR slot=%d src=%u rc=%d norm=0 "
                    "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X\n",
                    slot + 1, src_kind, rc,
                    hdr->b0, hdr->b1, hdr->w2, hdr->w4, hdr->w6);
        }
        else
        {
            fprintf(stderr,
                    "\nVEDA HDR MISS slot=%d src=%u "
                    "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X "
                    "can_norm=%d norm_attempted=%d norm_changed=%d norm_rc=%d "
                    "sm=%u len_lo=%u len_hi=%u raw_src=%u raw_tgt=%u\n",
                    slot + 1, src_kind,
                    hdr->b0, hdr->b1, hdr->w2, hdr->w4, hdr->w6,
                    can_norm, norm_attempted, norm_changed, norm_rc,
                    state->veda_sm[slot],
                    state->veda_len_lo[slot],
                    state->veda_len_hi[slot],
                    state->veda_raw_src[slot],
                    state->veda_raw_tgt[slot]);
        }
    }

    return rc;
}

int veda_try_build_tx_subst_frame(dsd_state *state, int slot)
{
    uint32_t id24;
    uint8_t *buf;

    if (!state || slot < 0 || slot > 1)
        return 0;
    
    veda_refresh_profile_from_live_ids(state, slot); 

    buf = state->veda_tx_buf[slot];

    state->veda_subst_active[slot] = 0;
    memset(buf, 0, 6);

    if (!state->veda_id24_valid[slot])
        return 0;

    if (state->veda_sm[slot] != 4 && state->veda_sm[slot] != 3)
        return 0;

    if (state->veda_len_hi[slot] == 0)
        return 0;

    id24 = veda_pick_profile_id24(state, slot, state->veda_last_sel[slot]);
    veda_pack_selected_id24(buf, id24);

    state->veda_subst_active[slot] = 1;

    if (state->veda_debug)
        veda_log_subst(state, slot, 4);

    return 1;
}

int veda_control_header_handler(dsd_opts *opts, dsd_state *state, int slot, const veda_air_header_t *hdr)
{
    UNUSED(opts);

    if (!state || !hdr || slot < 0 || slot > 1)
        return -1;

    if ((hdr->b0 & 0x60) != 0x20)
        return 0;

    switch (hdr->b1)
    {
    case 1:
        if (hdr->w6 == 0)
        {
            if (state->veda_sm[slot] == 2 || state->veda_sm[slot] == 5)
            {
                state->veda_sm[slot] = 6;
                state->veda_subst_active[slot] = 0;
                memset(state->veda_tx_buf[slot], 0, 6);

                if (state->veda_debug)
                    veda_log_subst(state, slot, 2);

                return 1;
            }
            return -1;
        }

        if (state->veda_sm[slot] != 2 && state->veda_sm[slot] != 5)
            return -1;

        state->veda_len_lo[slot] = hdr->w2;
        state->veda_len_hi[slot] = hdr->w6;
        state->veda_sm[slot] = 3;

        if (state->veda_debug)
            veda_log_subst(state, slot, 2);

        return 1;

    case 2:
        if (hdr->w6 == 0)
        {
            state->veda_sm[slot] = 2;
            state->veda_len_lo[slot] = 0;
            state->veda_len_hi[slot] = 0;
            state->veda_subst_active[slot] = 0;

            if (state->veda_debug)
                veda_log_subst(state, slot, 2);

            return 1;
        }

        if (state->veda_sm[slot] != 2 && state->veda_sm[slot] != 9)
            return -1;

        state->veda_len_lo[slot] = hdr->w2;
        state->veda_len_hi[slot] = hdr->w6;

        if (hdr->w2 != 0)
            state->veda_sm[slot] = 9;
        else
            state->veda_sm[slot] = 2;

        if (state->veda_debug)
            veda_log_subst(state, slot, 2);

        return 1;

    case 3:
        if (state->veda_sm[slot] == 3)
        {
            if (state->veda_len_hi[slot] == 0)
            {
                state->veda_sm[slot] = 5;

                if (state->veda_debug)
                    veda_log_subst(state, slot, 2);

                return 1;
            }

            state->veda_last_sel[slot] = (state->veda_len_lo[slot] == 0) ? 1 : 0;
            state->veda_sm[slot] = 4;

            if (state->veda_debug)
                veda_log_subst(state, slot, 2);

fprintf(stderr,
  "\nVEDA CMP slot=%d raw_src=%u raw_tgt=%u id_a=%u id_b=%u sel=%u subst=%u\n",
  slot + 1,
  state->veda_raw_src[slot],
  state->veda_raw_tgt[slot],
  state->veda_id24_a[slot],
  state->veda_id24_b[slot],
  state->veda_last_sel[slot],
  state->veda_subst_active[slot]);                

            if (veda_try_build_tx_subst_frame(state, slot))
                return 2;

            if (state->veda_debug)
                veda_log_subst(state, slot, 5);

            return -1;
        }

        if (state->veda_sm[slot] == 6)
        {
            state->veda_sm[slot] = 7;

            if (state->veda_debug)
                veda_log_subst(state, slot, 2);

            return 1;
        }

        return 0;

    default:
        return 0;
    }
}

void veda_dump_state(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return;

    fprintf(stderr,
            "\nVEDA STATE slot=%d sm=%u len_lo=%u len_hi=%u raw_src=%u raw_tgt=%u id_a=0x%06X id_b=0x%06X subst=%u\n",
            slot + 1,
            state->veda_sm[slot],
            state->veda_len_lo[slot],
            state->veda_len_hi[slot],
            state->veda_raw_src[slot],
            state->veda_raw_tgt[slot],
            state->veda_id24_a[slot] & 0xFFFFFFu,
            state->veda_id24_b[slot] & 0xFFFFFFu,
            state->veda_subst_active[slot]);
}

static int veda_get_live_ids(const dsd_state *state, int slot, uint32_t *id24_a, uint32_t *id24_b)
{
    uint32_t src = 0;
    uint32_t tgt = 0;

    if (!state || !id24_a || !id24_b || slot < 0 || slot > 1)
        return 0;

    /* 1) Самый приоритетный источник — уже собранная VEDA raw-карта */
    src = state->veda_raw_src[slot] & 0xFFFFFFu;
    tgt = state->veda_raw_tgt[slot] & 0xFFFFFFu;

    /* 2) Фолбэк на обычные оперативные lastsrc/lasttg */
    if (src == 0 || tgt == 0)
    {
        if (slot == 0)
        {
            src = ((uint32_t)state->lastsrc) & 0xFFFFFFu;
            tgt = ((uint32_t)state->lasttg)  & 0xFFFFFFu;
        }
        else
        {
            src = ((uint32_t)state->lastsrcR) & 0xFFFFFFu;
            tgt = ((uint32_t)state->lasttgR)  & 0xFFFFFFu;
        }
    }

    /* 3) Фолбэк на data/PDU адреса */
    if (src == 0 || tgt == 0)
    {
        src = (uint32_t)(state->dmr_lrrp_source[slot] & 0xFFFFFFu);
        tgt = (uint32_t)(state->dmr_lrrp_target[slot] & 0xFFFFFFu);
    }

    if (src == 0 || tgt == 0)
        return 0;

    /*
      Храним как абстрактную пару a/b.
      Пока не доказано жёстко, что +28=Target, +32=Source или наоборот.
      Но для практики берём:
        a = target
        b = source
      Если окажется наоборот — меняется только это место.
    */
    *id24_a = tgt & 0xFFFFFFu;
    *id24_b = src & 0xFFFFFFu;
    return 1;
}

void veda_trace_probe_air_header(dsd_opts *opts,
                                 dsd_state *state,
                                 int slot,
                                 const uint8_t *buf,
                                 uint8_t len,
                                 const char *tag,
                                 int sf_cur)
{
    uint8_t b0, b1;
    uint16_t w2, w4, w6;
    uint8_t svc;

    if (!opts || !state || !buf || slot < 0 || slot > 1)
        return;

    if (!opts->isVEDA || !opts->veda_debug)
        return;

    if (len < 8)
    {
        fprintf(stderr,
                "\n[VEDA MBCH] slot=%d tag=%s sf=%d len=%u short\n",
                slot + 1,
                tag ? tag : "MBC",
                sf_cur,
                (unsigned)len);
        return;
    }

    b0 = buf[0];
    b1 = buf[1];
    w2 = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    w4 = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    w6 = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    svc = (((b0 & 0x60) == 0x20) ? 1 : 0);

    fprintf(stderr,
            "\n[VEDA MBCH] slot=%d tag=%s sf=%d len=%u svc=%u "
            "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X raw=",
            slot + 1,
            tag ? tag : "MBC",
            sf_cur,
            (unsigned)len,
            (unsigned)svc,
            (unsigned)b0,
            (unsigned)b1,
            (unsigned)w2,
            (unsigned)w4,
            (unsigned)w6);

    for (int i = 0; i < len && i < 12; i++)
        fprintf(stderr, "%02X", buf[i]);

    fprintf(stderr, "\n");
}


void veda_trace_rejected_air_header(dsd_opts *opts,
                                    dsd_state *state,
                                    int slot,
                                    uint8_t databurst,
                                    const uint8_t *buf,
                                    uint8_t len,
                                    uint32_t crc_ok,
                                    uint32_t irr_err,
                                    int sf_cur)
{
    uint8_t b0 = 0, b1 = 0, svc = 0;
    uint16_t w2 = 0, w4 = 0, w6 = 0;
    int i;

    if (!opts || !state || !buf || slot < 0 || slot > 1)
        return;

    if (!opts->isVEDA || !opts->veda_debug)
        return;

    /*
      Нужны только реально подозрительные control/service burst-ы.
      Voice EMB сюда пока специально не тащим.
    */
    switch (databurst)
    {
    case 0x01: /* VLC */
    case 0x02: /* TLC */
    case 0x03: /* CSBK */
    case 0x04: /* MBCH */
    case 0x06: /* DATA HDR */
    case 0x07: /* 1/2 DATA */
        break;
    default:
        return;
    }

    /* интересуют только отброшенные/битые */
    if (crc_ok && irr_err == 0)
        return;

    /* не спамим бесконечно */
    if (state->veda_reject_probe_count[slot] >= 24)
        return;

    state->veda_reject_probe_count[slot]++;

    if (len >= 1)
        b0 = buf[0];
    if (len >= 2)
        b1 = buf[1];
    if (len >= 8)
    {
        w2 = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
        w4 = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
        w6 = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    }

    svc = (((b0 & 0x60) == 0x20) ? 1 : 0);
    if (svc)
        state->veda_reject_svc_hits[slot]++;

    fprintf(stderr,
            "\n[VEDA RXDROP] slot=%d sf=%d db=0x%02X crc=%u irr=%u len=%u svc=%u "
            "b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X raw=",
            slot + 1,
            sf_cur,
            (unsigned)databurst,
            (unsigned)crc_ok,
            (unsigned)irr_err,
            (unsigned)len,
            (unsigned)svc,
            (unsigned)b0,
            (unsigned)b1,
            (unsigned)w2,
            (unsigned)w4,
            (unsigned)w6);

    for (i = 0; i < len && i < 12; i++)
        fprintf(stderr, "%02X", buf[i]);

    fprintf(stderr, "\n");
}


static const char *veda_raw_kind_name(uint8_t kind)
{
    switch (kind)
    {
    case VEDA_RAW_MBC_BLK0: return "MBC_BLK0";
    case VEDA_RAW_MBC_SF:   return "MBC_SF";
    case VEDA_RAW_DB:       return "DB";
    case VEDA_RAW_VLC:      return "VLC";
    case VEDA_RAW_TLC:      return "TLC";
    case VEDA_RAW_EMB:      return "EMB";
    case VEDA_RAW_CACH:     return "CACH";
    case VEDA_RAW_MI:       return "MI";
    default:                return "NONE";
    }
}

static int veda_raw_can_open(uint8_t kind)
{
    return (kind == VEDA_RAW_MBC_BLK0 ||
            kind == VEDA_RAW_MBC_SF   ||
            kind == VEDA_RAW_VLC);
}

static void veda_raw_debug_dump(dsd_opts *opts,
                                dsd_state *state,
                                int slot,
                                const veda_raw_evt_t *evt)
{
    int i;

    if (!opts || !state || !evt || slot < 0 || slot > 1)
        return;

    if (!opts->isVEDA || !opts->veda_debug)
        return;

    fprintf(stderr,
            "\n[VEDA RAW] slot=%d sess=%u seq=%u kind=%s sf=%u len=%u "
            "crc=%u irr=%u db=%02X aux=%02X %02X %02X %02X raw=",
            slot + 1,
            (unsigned)evt->session_no,
            (unsigned)evt->seq,
            veda_raw_kind_name(evt->kind),
            (unsigned)evt->sf,
            (unsigned)evt->len,
            (unsigned)evt->crc_ok,
            (unsigned)evt->irr_err,
            (unsigned)evt->databurst,
            (unsigned)evt->aux0,
            (unsigned)evt->aux1,
            (unsigned)evt->aux2,
            (unsigned)evt->aux3);

    for (i = 0; i < evt->len; i++)
        fprintf(stderr, "%02X", evt->raw[i]);

    fprintf(stderr, "\n");
}

static void veda_raw_push_evt(dsd_opts *opts,
                              dsd_state *state,
                              int slot,
                              const veda_raw_evt_t *src)
{
    veda_raw_evt_t *dst;
    uint16_t idx;

    if (!state || !src || slot < 0 || slot > 1)
        return;

    if (!state->veda_raw_active[slot])
        return;

    if (state->veda_raw_count[slot] >= VEDA_RAW_MAX_EVTS)
        return;

    idx = state->veda_raw_count[slot]++;
    dst = &state->veda_raw_evt[slot][idx];
    *dst = *src;

    veda_raw_debug_dump(opts, state, slot, dst);
}

void veda_raw_reset_slot(dsd_state *state, int slot)
{
    if (!state || slot < 0 || slot > 1)
        return;

    state->veda_raw_active[slot] = 0;
    state->veda_raw_session_no[slot] = 0;
    state->veda_raw_start_sf[slot] = 0;
    state->veda_raw_first_voice_sf[slot] = 0;
    state->veda_raw_capture_until_sf[slot] = 0;
    state->veda_raw_count[slot] = 0;
    state->veda_raw_seq[slot] = 0;

    memset(state->veda_raw_evt[slot], 0, sizeof(state->veda_raw_evt[slot]));
}

void veda_raw_begin_if_needed(dsd_state *state, int slot, uint16_t sf, uint8_t first_kind)
{
    if (!state || slot < 0 || slot > 1)
        return;

    if (state->veda_raw_active[slot])
        return;

    if (!veda_raw_can_open(first_kind))
        return;

    state->veda_raw_session_no[slot]++;
    if (state->veda_raw_session_no[slot] == 0)
        state->veda_raw_session_no[slot] = 1;

    state->veda_raw_active[slot] = 1;
    state->veda_raw_start_sf[slot] = sf;
    state->veda_raw_first_voice_sf[slot] = 0;
    state->veda_raw_capture_until_sf[slot] = (uint16_t)(sf + VEDA_RAW_SF_WINDOW);
    state->veda_raw_count[slot] = 0;
    state->veda_raw_seq[slot] = 0;

    memset(state->veda_raw_evt[slot], 0, sizeof(state->veda_raw_evt[slot]));
}

void veda_raw_close_if_needed(dsd_state *state, int slot, uint16_t sf, uint8_t reason)
{
    (void)sf;
    (void)reason;

    if (!state || slot < 0 || slot > 1)
        return;

    if (!state->veda_raw_active[slot])
        return;

    state->veda_raw_active[slot] = 0;
}

static int veda_raw_prepare_event(dsd_state *state,
                                  int slot,
                                  uint8_t kind,
                                  uint16_t sf,
                                  veda_raw_evt_t *evt)
{
    if (!state || !evt || slot < 0 || slot > 1)
        return 0;

    if (state->veda_raw_active[slot] &&
        sf > state->veda_raw_capture_until_sf[slot])
    {
        veda_raw_close_if_needed(state, slot, sf, 1);
    }

    if (!state->veda_raw_active[slot])
    {
        veda_raw_begin_if_needed(state, slot, sf, kind);
    }

    if (!state->veda_raw_active[slot])
        return 0;

    memset(evt, 0, sizeof(*evt));

    state->veda_raw_seq[slot]++;
    if (state->veda_raw_seq[slot] == 0)
        state->veda_raw_seq[slot] = 1;

    evt->kind = kind;
    evt->slot = (uint8_t)slot;
    evt->sf = sf;
    evt->session_no = state->veda_raw_session_no[slot];
    evt->seq = state->veda_raw_seq[slot];

    return 1;
}

void veda_raw_log_mbc(dsd_opts *opts, dsd_state *state, int slot,
                      uint8_t kind, uint8_t databurst,
                      const uint8_t *raw, uint8_t len,
                      uint8_t crc_ok, uint8_t irr_err,
                      uint8_t blockcounter, uint8_t blocks,
                      uint8_t lb, uint8_t pf, uint16_t sf)
{
    veda_raw_evt_t evt;

    if (!raw || len == 0)
        return;

    if (!veda_raw_prepare_event(state, slot, kind, sf, &evt))
        return;

    evt.databurst = databurst;
    evt.crc_ok = crc_ok;
    evt.irr_err = irr_err;
    evt.len = (len > VEDA_RAW_MAX_BYTES) ? VEDA_RAW_MAX_BYTES : len;
    evt.aux0 = blockcounter;
    evt.aux1 = blocks;
    evt.aux2 = lb;
    evt.aux3 = pf;

    memcpy(evt.raw, raw, evt.len);

    veda_raw_push_evt(opts, state, slot, &evt);
}

void veda_raw_log_db(dsd_opts *opts, dsd_state *state, int slot,
                     uint8_t databurst,
                     const uint8_t *raw, uint8_t len,
                     uint8_t crc_ok, uint8_t irr_err,
                     uint16_t sf)
{
    veda_raw_evt_t evt;

    if (!raw || len == 0)
        return;

    if (!veda_raw_prepare_event(state, slot, VEDA_RAW_DB, sf, &evt))
        return;

    evt.databurst = databurst;
    evt.crc_ok = crc_ok;
    evt.irr_err = irr_err;
    evt.len = (len > VEDA_RAW_MAX_BYTES) ? VEDA_RAW_MAX_BYTES : len;

    memcpy(evt.raw, raw, evt.len);

    veda_raw_push_evt(opts, state, slot, &evt);
}

void veda_raw_log_lc(dsd_opts *opts, dsd_state *state, int slot,
                     uint8_t kind,
                     const uint8_t *raw, uint8_t len,
                     uint8_t crc_ok, uint8_t irr_err,
                     uint8_t flco, uint8_t fid, uint8_t so,
                     uint16_t sf)
{
    veda_raw_evt_t evt;

    if (!raw || len == 0)
        return;

    if (!veda_raw_prepare_event(state, slot, kind, sf, &evt))
        return;

    if (kind == VEDA_RAW_VLC) evt.databurst = 0x01;
    else if (kind == VEDA_RAW_TLC) evt.databurst = 0x02;
    else if (kind == VEDA_RAW_EMB) evt.databurst = 0xEB;
    else evt.databurst = 0x00;

    evt.crc_ok = crc_ok;
    evt.irr_err = irr_err;
    evt.len = (len > VEDA_RAW_MAX_BYTES) ? VEDA_RAW_MAX_BYTES : len;
    evt.aux0 = flco;
    evt.aux1 = fid;
    evt.aux2 = so;

    memcpy(evt.raw, raw, evt.len);

    if ((kind == VEDA_RAW_VLC || kind == VEDA_RAW_EMB) &&
        state->veda_raw_first_voice_sf[slot] == 0)
    {
        state->veda_raw_first_voice_sf[slot] = sf;
    }

    veda_raw_push_evt(opts, state, slot, &evt);
}

void veda_raw_log_cach(dsd_opts *opts, dsd_state *state, int slot,
                       uint32_t tact_raw,
                       uint8_t tact_ok, uint8_t at, uint8_t tdma_slot, uint8_t lcss,
                       const uint8_t *raw, uint8_t len,
                       uint16_t sf)
{
    veda_raw_evt_t evt;

    if (!raw || len == 0)
        return;

    if (!veda_raw_prepare_event(state, slot, VEDA_RAW_CACH, sf, &evt))
        return;

    evt.crc_ok = tact_ok;
    evt.len = (len > VEDA_RAW_MAX_BYTES) ? VEDA_RAW_MAX_BYTES : len;
    evt.aux0 = tact_ok;
    evt.aux1 = at;
    evt.aux2 = tdma_slot;
    evt.aux3 = lcss;
    evt.tact_raw = tact_raw;

    memcpy(evt.raw, raw, evt.len);

    veda_raw_push_evt(opts, state, slot, &evt);
}

void veda_raw_log_mi(dsd_opts *opts, dsd_state *state, int slot,
                     uint32_t mi32, uint16_t sf)
{
    veda_raw_evt_t evt;

    if (!veda_raw_prepare_event(state, slot, VEDA_RAW_MI, sf, &evt))
        return;

    evt.len = 4;
    evt.raw[0] = (uint8_t)((mi32 >> 24) & 0xFF);
    evt.raw[1] = (uint8_t)((mi32 >> 16) & 0xFF);
    evt.raw[2] = (uint8_t)((mi32 >> 8) & 0xFF);
    evt.raw[3] = (uint8_t)(mi32 & 0xFF);

    veda_raw_push_evt(opts, state, slot, &evt);
}