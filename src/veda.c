/*
 * veda.c - VEDA MS-only experimental hypothesis runner for dsd_new.
 *
 * This file is intentionally separated from dsd_veda.c.
 * The old dsd_veda.c/h can keep existing collectors and experimental logic.
 * This file is for the new, cleaner MS-only path.
 *
 * Current status:
 *   - collect mode is implemented and does not modify AMBE frames;
 *   - KX/split, sbx256/Gimli and case8 branches are skeleton/model APIs;
 *   - no --veda-key32 shortcut is implemented here;
 *   - key32 must be produced by a hypothesis/builder.
 */

#include "veda.h"
#include "dmr_const.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define VEDA_DEBUG_KEYSTREAM 1

static veda_context_t g_veda_ctx;

static int veda_str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return 0;
    while (*a != '\0' && *b != '\0')
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void veda_hexdump_stderr(const char *tag, const uint8_t *buf, int len)
{
    int i;
    if (tag != NULL)
        fprintf(stderr, "%s", tag);
    if (buf == NULL || len <= 0)
    {
        fprintf(stderr, "<empty>");
        return;
    }
    for (i = 0; i < len; i++)
        fprintf(stderr, "%02X", buf[i]);
}

static void veda_copy_field(uint8_t *dst, int *dst_len, int *dst_valid,
                            const uint8_t *src, int len)
{
    int n;
    if (dst == NULL || dst_len == NULL || dst_valid == NULL)
        return;

    memset(dst, 0, VEDA_MAX_FIELD_BYTES);
    *dst_len = 0;
    *dst_valid = 0;

    if (src == NULL || len <= 0)
        return;

    n = len;
    if (n > VEDA_MAX_FIELD_BYTES)
        n = VEDA_MAX_FIELD_BYTES;

    memcpy(dst, src, (size_t)n);
    *dst_len = n;
    *dst_valid = 1;
}

static int veda_ambe_count_bits(char fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS])
{
    int r, c, n = 0;
    if (fr == NULL)
        return 0;
    for (r = 0; r < VEDA_AMBE_ROWS; r++)
    {
        for (c = 0; c < VEDA_AMBE_COLS; c++)
        {
            if (fr[r][c] & 1)
                n++;
        }
    }
    return n;
}

veda_context_t *veda_get_context(void)
{
    return &g_veda_ctx;
}

void veda_init_context(veda_context_t *v)
{
    if (v == NULL)
        return;
    memset(v, 0, sizeof(*v));
    v->hypothesis = VEDA_HYP_COLLECT;
    v->ms_only = 1;
}

void veda_reset_global_context(void)
{
    veda_init_context(&g_veda_ctx);
}

static void veda_store_le32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 0);
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static uint32_t veda_rotl32(uint32_t x, int r)
{
    return (x << r) | (x >> (32 - r));
}

static void veda_gimli384_permute_model(uint32_t s[12])
{
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t t;
    int round;
    int col;

    for (round = 24; round > 0; round--)
    {
        for (col = 0; col < 4; col++)
        {
            x = veda_rotl32(s[col], 24);
            y = veda_rotl32(s[col + 4], 9);
            z = s[col + 8];

            s[col + 8] = x ^ (z << 1) ^ ((y & z) << 2);
            s[col + 4] = y ^ x ^ ((x | z) << 1);
            s[col] = z ^ y ^ ((x & y) << 3);
        }

        if ((round & 3) == 0)
        {
            t = s[0];
            s[0] = s[1];
            s[1] = t;

            t = s[2];
            s[2] = s[3];
            s[3] = t;

            s[0] ^= (0x9E377900u | (uint32_t)round);
        }
        else if ((round & 3) == 2)
        {
            t = s[0];
            s[0] = s[2];
            s[2] = t;

            t = s[1];
            s[1] = s[3];
            s[3] = t;
        }
    }
}

