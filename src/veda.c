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

#include <stdio.h>
#include <string.h>
#include <ctype.h>

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
    case VEDA_HYP_CASE8_PROOF:
        return "case8-proof";
    case VEDA_HYP_AUTO:
        return "auto";
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
}

void veda_ms_collect_vlc(const uint8_t *raw, int len, int crc_ok, int fec_err)
{
    veda_context_t *v = &g_veda_ctx;
    veda_copy_field(v->ms.vlc_raw, &v->ms.vlc_len, &v->ms.vlc_valid, raw, len);
    v->ms.crc_ok = (uint8_t)(crc_ok ? 1 : 0);
    v->ms.fec_err = (uint8_t)(fec_err ? 1 : 0);

    if (v->debug)
    {
        fprintf(stderr, "\n[VEDA MS VLC] slot=%d len=%d crc=%d fec_err=%d raw=",
                VEDA_MS_DISPLAY_SLOT, v->ms.vlc_len, crc_ok, fec_err);
        veda_hexdump_stderr(NULL, v->ms.vlc_raw, v->ms.vlc_len);
        fprintf(stderr, "\n");
    }
}

void veda_ms_collect_emb(const uint8_t *raw, int len, int crc_ok, int fec_err)
{
    veda_context_t *v = &g_veda_ctx;
    veda_copy_field(v->ms.emb_raw, &v->ms.emb_len, &v->ms.emb_valid, raw, len);
    v->ms.crc_ok = (uint8_t)(crc_ok ? 1 : 0);
    v->ms.fec_err = (uint8_t)(fec_err ? 1 : 0);

    if (v->debug)
    {
        fprintf(stderr, "\n[VEDA MS EMB] slot=%d sf=%u len=%d crc=%d fec_err=%d raw=",
                VEDA_MS_DISPLAY_SLOT, v->ms.superframe, v->ms.emb_len, crc_ok, fec_err);
        veda_hexdump_stderr(NULL, v->ms.emb_raw, v->ms.emb_len);
        fprintf(stderr, "\n");
    }
}

void veda_ms_collect_mi32(uint32_t mi32, const char *source)
{
    veda_context_t *v = &g_veda_ctx;
    v->ms.mi32 = mi32;
    v->ms.mi32_valid = 1;
    memset(v->ms.mi32_source, 0, sizeof(v->ms.mi32_source));
    if (source != NULL)
    {
        strncpy(v->ms.mi32_source, source, sizeof(v->ms.mi32_source) - 1);
    }

    if (v->debug)
    {
        fprintf(stderr, "\n[VEDA MS MI] slot=%d sf=%u source=%s mi32=%08X\n",
                VEDA_MS_DISPLAY_SLOT, v->ms.superframe,
                v->ms.mi32_source[0] ? v->ms.mi32_source : "unknown",
                v->ms.mi32);
    }
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

    if (!v || !seed8 || !tweak0 || !tweak1) return VEDA_RC_ERROR;
    if (!v->ms.cand_valid || v->ms.cand_len < 8) return VEDA_RC_WAIT_KEY32;

    p = v->ms.cand_raw;
    n = v->ms.cand_len;

    memset(seed8, 0, VEDA_SEED8_BYTES);
    *tweak0 = 0;
    *tweak1 = 0;

    if (profile == VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8) {
        *tweak0 = veda_load_be32(p + 0);
        *tweak1 = veda_load_be32(p + 4);
    } else if (profile == VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8) {
        memcpy(seed8, p, 8);
        *tweak0 = veda_load_be32(p + n - 8);
        *tweak1 = veda_load_be32(p + n - 4);
    } else {
        return VEDA_RC_ERROR;
    }

    if (v->debug) fprintf(stderr, "[VEDA2 SEED-TWEAK] profile=%d cand_src=%u cand_len=%d seed=%02X%02X%02X%02X%02X%02X%02X%02X tweak=%08X:%08X\n", (int)profile, v->ms.cand_source_type, v->ms.cand_len, seed8[0], seed8[1], seed8[2], seed8[3], seed8[4], seed8[5], seed8[6], seed8[7], *tweak0, *tweak1);

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

int veda_try_voice_candidate(veda_context_t *v, veda_stream_ctx_t *sc, char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS])
{
    if (!v || !sc || !ambe_fr || !ambe_fr2 || !ambe_fr3) return VEDA_RC_ERROR;

    if (!sc->key32_valid || !sc->seed8_valid || !sc->tweak_valid) {
        if (v->debug) fprintf(stderr, "[VEDA2 VOICE-GATE] stream_not_ready key32=%d seed8=%d tweak=%d\n", sc->key32_valid, sc->seed8_valid, sc->tweak_valid);
        return VEDA_RC_WAIT_KEY32;
    }

    if (v->debug) fprintf(stderr, "[VEDA2 VOICE-GATE] ready keysrc=%d bits=%d/%d/%d transform=WAIT_IDA_PROOF\n", (int)sc->key32_source, veda_ambe_count_bits(ambe_fr), veda_ambe_count_bits(ambe_fr2), veda_ambe_count_bits(ambe_fr3));

    return VEDA_RC_WAIT_IDA_PROOF;
}