const char *veda_hypothesis_name(veda_hypothesis_t h)
{
    switch (h)
    {
    case VEDA_HYP_COLLECT:
        return "collect";
    case VEDA_HYP_MAIN_TREE:
        return "main-tree";
    case VEDA_HYP_RUNTIME_BRIDGE:
        return "runtime-bridge";
    case VEDA_HYP_CASE7_SERVICE:
        return "case7-service";
    case VEDA_HYP_CASE8_PROOF:
        return "case8-proof";
    case VEDA_HYP_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

static const char *veda_key32_source_name(veda_key32_source_t s)
{
    switch (s)
    {
    case VEDA_KEY32_SRC_NONE:
        return "none";
    case VEDA_KEY32_SRC_OUT0:
        return "out0";
    case VEDA_KEY32_SRC_OUT1:
        return "out1";
    case VEDA_KEY32_SRC_RUNTIME_BRIDGE:
        return "runtime-bridge";
    case VEDA_KEY32_SRC_CASE8_SIDE_PROOF:
        return "case8-side-proof";
    case VEDA_KEY32_SRC_TEMP_CPS_CAND:
        return "temp-cps-cand";
    case VEDA_KEY32_SRC_TEMP_KX64_FIRST32:
        return "temp-kx64-first32";
    case VEDA_KEY32_SRC_TEMP_CAND_REPEAT:
        return "temp-cand-repeat";
    case VEDA_KEY32_SRC_AIR_H1_DB01_BASE:
        return "air-h1-db01-base";
    case VEDA_KEY32_SRC_AIR_H2_DB01_EB_REFRESH:
        return "air-h2-db01-eb-refresh";
    case VEDA_KEY32_SRC_AIR_H3_DB01_EB_KEY32:
        return "air-h3-db01-eb-key32";
    case VEDA_KEY32_SRC_AIR_H4_SPLIT_LEVELS:
        return "air-h4-split-levels";
    default:
        return "unknown";
    }
}

static const char *veda_st_profile_name(veda_seed_tweak_profile_t p)
{
    switch (p)
    {
    case VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8:
        return "zero-seed-cand-tweak-first8";
    case VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8:
        return "cand-seed-first8-cand-tweak-last8";
    default:
        return "unknown";
    }
}

veda_hypothesis_t veda_hypothesis_from_string(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return VEDA_HYP_COLLECT;
    if (veda_str_eq(s, "collect"))
        return VEDA_HYP_COLLECT;
    if (veda_str_eq(s, "main-tree"))
        return VEDA_HYP_MAIN_TREE;
    if (veda_str_eq(s, "runtime-bridge"))
        return VEDA_HYP_RUNTIME_BRIDGE;
    if (veda_str_eq(s, "case7-service") || veda_str_eq(s, "case7"))
        return VEDA_HYP_CASE7_SERVICE;
    if (veda_str_eq(s, "case8-proof"))
        return VEDA_HYP_CASE8_PROOF;
    if (veda_str_eq(s, "auto"))
        return VEDA_HYP_AUTO;
    return VEDA_HYP_UNKNOWN;
}

void veda_set_hypothesis(veda_hypothesis_t h)
{
    g_veda_ctx.hypothesis = h;
}

void veda_set_debug(int debug)
{
    g_veda_ctx.debug = debug ? 1 : 0;
}

void veda_set_cps_key16(const uint8_t key16[VEDA_CPS_KEY16_BYTES])
{
    if (key16 == NULL)
    {
        memset(g_veda_ctx.cps_key16, 0, sizeof(g_veda_ctx.cps_key16));
        g_veda_ctx.cps_key_valid = 0;
        return;
    }

    memcpy(g_veda_ctx.cps_key16, key16, VEDA_CPS_KEY16_BYTES);
    g_veda_ctx.cps_key_valid = 1;

    if (g_veda_ctx.debug)
    {
        fprintf(stderr, "\n[VEDA NEW] CPS-key16 set: ");
        veda_hexdump_stderr(NULL, g_veda_ctx.cps_key16, VEDA_CPS_KEY16_BYTES);
        fprintf(stderr, "\n");
    }
}

void veda_ms_reset(veda_context_t *v)
{
    if (v == NULL)
        return;
    memset(&v->ms, 0, sizeof(v->ms));
    memset(&v->kx, 0, sizeof(v->kx));
    memset(&v->stream, 0, sizeof(v->stream));
    memset(&v->key_candidate, 0, sizeof(v->key_candidate));
    v->key32_candidate_count = 0;
    v->voice_try_count = 0;
    v->rx_transform_count = 0;
}

static void veda_ms_reset_dynamic_after_db01(veda_context_t *v)
{
    if (!v)
        return;

    memset(v->ms.emb_raw, 0, sizeof(v->ms.emb_raw));
    memset(v->ms.last_unique_emb_raw, 0, sizeof(v->ms.last_unique_emb_raw));
    memset(v->ms.mi32_source, 0, sizeof(v->ms.mi32_source));

    v->ms.emb_len = 0;
    v->ms.emb_valid = 0;
    v->ms.last_unique_emb_len = 0;
    v->ms.last_unique_emb_valid = 0;
    v->ms.emb_changed = 0;
    v->ms.emb_unique_count = 0;

    v->ms.mi32 = 0;
    v->ms.mi32_valid = 0;

    memset(v->ms.cand_raw, 0, sizeof(v->ms.cand_raw));
    v->ms.cand_len = 0;
    v->ms.cand_valid = 0;
    v->ms.cand_source_type = 0;

    memset(v->ms.kx64_raw, 0, sizeof(v->ms.kx64_raw));
    v->ms.kx64_valid = 0;
    v->ms.kx64_count = 0;

    memset(&v->kx, 0, sizeof(v->kx));
    memset(&v->stream, 0, sizeof(v->stream));
    memset(&v->key_candidate, 0, sizeof(v->key_candidate));

    v->voice_try_count = 0;
    v->key32_candidate_count = 0;
    v->rx_transform_count = 0;
}

void veda_ms_collect_vlc(const uint8_t *raw, int len, int crc_ok, int fec_err)
{
    veda_context_t *v = &g_veda_ctx;
    int same = 0;

    if (raw != NULL && len > 0 && v->ms.last_unique_vlc_valid &&
        v->ms.last_unique_vlc_len == len &&
        memcmp(v->ms.last_unique_vlc_raw, raw, (size_t)len) == 0)
    {
        same = 1;
    }

    veda_copy_field(v->ms.vlc_raw, &v->ms.vlc_len, &v->ms.vlc_valid, raw, len);

    v->ms.crc_ok = (uint8_t)(crc_ok ? 1 : 0);
    v->ms.fec_err = (uint8_t)(fec_err ? 1 : 0);
    v->ms.vlc_changed = same ? 0 : 1;

    if (!same && raw != NULL && len > 0)
    {
        v->ms.session_count++;
        v->ms.session_id = v->ms.session_count;

        veda_copy_field(v->ms.last_unique_vlc_raw, &v->ms.last_unique_vlc_len,
                        &v->ms.last_unique_vlc_valid, raw, len);

        veda_ms_reset_dynamic_after_db01(v);
    }

    if (v->debug)
    {
        fprintf(stderr,
                "\n[VEDA MS VLC] slot=%d session=%u len=%d crc=%d fec_err=%d changed=%d raw=",
                VEDA_MS_DISPLAY_SLOT,
                v->ms.session_id,
                v->ms.vlc_len,
                crc_ok,
                fec_err,
                v->ms.vlc_changed);
        veda_hexdump_stderr(NULL, v->ms.vlc_raw, v->ms.vlc_len);
        fprintf(stderr, "\n");
    }
}

void veda_ms_collect_emb(const uint8_t *raw, int len, int crc_ok, int fec_err)
{
    veda_context_t *v = &g_veda_ctx;
    int same = 0;

    if (raw != NULL && len > 0 && v->ms.last_unique_emb_valid &&
        v->ms.last_unique_emb_len == len &&
        memcmp(v->ms.last_unique_emb_raw, raw, (size_t)len) == 0)
    {
        same = 1;
    }

    veda_copy_field(v->ms.emb_raw, &v->ms.emb_len, &v->ms.emb_valid, raw, len);

    v->ms.crc_ok = (uint8_t)(crc_ok ? 1 : 0);
    v->ms.fec_err = (uint8_t)(fec_err ? 1 : 0);
    v->ms.emb_changed = same ? 0 : 1;

    if (!same && raw != NULL && len > 0)
    {
        veda_copy_field(v->ms.last_unique_emb_raw, &v->ms.last_unique_emb_len,
                        &v->ms.last_unique_emb_valid, raw, len);
        v->ms.emb_unique_count++;
    }

    if (v->debug)
    {
        fprintf(stderr,
                "\n[VEDA MS EMB] slot=%d sf=%u len=%d crc=%d fec_err=%d changed=%d uniq=%u raw=",
                VEDA_MS_DISPLAY_SLOT,
                v->ms.superframe,
                v->ms.emb_len,
                crc_ok,
                fec_err,
                v->ms.emb_changed,
                v->ms.emb_unique_count);
        veda_hexdump_stderr(NULL, v->ms.emb_raw, v->ms.emb_len);
        fprintf(stderr, "\n");
    }
}

void veda_ms_collect_voice_dyn32(uint32_t voice_dyn32, const char *source)
{
    veda_context_t *v = &g_veda_ctx;

    v->ms.mi32 = voice_dyn32;
    v->ms.mi32_valid = 1;

    memset(v->ms.mi32_source, 0, sizeof(v->ms.mi32_source));
    if (source != NULL)
        strncpy(v->ms.mi32_source, source, sizeof(v->ms.mi32_source) - 1);

    if (v->debug)
    {
        fprintf(stderr, "\n[VEDA MS VOICE-DYN32] slot=%d sf=%u source=%s dyn32=%08X\n",
                VEDA_MS_DISPLAY_SLOT,
                v->ms.superframe,
                v->ms.mi32_source[0] ? v->ms.mi32_source : "unknown",
                v->ms.mi32);
    }
}

void veda_ms_collect_mi32(uint32_t mi32, const char *source)
{
    veda_ms_collect_voice_dyn32(mi32, source ? source : "legacy-mi32");
}

void veda_ms_collect_ids(uint32_t id_a, uint32_t id_b)
{
    veda_context_t *v = &g_veda_ctx;
    v->ms.id_a = id_a;
    v->ms.id_b = id_b;
    v->ms.ids_valid = 1;

    if (v->debug)
    {
        fprintf(stderr, "\n[VEDA MS IDS] slot=%d id_a=0x%06X id_b=0x%06X\n",
                VEDA_MS_DISPLAY_SLOT, id_a & 0xFFFFFFu, id_b & 0xFFFFFFu);
    }
}

void veda_ms_set_position(uint32_t superframe, uint32_t burst_index, uint32_t seq)
{
    veda_context_t *v = &g_veda_ctx;
    v->ms.superframe = superframe;
    v->ms.burst_index = burst_index;
    v->ms.seq = seq;
}

static uint32_t veda_load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t veda_load_le32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int veda_build_seed_tweak_from_candidate(veda_context_t *v, veda_seed_tweak_profile_t profile, uint8_t seed8[VEDA_SEED8_BYTES], uint32_t *tweak0, uint32_t *tweak1)
{
    const uint8_t *p;
    int n;

    if (!v || !seed8 || !tweak0 || !tweak1)
        return VEDA_RC_ERROR;
    if (!v->ms.cand_valid || v->ms.cand_len < 8)
        return VEDA_RC_WAIT_KEY32;

    p = v->ms.cand_raw;
    n = v->ms.cand_len;

    memset(seed8, 0, VEDA_SEED8_BYTES);
    *tweak0 = 0;
    *tweak1 = 0;

    if (profile == VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8)
    {
        *tweak0 = veda_load_le32(p + 0);
        *tweak1 = veda_load_le32(p + 4);
    }
    else if (profile == VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8)
    {
        memcpy(seed8, p, 8);
        *tweak0 = veda_load_le32(p + n - 8);
        *tweak1 = veda_load_le32(p + n - 4);
    }
    else
    {
        return VEDA_RC_ERROR;
    }

    if (v->debug)
        fprintf(stderr, "[VEDA2 SEED-TWEAK] profile=%d cand_src=%u cand_len=%d seed=%02X%02X%02X%02X%02X%02X%02X%02X tweak=%08X:%08X\n", (int)profile, v->ms.cand_source_type, v->ms.cand_len, seed8[0], seed8[1], seed8[2], seed8[3], seed8[4], seed8[5], seed8[6], seed8[7], *tweak0, *tweak1);

    return VEDA_RC_OK;
}

int veda_try_build_key32_main_tree(veda_context_t *v, veda_key_candidate_t *kc)
{
    if (!v || !kc)
        return VEDA_RC_ERROR;

    memset(kc, 0, sizeof(*kc));

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 MAIN-TREE GATE] cps=%d kx64=%d cand=%d vlc=%d emb=%d sf=%u key32=0\n",
                v->cps_key_valid, v->ms.kx64_valid, v->ms.cand_valid,
                v->ms.vlc_valid, v->ms.emb_valid, v->ms.superframe);
    }

    if (!v->cps_key_valid)
        return VEDA_RC_WAIT_KEY32;
    if (!v->ms.kx64_valid)
        return VEDA_RC_WAIT_KEY32;
    if (!v->ms.cand_valid)
        return VEDA_RC_WAIT_KEY32;

    if (v->debug)
        fprintf(stderr, "[VEDA2 STREAM-GATE] sbx256 shell ready, waiting real key32 builder / IDA proof\n");

    return VEDA_RC_WAIT_IDA_PROOF;
}