int veda_ms_on_voice_triplet(dsd_opts *opts, dsd_state *state, int slot, char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS])
{
    (void)opts;
    (void)state;
    (void)ambe_fr;
    (void)ambe_fr2;
    (void)ambe_fr3;

    veda_context_t *v = &g_veda_ctx;

    v->voice_try_count++;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 MS VOICE] slot=%d hyp=%d try=%u cps=%d mi=%d vlc=%d emb=%d\n", slot, (int)v->hypothesis, v->voice_try_count, v->cps_key_valid, v->ms.mi32_valid, v->ms.vlc_valid, v->ms.emb_valid);
    }

    if (v->hypothesis == VEDA_HYP_COLLECT)
        return 0;

    if (!v->cps_key_valid)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 WAIT_CPS16]\n");
        return 0;
    }

    if (!v->stream.key32_valid)
    {
        if (v->debug)
            fprintf(stderr, "[VEDA2 MAIN-TREE GATE] cps=%d kx64=%d vlc=%d emb=%d sf=%u key32=0\n", v->cps_key_valid, v->ms.kx64_valid, v->ms.vlc_valid, v->ms.emb_valid, v->ms.superframe);
        return 0;
    }
    
    if (v->stream.key32_valid) veda_try_voice_candidate(v, &v->stream, ambe_fr, ambe_fr2, ambe_fr3);

    if (v->hypothesis == VEDA_HYP_MAIN_TREE || v->hypothesis == VEDA_HYP_AUTO)
    {
        int rc = veda_try_build_key32_main_tree(v, &v->key_candidate);
        if (rc != VEDA_RC_OK)
            return 0;
    }

    return 0;
}

int veda_stream256_init_model(veda_stream_ctx_t *ctx, const uint8_t *seed8, const uint8_t *key32)
{
    if (!ctx || !seed8 || !key32) return VEDA_RC_ERROR;

    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->seed8, seed8, VEDA_SEED8_BYTES);
    memcpy(ctx->key32, key32, VEDA_KEY32_BYTES);

    ctx->seed8_valid = 1;
    ctx->key32_valid = 1;

    return VEDA_RC_WAIT_IDA_PROOF;
}

int veda_stream_init_model(veda_stream_ctx_t *ctx, const uint8_t *key32, const uint8_t *seed8, 
    uint32_t tweak0, uint32_t tweak1, veda_key32_source_t key32_source)
{
    int rc;

    rc = veda_stream256_init_model(ctx, seed8, key32);
    if (rc == VEDA_RC_ERROR) return rc;

    rc = veda_stream256_apply_tweak64_model(ctx, tweak0, tweak1);
    if (rc == VEDA_RC_ERROR) return rc;

    ctx->stream_valid = 0;
    ctx->key32_source = key32_source;
    return VEDA_RC_WAIT_IDA_PROOF;
}

int veda_stream256_apply_tweak64_model(veda_stream_ctx_t *ctx, uint32_t tweak0, uint32_t tweak1)
{
    if (!ctx || !ctx->key32_valid || !ctx->seed8_valid) return VEDA_RC_ERROR;

    ctx->tweak0 = tweak0;
    ctx->tweak1 = tweak1;
    ctx->tweak_valid = 1;

    return VEDA_RC_WAIT_IDA_PROOF;
}

int veda_stream_cfb128_crypt_model(veda_stream_ctx_t *ctx, uint8_t *buf, size_t len)
{
    if (!ctx || !buf || len == 0) return VEDA_RC_ERROR;
    if (!ctx->key32_valid || !ctx->seed8_valid || !ctx->tweak_valid) return VEDA_RC_WAIT_KEY32;

    return VEDA_RC_WAIT_IDA_PROOF;
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

void veda_ms_collect_candidate(uint8_t source_type, const uint8_t *payload, uint8_t payload_len, int sf_cur)
{
    veda_context_t *v = &g_veda_ctx;
    int n;

    if (!payload || payload_len == 0)
        return;

    n = payload_len;
    if (n > VEDA_MAX_FIELD_BYTES)
        n = VEDA_MAX_FIELD_BYTES;

    memset(v->ms.cand_raw, 0, sizeof(v->ms.cand_raw));
    memcpy(v->ms.cand_raw, payload, (size_t)n);
    v->ms.cand_len = n;
    v->ms.cand_valid = 1;
    v->ms.cand_source_type = source_type;
    v->ms.cand_sf_cur = sf_cur;
    v->ms.cand_count++;

    if (source_type == VEDA_CAND_VLC01)
    {
        memcpy(v->ms.vlc_raw, payload, n);
        v->ms.vlc_len = n;
        v->ms.vlc_valid = 1;
    }
    else if (source_type == VEDA_CAND_VC_EMB)
    {
        memcpy(v->ms.emb_raw, payload, n);
        v->ms.emb_len = n;
        v->ms.emb_valid = 1;
    }

    v->ms.superframe = (uint32_t)sf_cur;

    if (v->debug)
    {
        fprintf(stderr, "[VEDA2 CAND] src=%u sf=%d len=%d raw=", source_type, sf_cur, n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "%02X", payload[i]);
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