static int veda_fill_temp_key32_cps_cand(veda_context_t *v, veda_key_candidate_t *kc)
{
    int i;

    if (!v->cps_key_valid || !v->ms.cand_valid || v->ms.cand_len <= 0)
        return VEDA_RC_WAIT_KEY32;

    for (i = 0; i < VEDA_KEY32_BYTES; i++)
    {
        uint8_t a = v->cps_key16[i & 15];
        uint8_t b = v->ms.cand_raw[i % v->ms.cand_len];
        kc->key32[i] = (uint8_t)(a ^ b ^ (uint8_t)(0xA5u + i));
    }

    kc->source = VEDA_KEY32_SRC_TEMP_CPS_CAND;
    return VEDA_RC_OK;
}

static int veda_fill_temp_key32_kx64_first32(veda_context_t *v, veda_key_candidate_t *kc)
{
    if (!v->ms.kx64_valid)
        return VEDA_RC_WAIT_KEY32;

    memcpy(kc->key32, v->ms.kx64_raw, VEDA_KEY32_BYTES);
    kc->source = VEDA_KEY32_SRC_TEMP_KX64_FIRST32;
    return VEDA_RC_OK;
}

static int veda_fill_temp_key32_cand_repeat(veda_context_t *v, veda_key_candidate_t *kc)
{
    int i;

    if (!v->ms.cand_valid || v->ms.cand_len <= 0)
        return VEDA_RC_WAIT_KEY32;

    for (i = 0; i < VEDA_KEY32_BYTES; i++)
    {
        kc->key32[i] = v->ms.cand_raw[i % v->ms.cand_len];
    }

    kc->source = VEDA_KEY32_SRC_TEMP_CAND_REPEAT;
    return VEDA_RC_OK;
}

static uint32_t veda_mix_u32(uint32_t a, uint32_t b, uint32_t c)
{
    a ^= b + 0x9E3779B9u + (a << 6) + (a >> 2);
    a ^= c + 0x85EBCA6Bu + (a << 7) + (a >> 3);
    return a;
}

static uint32_t veda_load_le32_part(const uint8_t *p, int n, int off)
{
    if (!p || n < off + 4)
        return 0;

    return veda_load_le32(p + off);
}

static void veda_air_absorb_bytes_model(uint32_t st[12], const uint8_t *p, int n,
                                        uint32_t domain)
{
    int i;

    if (!st || !p || n <= 0)
        return;

    st[10] ^= domain;
    st[11] ^= (uint32_t)n;

    for (i = 0; i < n; i++)
    {
        st[i & 11] ^= ((uint32_t)p[i] << ((i & 3) * 8));

        if ((i & 15) == 15)
            veda_gimli384_permute_model(st);
    }

    veda_gimli384_permute_model(st);
}

static void veda_air_squeeze_key32_model(uint32_t st[12], uint8_t out32[VEDA_KEY32_BYTES])
{
    int i;

    veda_gimli384_permute_model(st);

    for (i = 0; i < 8; i++)
        veda_store_le32(out32 + (i * 4), st[i & 11]);
}

static int veda_air_build_session_key32(veda_context_t *v, veda_key_candidate_t *kc,
                                        int hprofile)
{
    uint32_t st[12];
    int i;

    if (!v || !kc)
        return VEDA_RC_ERROR;
    if (!v->cps_key_valid)
        return VEDA_RC_WAIT_KEY32;
    if (!v->ms.vlc_valid && !v->ms.cand_valid)
        return VEDA_RC_WAIT_KEY32;

    memset(st, 0, sizeof(st));

    for (i = 0; i < 4; i++)
        st[i] = veda_load_le32(v->cps_key16 + (i * 4));

    st[4] = (uint32_t)hprofile;
    st[5] = v->ms.session_id;
    st[6] = 0;
    st[7] = 0;

    if (v->ms.vlc_valid)
        veda_air_absorb_bytes_model(st, v->ms.vlc_raw, v->ms.vlc_len, 0x44423031u); /* DB01 */
    else
        veda_air_absorb_bytes_model(st, v->ms.cand_raw, v->ms.cand_len, 0x43414E44u); /* CAND */

    if (v->ms.kx64_valid)
        veda_air_absorb_bytes_model(st, v->ms.kx64_raw, 64, 0x4B583634u); /* KX64 */

    if (hprofile == 3 && v->ms.emb_valid)
        veda_air_absorb_bytes_model(st, v->ms.emb_raw, v->ms.emb_len, 0x45423033u); /* EB03 */

    /*if (hprofile == 4 && v->ms.mi32_valid)
    {
        st[8] ^= v->ms.mi32;
        st[9] ^= veda_mix_u32(v->ms.mi32, v->ms.superframe, v->ms.burst_index);
        veda_gimli384_permute_model(st);
    }*/

    veda_air_squeeze_key32_model(st, kc->key32);

    if (hprofile == 1)
        kc->source = VEDA_KEY32_SRC_AIR_H1_DB01_BASE;
    else if (hprofile == 2)
        kc->source = VEDA_KEY32_SRC_AIR_H2_DB01_EB_REFRESH;
    else if (hprofile == 3)
        kc->source = VEDA_KEY32_SRC_AIR_H3_DB01_EB_KEY32;
    else
        kc->source = VEDA_KEY32_SRC_AIR_H4_SPLIT_LEVELS;

    return VEDA_RC_OK;
}

static int veda_air_profile_from_hypothesis(veda_context_t *v)
{
    if (!v)
        return 1;

    if (v->hypothesis == VEDA_HYP_MAIN_TREE)
        return 1;
    if (v->hypothesis == VEDA_HYP_RUNTIME_BRIDGE)
        return 2;
    if (v->hypothesis == VEDA_HYP_CASE7_SERVICE)
        return 3;
    if (v->hypothesis == VEDA_HYP_CASE8_PROOF)
        return 4;

    return 1;
}

static int veda_try_build_temporary_seed_tweak(veda_context_t *v, veda_seed_tweak_profile_t *used_profile,
                                               uint8_t seed8[VEDA_SEED8_BYTES], uint32_t *tweak0, uint32_t *tweak1)
{
    uint32_t voice_dyn32;
    uint32_t eb0;
    uint32_t eb1;
    uint32_t db01;
    int hprofile;

    if (!v || !used_profile || !seed8 || !tweak0 || !tweak1)
        return VEDA_RC_ERROR;

    if (!v->ms.vlc_valid && !v->ms.emb_valid && !v->ms.mi32_valid && !v->ms.cand_valid)
        return VEDA_RC_WAIT_KEY32;

    hprofile = veda_air_profile_from_hypothesis(v);

    if ((hprofile == 1 || hprofile == 2 || hprofile == 3 || hprofile == 4) &&
        (!v->ms.vlc_valid || !v->ms.emb_valid || !v->ms.mi32_valid))
    {
        if (v->debug)
        {
            fprintf(stderr,
                    "[VEDA2 AIR-H%d WAIT] need db01=%d eb=%d voice_dyn32=%d\n",
                    hprofile,
                    v->ms.vlc_valid,
                    v->ms.emb_valid,
                    v->ms.mi32_valid);
        }

        return VEDA_RC_WAIT_KEY32;
    }

    memset(seed8, 0, VEDA_SEED8_BYTES);

    voice_dyn32 = v->ms.mi32_valid ? v->ms.mi32 : 0;
    eb0 = v->ms.emb_valid ? veda_load_le32_part(v->ms.emb_raw, v->ms.emb_len, 0) : 0;
    eb1 = v->ms.emb_valid ? veda_load_le32_part(v->ms.emb_raw, v->ms.emb_len, v->ms.emb_len - 4) : 0;
    db01 = v->ms.vlc_valid ? veda_load_le32_part(v->ms.vlc_raw, v->ms.vlc_len, 0) : 0;

    if (hprofile == 1)
    {
        seed8[0] = (uint8_t)(voice_dyn32 >> 0);
        seed8[1] = (uint8_t)(voice_dyn32 >> 8);
        seed8[2] = (uint8_t)(voice_dyn32 >> 16);
        seed8[3] = (uint8_t)(voice_dyn32 >> 24);
        seed8[4] = (uint8_t)v->ms.superframe;
        seed8[5] = (uint8_t)v->ms.burst_index;
        // seed8[6] = (uint8_t)v->ms.seq;
        seed8[6] = (uint8_t)(v->rx_transform_count + 1);
        seed8[7] = 0x01;

        if (v->ms.emb_changed)
        {
            *tweak0 = veda_mix_u32(eb0, db01, voice_dyn32);
            // *tweak1 = veda_mix_u32(eb1, v->ms.seq, v->ms.superframe);
            *tweak1 = veda_mix_u32(eb1, v->ms.burst_index, v->rx_transform_count + 1);
        }
        else
        {
            *tweak0 = veda_mix_u32(voice_dyn32, db01, v->ms.burst_index);
            // *tweak1 = veda_mix_u32(v->ms.emb_unique_count, v->ms.seq, v->ms.superframe);
            *tweak1 = veda_mix_u32(eb1, v->ms.burst_index, v->rx_transform_count + 1);
        }
        *used_profile = VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8;
    }
    else if (hprofile == 3)
    {
        seed8[0] = (uint8_t)(voice_dyn32 >> 0);
        seed8[1] = (uint8_t)(voice_dyn32 >> 8);
        seed8[2] = (uint8_t)(voice_dyn32 >> 16);
        seed8[3] = (uint8_t)(voice_dyn32 >> 24);
        seed8[4] = (uint8_t)v->ms.superframe;
        seed8[5] = (uint8_t)v->ms.burst_index;
        seed8[6] = (uint8_t)(v->rx_transform_count + 1);
        seed8[7] = 0x03;

        *tweak0 = veda_mix_u32(eb0, db01, v->ms.emb_unique_count);
        *tweak1 = veda_mix_u32(eb1, voice_dyn32, v->rx_transform_count + 1);

        *used_profile = VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8;
    }
    else if (hprofile == 4)
    {
        seed8[0] = (uint8_t)(eb0 >> 0);
        seed8[1] = (uint8_t)(eb0 >> 8);
        seed8[2] = (uint8_t)(eb0 >> 16);
        seed8[3] = (uint8_t)(eb0 >> 24);
        seed8[4] = (uint8_t)v->ms.superframe;
        seed8[5] = (uint8_t)v->ms.burst_index;
        seed8[6] = (uint8_t)(v->rx_transform_count + 1);
        seed8[7] = 0x04;

        *tweak0 = veda_mix_u32(db01, voice_dyn32, v->ms.emb_unique_count);
        *tweak1 = veda_mix_u32(eb1, v->ms.superframe, v->rx_transform_count + 1);

        *used_profile = VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8;
    }
    else if (hprofile == 33) //было 3
    {
        if (v->ms.emb_valid && v->ms.emb_len >= 8)
            memcpy(seed8, v->ms.emb_raw, 8);
        else if (v->ms.cand_valid && v->ms.cand_len >= 8)
            memcpy(seed8, v->ms.cand_raw, 8);
        else
            return VEDA_RC_WAIT_KEY32;

        *tweak0 = veda_mix_u32(voice_dyn32, v->ms.superframe, v->ms.seq);
        *tweak1 = veda_mix_u32(eb1, v->ms.burst_index, v->ms.voice_triplet_count);
        *used_profile = VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8;
    }
    else
    {
        if (v->ms.vlc_valid && v->ms.vlc_len >= 8)
            memcpy(seed8, v->ms.vlc_raw, 8);
        else if (v->ms.cand_valid && v->ms.cand_len >= 8)
            memcpy(seed8, v->ms.cand_raw, 8);
        else
            return VEDA_RC_WAIT_KEY32;

        *tweak0 = veda_mix_u32(eb0, voice_dyn32, v->ms.burst_index);
        *tweak1 = veda_mix_u32(eb1, db01, v->ms.seq);
        *used_profile = VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8;
    }

    if (v->debug)
    {
        fprintf(stderr,
                "[VEDA2 AIR-H%d] db01=%d eb=%d voice_dyn32=%d seed=%02X%02X%02X%02X%02X%02X%02X%02X tweak=%08X:%08X\n",
                hprofile,
                v->ms.vlc_valid,
                v->ms.emb_valid,
                v->ms.mi32_valid,
                seed8[0], seed8[1], seed8[2], seed8[3],
                seed8[4], seed8[5], seed8[6], seed8[7],
                *tweak0,
                *tweak1);
    }

    return VEDA_RC_OK;
}

static int veda_build_temporary_kx_split_from_air(veda_context_t *v)
{
    int i;
    int n;
    uint8_t a;
    uint8_t b;
    uint8_t c;

    if (!v)
        return VEDA_RC_ERROR;
    if (!v->cps_key_valid)
        return VEDA_RC_WAIT_KEY32;
    if (!v->ms.cand_valid || v->ms.cand_len < 8)
        return VEDA_RC_WAIT_KEY32;

    memset(&v->kx, 0, sizeof(v->kx));

    n = v->ms.cand_len;

    for (i = 0; i < VEDA_KX_SEED32_BYTES; i++)
    {
        a = v->cps_key16[i & 15];
        b = v->ms.cand_raw[i % n];
        c = v->ms.kx64_valid ? v->ms.kx64_raw[i & 63] : 0;
        v->kx.seed32[i] = (uint8_t)(a ^ b ^ c ^ (uint8_t)(0x3Du + i));
    }

    for (i = 0; i < VEDA_KEY32_BYTES; i++)
    {
        a = v->kx.seed32[i];
        b = v->ms.kx64_valid ? v->ms.kx64_raw[(i + 32) & 63] : v->ms.cand_raw[(i + 3) % n];
        c = v->cps_key16[(i + 7) & 15];
        v->kx.out0[i] = (uint8_t)(a ^ b ^ c ^ (uint8_t)(0xA5u + i));
    }

    for (i = 0; i < VEDA_KEY32_BYTES; i++)
    {
        a = v->kx.seed32[31 - i];
        b = v->ms.cand_raw[(i + 5) % n];
        c = v->ms.kx64_valid ? v->ms.kx64_raw[i & 63] : v->cps_key16[i & 15];
        v->kx.out1[i] = (uint8_t)(a ^ b ^ c ^ (uint8_t)(0x5Au + i));
    }

    v->kx.seed32_valid = 1;
    v->kx.out0_valid = 1;
    v->kx.out1_valid = 1;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 TEMP-KX-SPLIT] proof=not_confirmed cand_len=%d kx64=%d seed32=",
                v->ms.cand_len, v->ms.kx64_valid);
        veda_hexdump_stderr(NULL, v->kx.seed32, 8);
        fprintf(stderr, " out0=");
        veda_hexdump_stderr(NULL, v->kx.out0, 8);
        fprintf(stderr, " out1=");
        veda_hexdump_stderr(NULL, v->kx.out1, 8);
        fprintf(stderr, "\n");
    }

    return VEDA_RC_OK;
}

static int veda_fill_temp_key32_out0(veda_context_t *v, veda_key_candidate_t *kc)
{
    if (!v || !kc)
        return VEDA_RC_ERROR;
    if (!v->kx.out0_valid)
        return VEDA_RC_WAIT_KEY32;

    memcpy(kc->key32, v->kx.out0, VEDA_KEY32_BYTES);
    kc->source = VEDA_KEY32_SRC_OUT0;
    return VEDA_RC_OK;
}

static int veda_fill_temp_key32_out1(veda_context_t *v, veda_key_candidate_t *kc)
{
    if (!v || !kc)
        return VEDA_RC_ERROR;
    if (!v->kx.out1_valid)
        return VEDA_RC_WAIT_KEY32;

    memcpy(kc->key32, v->kx.out1, VEDA_KEY32_BYTES);
    kc->source = VEDA_KEY32_SRC_OUT1;
    return VEDA_RC_OK;
}

static int veda_fill_temp_key32_by_hypothesis(veda_context_t *v, veda_key_candidate_t *kc)
{
    int rc;
    int hprofile;

    if (!v || !kc)
        return VEDA_RC_ERROR;

    hprofile = veda_air_profile_from_hypothesis(v);

    rc = veda_air_build_session_key32(v, kc, hprofile);
    if (rc == VEDA_RC_OK)
        return rc;

    rc = veda_fill_temp_key32_kx64_first32(v, kc);
    if (rc != VEDA_RC_OK)
        rc = veda_fill_temp_key32_cps_cand(v, kc);
    if (rc != VEDA_RC_OK)
        rc = veda_fill_temp_key32_cand_repeat(v, kc);

    return rc;
}

int veda_try_build_temporary_key_candidate(veda_context_t *v, veda_key_candidate_t *kc)
{
    uint8_t seed8[VEDA_SEED8_BYTES];
    uint32_t tweak0 = 0;
    uint32_t tweak1 = 0;
    veda_seed_tweak_profile_t st_profile = VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8;
    int rc;

    if (!v || !kc)
        return VEDA_RC_ERROR;

    memset(kc, 0, sizeof(*kc));

    if (!v->cps_key_valid)
        return VEDA_RC_WAIT_KEY32;
    if (!v->ms.cand_valid || v->ms.cand_len < 8)
        return VEDA_RC_WAIT_KEY32;

    rc = veda_fill_temp_key32_by_hypothesis(v, kc);
    if (rc != VEDA_RC_OK)
        return rc;

    rc = veda_try_build_temporary_seed_tweak(v, &st_profile, seed8, &tweak0, &tweak1);

    if (rc != VEDA_RC_OK)
        return rc;

    memcpy(kc->seed8, seed8, VEDA_SEED8_BYTES);
    kc->tweak0 = tweak0;
    kc->tweak1 = tweak1;

    kc->key_valid = 1;
    kc->seed_valid = 1;
    kc->tweak_valid = 1;
    kc->score = 0;

    if (v->debug)
    {
        fprintf(stderr,
                "[VEDA2 TEMP-KEY] proof=not_confirmed hyp=%s source=%s st_profile=%s cand_len=%d kx64=%d\n",
                veda_hypothesis_name(v->hypothesis),
                veda_key32_source_name(kc->source),
                veda_st_profile_name(st_profile),
                v->ms.cand_len,
                v->ms.kx64_valid);
    }

    return VEDA_RC_OK;
}

int veda_stream_init_from_key_candidate(veda_context_t *v,
                                        veda_key_candidate_t *kc)
{
    int rc;

    if (!v || !kc)
        return VEDA_RC_ERROR;

    if (!kc->key_valid || !kc->seed_valid || !kc->tweak_valid)
    {
        if (v->debug)
        {
            fprintf(stderr,
                    "[VEDA2 STREAM-FROM-KC] wait key=%d seed=%d tweak=%d\n",
                    kc->key_valid,
                    kc->seed_valid,
                    kc->tweak_valid);
        }

        return VEDA_RC_WAIT_KEY32;
    }

    rc = veda_stream_init_model(
        &v->stream,
        kc->key32,
        kc->seed8,
        kc->tweak0,
        kc->tweak1,
        kc->source);

    if (rc == VEDA_RC_WAIT_IDA_PROOF)
        rc = VEDA_RC_OK;

    if (rc == VEDA_RC_OK)
    {
        v->stream.stream_valid = 1;

        if (v->debug)
        {
            fprintf(stderr, "[VEDA2 STREAM-FROM-KC] ready keysrc=%s proof=not_confirmed key32=", veda_key32_source_name(kc->source));
            veda_hexdump_stderr(NULL, kc->key32, VEDA_KEY32_BYTES);

            fprintf(stderr, " seed8=");
            veda_hexdump_stderr(NULL, kc->seed8, VEDA_SEED8_BYTES);

            fprintf(stderr, " tweak=%08X:%08X\n", kc->tweak0, kc->tweak1);
        }
    }

    return rc;
}

int veda_try_voice_candidate(veda_context_t *v, veda_stream_ctx_t *sc, char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS])
{
    if (!v || !sc || !ambe_fr || !ambe_fr2 || !ambe_fr3)
        return VEDA_RC_ERROR;

    if (!sc->key32_valid || !sc->seed8_valid || !sc->tweak_valid)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 VOICE-GATE] stream_not_ready key32=%d seed8=%d tweak=%d\n", sc->key32_valid, sc->seed8_valid, sc->tweak_valid);
        return VEDA_RC_WAIT_KEY32;
    }

    if (v->debug)
        fprintf(stderr, "[VEDA2 VOICE-GATE] ready keysrc=%d bits=%d/%d/%d transform=WAIT_IDA_PROOF\n", (int)sc->key32_source, veda_ambe_count_bits(ambe_fr), veda_ambe_count_bits(ambe_fr2), veda_ambe_count_bits(ambe_fr3));

    return VEDA_RC_WAIT_IDA_PROOF;
}

static void veda_rx_pack_one_dibit(uint8_t out27[27], int *bitpos, int dibit)
{
    int b1 = (dibit >> 1) & 1;
    int b0 = dibit & 1;

    if (b1)
        out27[*bitpos >> 3] |= (uint8_t)(1u << (7 - (*bitpos & 7)));
    (*bitpos)++;

    if (b0)
        out27[*bitpos >> 3] |= (uint8_t)(1u << (7 - (*bitpos & 7)));
    (*bitpos)++;
}

static int veda_rx_unpack_one_bit(const uint8_t in27[27], int *bitpos)
{
    int bit = (in27[*bitpos >> 3] >> (7 - (*bitpos & 7))) & 1;
    (*bitpos)++;
    return bit;
}

static void veda_rx_pack_range(dsd_state *state, int first, int last_excl, uint8_t out27[27], int *bitpos)
{
    int i;

    for (i = first; i < last_excl; i++)
    {
        veda_rx_pack_one_dibit(out27, bitpos, state->dmr_stereo_payload[i] & 3);
    }
}

static void veda_rx_unpack_range(dsd_state *state, int first, int last_excl, const uint8_t in27[27], int *bitpos)
{
    int i;

    for (i = first; i < last_excl; i++)
    {
        int b1 = veda_rx_unpack_one_bit(in27, bitpos);
        int b0 = veda_rx_unpack_one_bit(in27, bitpos);
        state->dmr_stereo_payload[i] = (char)((b1 << 1) | b0);
    }
}

int veda_rx_pack_ms_payload216(dsd_state *state, uint8_t out27[27])
{
    int bitpos = 0;

    if (!state || !out27)
        return VEDA_RC_ERROR;

    memset(out27, 0, 27);

    veda_rx_pack_range(state, 12, 48, out27, &bitpos);
    veda_rx_pack_range(state, 48, 66, out27, &bitpos);
    veda_rx_pack_range(state, 90, 108, out27, &bitpos);
    veda_rx_pack_range(state, 108, 144, out27, &bitpos);

    return (bitpos == 216) ? VEDA_RC_OK : VEDA_RC_ERROR;
}

int veda_rx_unpack_ms_payload216(dsd_state *state, const uint8_t in27[27])
{
    int bitpos = 0;

    if (!state || !in27)
        return VEDA_RC_ERROR;

    veda_rx_unpack_range(state, 12, 48, in27, &bitpos);
    veda_rx_unpack_range(state, 48, 66, in27, &bitpos);
    veda_rx_unpack_range(state, 90, 108, in27, &bitpos);
    veda_rx_unpack_range(state, 108, 144, in27, &bitpos);

    return (bitpos == 216) ? VEDA_RC_OK : VEDA_RC_ERROR;
}

int veda_rx_rebuild_ms_ambe_from_payload(dsd_state *state, char ambe_fr[4][24], char ambe_fr2[4][24], char ambe_fr3[4][24])
{
    const int *w, *x, *y, *z;
    int i;
    int dibit;

    if (!state || !ambe_fr || !ambe_fr2 || !ambe_fr3)
        return VEDA_RC_ERROR;

    memset(ambe_fr, 0, sizeof(char) * 4 * 24);
    memset(ambe_fr2, 0, sizeof(char) * 4 * 24);
    memset(ambe_fr3, 0, sizeof(char) * 4 * 24);

    w = rW;
    x = rX;
    y = rY;
    z = rZ;
    for (i = 0; i < 36; i++)
    {
        dibit = state->dmr_stereo_payload[i + 12] & 3;
        ambe_fr[*w][*x] = (1 & (dibit >> 1));
        ambe_fr[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }

    w = rW;
    x = rX;
    y = rY;
    z = rZ;
    for (i = 0; i < 18; i++)
    {
        dibit = state->dmr_stereo_payload[i + 48] & 3;
        ambe_fr2[*w][*x] = (1 & (dibit >> 1));
        ambe_fr2[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }

    for (i = 0; i < 18; i++)
    {
        dibit = state->dmr_stereo_payload[i + 90] & 3;
        ambe_fr2[*w][*x] = (1 & (dibit >> 1));
        ambe_fr2[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }

    w = rW;
    x = rX;
    y = rY;
    z = rZ;
    for (i = 0; i < 36; i++)
    {
        dibit = state->dmr_stereo_payload[i + 108] & 3;
        ambe_fr3[*w][*x] = (1 & (dibit >> 1));
        ambe_fr3[*y][*z] = (1 & dibit);
        w++;
        x++;
        y++;
        z++;
    }

    return VEDA_RC_OK;
}

static int veda_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t len)
{
    size_t i;
    int n = 0;

    if (!a || !b)
        return 0;

    for (i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            n++;
    }

    return n;
}

int veda_rx_try_payload216(veda_context_t *v, dsd_state *state)
{
    uint8_t enc27[27];
    uint8_t dec27[27];
    int rc;

    if (!v || !state)
        return VEDA_RC_ERROR;

    rc = veda_rx_pack_ms_payload216(state, enc27);
    if (rc != VEDA_RC_OK)
        return rc;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 RX-PAYLOAD216-IN] raw=");
        veda_hexdump_stderr(NULL, enc27, 27);
        fprintf(stderr, "\n");
    }

    if (!v->stream.key32_valid || !v->stream.seed8_valid || !v->stream.tweak_valid)
        return VEDA_RC_WAIT_KEY32;

    memcpy(dec27, enc27, 27);
    rc = veda_stream_cfb128_crypt_model(&v->stream, dec27, 27);

    if (rc != VEDA_RC_OK)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 RX-PAYLOAD216-WAIT] cfb_rc=%d\n", rc);
        return rc;
    }

    if (v->debug)
    {
        int diff = veda_count_diff_bytes(enc27, dec27, 27);

        fprintf(stderr, "[VEDA2 RX-PAYLOAD216-OUT] diff=%d raw=", diff);
        veda_hexdump_stderr(NULL, dec27, 27);
        fprintf(stderr, "\n");
    }

    rc = veda_rx_unpack_ms_payload216(state, dec27);
    if (rc != VEDA_RC_OK)
        return rc;

    return VEDA_RC_OK;
}

int veda_ms_on_voice_triplet(dsd_opts *opts, dsd_state *state, int slot, char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS])
{
    veda_context_t *v = &g_veda_ctx;
    int rc;
    int prc;

    (void)opts;

    if (!state)
        return 0;

    v->voice_try_count++;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 MS VOICE] slot=%d hyp=%d try=%u cps=%d kx64=%d cand=%d vlc=%d emb=%d\n", slot, (int)v->hypothesis, v->voice_try_count, v->cps_key_valid, v->ms.kx64_valid, v->ms.cand_valid, v->ms.vlc_valid, v->ms.emb_valid);
    }

    if (v->hypothesis == VEDA_HYP_COLLECT)
        return 0;

    if (!v->cps_key_valid)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 WAIT_CPS16]\n");
        return 0;
    }

    rc = VEDA_RC_WAIT_IDA_PROOF;

    if (v->hypothesis == VEDA_HYP_MAIN_TREE || v->hypothesis == VEDA_HYP_AUTO)
    {
        rc = veda_try_build_key32_main_tree(v, &v->key_candidate);
    }
    else if (v->debug)
    {
        fprintf(stderr, "[VEDA2 RX-HYP-GATE] hyp=%d real_key32=WAIT_IDA_PROOF, using temp candidate\n",
                (int)v->hypothesis);
    }

    if (rc != VEDA_RC_OK)
    {
        rc = veda_try_build_temporary_key_candidate(v, &v->key_candidate);
    }

    if (rc == VEDA_RC_OK)
    {
        rc = veda_stream_init_from_key_candidate(v, &v->key_candidate);
    }

    if (rc != VEDA_RC_OK && v->debug)
    {
        fprintf(stderr, "[VEDA2 RX-KEY-STREAM-BUILD] hyp=%d rc=%d\n", (int)v->hypothesis, rc);
    }

    prc = veda_rx_try_payload216(v, state);
    if (prc == VEDA_RC_OK)
    {
        v->rx_transform_count++;
        return 1;
    }

    if (!v->stream.key32_valid)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 MAIN-TREE GATE] cps=%d kx64=%d cand=%d vlc=%d emb=%d sf=%u key32=0\n", v->cps_key_valid, v->ms.kx64_valid, v->ms.cand_valid, v->ms.vlc_valid, v->ms.emb_valid, v->ms.superframe);
        return 0;
    }

    veda_try_voice_candidate(v, &v->stream, ambe_fr, ambe_fr2, ambe_fr3);
    return 0;
}

int veda_stream256_init_model(veda_stream_ctx_t *ctx, const uint8_t *seed8, const uint8_t *key32)
{
    int i;

    if (!ctx || !seed8 || !key32)
        return VEDA_RC_ERROR;

    memset(ctx, 0, sizeof(*ctx));

    memcpy(ctx->seed8, seed8, VEDA_SEED8_BYTES);
    memcpy(ctx->key32, key32, VEDA_KEY32_BYTES);

    ctx->st[0] = veda_load_le32(seed8 + 0);
    ctx->st[1] = veda_load_le32(seed8 + 4);
    ctx->st[2] = 0x78323673u; /* "sbx8" marker/model */
    ctx->st[3] = 0x00000100u;

    for (i = 0; i < 4; i++)
        ctx->st[4 + i] ^= veda_load_le32(key32 + (i * 4));

    veda_gimli384_permute_model(ctx->st);

    for (i = 0; i < 4; i++)
        ctx->st[4 + i] ^= veda_load_le32(key32 + 16 + (i * 4));

    ctx->st[10] ^= 0x78323673u; /* sbx256/domain placeholder */
    ctx->st[11] ^= 0x000000FDu; /* model: permute/domain param 253 */

    ctx->seed8_valid = 1;
    ctx->key32_valid = 1;

    return VEDA_RC_OK;
}

int veda_stream256_apply_tweak64_model(veda_stream_ctx_t *ctx, uint32_t tweak0, uint32_t tweak1)
{
    if (!ctx || !ctx->key32_valid || !ctx->seed8_valid)
        return VEDA_RC_ERROR;

    ctx->tweak0 = tweak0;
    ctx->tweak1 = tweak1;

    ctx->st[0] ^= tweak0;
    ctx->st[1] ^= tweak1;
    ctx->st[2] ^= 0x54574B36u; /* "TWK6" model marker */
    ctx->st[3] ^= 0x00000040u; /* 64-bit tweak marker */

    veda_gimli384_permute_model(ctx->st);

    ctx->tweak_valid = 1;

    return VEDA_RC_OK;
}

int veda_stream_init_model(veda_stream_ctx_t *ctx, const uint8_t *key32, const uint8_t *seed8,
                           uint32_t tweak0, uint32_t tweak1, veda_key32_source_t key32_source)
{
    int rc;

    rc = veda_stream256_init_model(ctx, seed8, key32);
    if (rc != VEDA_RC_OK)
        return rc;

    rc = veda_stream256_apply_tweak64_model(ctx, tweak0, tweak1);
    if (rc != VEDA_RC_OK)
        return rc;

    ctx->stream_valid = 1;
    ctx->key32_source = key32_source;

    return VEDA_RC_OK;
}

int veda_stream_generate_keystream_model(veda_stream_ctx_t *ctx, uint8_t *out, size_t len)
{
    uint8_t word[4];
    size_t pos = 0;
    size_t n;
    int i;

    if (!ctx || !out || len == 0)
        return VEDA_RC_ERROR;

    if (!ctx->key32_valid || !ctx->seed8_valid || !ctx->tweak_valid)
        return VEDA_RC_WAIT_KEY32;

    while (pos < len)
    {
        veda_gimli384_permute_model(ctx->st);

        for (i = 0; i < 4 && pos < len; i++)
        {
            veda_store_le32(word, ctx->st[i]);

            n = len - pos;
            if (n > 4)
                n = 4;

            memcpy(out + pos, word, n);
            pos += n;
        }
    }

    return VEDA_RC_OK;
}

int veda_stream_cfb128_crypt_model(veda_stream_ctx_t *ctx, uint8_t *buf, size_t len)
{
    uint8_t ks[27];
    size_t i;
    int rc;

    if (!ctx || !buf || len == 0)
        return VEDA_RC_ERROR;
    if (len > sizeof(ks))
        return VEDA_RC_ERROR;

    rc = veda_stream_generate_keystream_model(ctx, ks, len);
    if (rc != VEDA_RC_OK)
        return rc;

#ifdef VEDA_DEBUG_KEYSTREAM
    fprintf(stderr, "[VEDA2 KS27] len=%zu raw=", len);
    veda_hexdump_stderr(NULL, ks, len);
    fprintf(stderr, "\n");
#endif

    for (i = 0; i < len; i++)
        buf[i] ^= ks[i];

    return VEDA_RC_OK;
}

int veda_key_primary_material_init_model(veda_case8_proof_t *case8)
{
    if (case8 == NULL)
        return VEDA_RC_ERROR;
    memset(case8, 0, sizeof(*case8));

    /* Firmware analogue: veda_key_primary_material_init / sub_8005D54.
     * Confirmed role: builds case8 primary16 secure-loader/auth material.
     * Not a voice key32.
     * TODO/IDA: feed real device_secret_out and OTP only in side proof mode.
     */
    return VEDA_RC_NOT_IMPLEMENTED;
}

int veda_case8_build_key32_candidate_model(veda_case8_proof_t *case8)
{
    if (case8 == NULL)
        return VEDA_RC_ERROR;
    if (!case8->primary16_valid)
        return VEDA_RC_WAIT_IDA_PROOF;

    /* Candidate only: primary16 || zero16.
     * Requires stack-layout proof before it can be treated as fact.
     */
    memset(case8->key32_candidate, 0, sizeof(case8->key32_candidate));
    memcpy(case8->key32_candidate, case8->primary16, 16);
    case8->key32_candidate_valid = 1;

    return VEDA_RC_OK;
}

int veda_case8_build_tweak64_candidate_model(veda_case8_proof_t *case8)
{
    if (case8 == NULL)
        return VEDA_RC_ERROR;

    /* Candidate only:
     *   tweak64 = [mix0, mix1]
     * Requires ASM arg-proof into stream_init/apply_tweak64.
     */
    if (!case8->tweak_valid)
        return VEDA_RC_WAIT_IDA_PROOF;
    return VEDA_RC_OK;
}

void veda_ms_collect_candidate(uint8_t source_type, const uint8_t *payload,
                               uint8_t payload_len, int sf_cur)
{
    veda_context_t *v = &g_veda_ctx;
    int n;

    if (!payload || payload_len == 0)
        return;

    n = payload_len;
    if (n > VEDA_MAX_FIELD_BYTES)
        n = VEDA_MAX_FIELD_BYTES;

    if (source_type == VEDA_CAND_VLC01)
        veda_ms_collect_vlc(payload, n, 1, 0);
    else if (source_type == VEDA_CAND_VC_EMB)
        veda_ms_collect_emb(payload, n, 1, 0);

    veda_copy_field(v->ms.cand_raw, &v->ms.cand_len,
                    &v->ms.cand_valid, payload, n);

    v->ms.cand_source_type = source_type;
    v->ms.cand_sf_cur = sf_cur;
    v->ms.cand_count++;

    if (v->debug)
    {
        fprintf(stderr,
                "\n[VEDA2 CAND] src=%u sf=%d len=%d count=%u raw=",
                source_type, sf_cur, v->ms.cand_len, v->ms.cand_count);
        veda_hexdump_stderr(NULL, v->ms.cand_raw, v->ms.cand_len);
        fprintf(stderr, "\n");
    }
}

void veda_ms_collect_kx64(const uint8_t *payload64)
{
    veda_context_t *v = &g_veda_ctx;

    if (!payload64)
        return;

    v->ms.kx64_valid = 1;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 KX64] raw=");
        for (int i = 0; i < 64; i++)
            fprintf(stderr, "%02X", payload64[i]);
        fprintf(stderr, "\n");
    }
}
