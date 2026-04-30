#ifndef VEDA_H
#define VEDA_H
#include "dsd.h"
/*
 * veda.h - VEDA MS-only experimental hypothesis runner for dsd_new.
 *
 * IMPORTANT INTEGRATION RULE:
 *   Do NOT paste this file into dsd.h.
 *   Do NOT include dsd.h from this header.
 *   This header is self-contained on purpose, so it can be included from
 *   dmr_ms.c / option parser / other C files without creating typedef cycles.
 *
 * Current scope:
 *   - MS / Direct Mode / Mono only;
 *   - --veda-key is CPS-key16 / NPSK root material, not key32;
 *   - key32 must be produced by a hypothesis/builder, not passed by CLI;
 *   - functions ending with _model mirror firmware / Hex-Rays pseudocode layers.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef VEDA_MAX_FIELD_BYTES
#define VEDA_MAX_FIELD_BYTES 32
#endif

#define VEDA_CPS_KEY16_BYTES 16
#define VEDA_KEY32_BYTES 32
#define VEDA_SEED8_BYTES 8
#define VEDA_TWEAK64_BYTES 8
#define VEDA_KX_SEED32_BYTES 32
#define VEDA_AMBE_ROWS 4
#define VEDA_AMBE_COLS 24
#define VEDA_MS_SLOT_INDEX 0
#define VEDA_MS_DISPLAY_SLOT 1

    typedef enum
    {
        VEDA_HYP_COLLECT = 0,
        VEDA_HYP_MAIN_TREE,
        VEDA_HYP_RUNTIME_BRIDGE,
        VEDA_HYP_CASE7_SERVICE,
        VEDA_HYP_CASE8_PROOF,
        VEDA_HYP_AUTO,
        VEDA_HYP_UNKNOWN
    } veda_hypothesis_t;

    typedef enum
    {
        VEDA_RC_OK = 0,
        VEDA_RC_ERROR = -1,
        VEDA_RC_NOT_IMPLEMENTED = -2,
        VEDA_RC_WAIT_KEY32 = -3,
        VEDA_RC_WAIT_IDA_PROOF = -4
    } veda_rc_t;

    typedef enum
    {
        VEDA_KEY32_SRC_NONE = 0,
        VEDA_KEY32_SRC_OUT0,
        VEDA_KEY32_SRC_OUT1,
        VEDA_KEY32_SRC_RUNTIME_BRIDGE,
        VEDA_KEY32_SRC_CASE8_SIDE_PROOF,
        VEDA_KEY32_SRC_TEMP_CPS_CAND,
        VEDA_KEY32_SRC_TEMP_KX64_FIRST32,
        VEDA_KEY32_SRC_TEMP_CAND_REPEAT,
        VEDA_KEY32_SRC_AIR_H1_DB01_BASE,
        VEDA_KEY32_SRC_AIR_H2_DB01_EB_REFRESH,
        VEDA_KEY32_SRC_AIR_H3_DB01_EB_KEY32,
        VEDA_KEY32_SRC_AIR_H4_SPLIT_LEVELS
    } veda_key32_source_t;

    typedef enum
    {
        VEDA_ST_PROFILE_ZERO_SEED_CAND_TWEAK_FIRST8 = 0,
        VEDA_ST_PROFILE_CAND_SEED_FIRST8_CAND_TWEAK_LAST8 = 1
    } veda_seed_tweak_profile_t;

    /* Candidate fields collected from DMR MS/VEDA stream.
     * These are not proven algorithm fields. They are inputs for hypotheses.
     */
    typedef struct
    {
        uint8_t vlc_raw[VEDA_MAX_FIELD_BYTES];
        int vlc_len;
        int vlc_valid;

        uint8_t last_unique_vlc_raw[VEDA_MAX_FIELD_BYTES];
        int last_unique_vlc_len;
        int last_unique_vlc_valid;
        int vlc_changed;
        uint32_t session_count;
        uint32_t session_id;

        uint8_t emb_raw[VEDA_MAX_FIELD_BYTES];
        int emb_len;
        int emb_valid;

        uint8_t last_unique_emb_raw[VEDA_MAX_FIELD_BYTES];
        int last_unique_emb_len;
        int last_unique_emb_valid;
        int emb_changed;
        uint32_t emb_unique_count;

        uint32_t mi32;
        int mi32_valid;
        char mi32_source[24];

        uint32_t id_a;
        uint32_t id_b;
        int ids_valid;

        uint32_t superframe;
        uint32_t burst_index;
        uint32_t seq;
        uint32_t voice_triplet_count;

        uint8_t crc_ok;
        uint8_t fec_err;
        uint8_t irr;

        uint8_t cand_raw[VEDA_MAX_FIELD_BYTES];
        int cand_len;
        int cand_valid;
        uint8_t cand_source_type;
        int cand_sf_cur;
        uint32_t cand_count;

        uint8_t kx64_raw[64];
        int kx64_valid;
        uint32_t kx64_count;

    } veda_ms_candidate_t;

    /* Result of KX/split model.
     * out0/out1 are internal results, never external CLI inputs.
     */
    typedef struct
    {
        uint8_t seed32[VEDA_KX_SEED32_BYTES];
        uint8_t out0[VEDA_KEY32_BYTES];
        uint8_t out1[VEDA_KEY32_BYTES];

        int seed32_valid;
        int out0_valid;
        int out1_valid;
    } veda_kx_split_result_t;

    typedef struct
    {
        uint32_t st[12];

        uint8_t key32[VEDA_KEY32_BYTES];
        uint8_t seed8[VEDA_SEED8_BYTES];

        uint32_t tweak0;
        uint32_t tweak1;

        int key32_valid;
        int seed8_valid;
        int tweak_valid;
        int stream_valid;

        veda_key32_source_t key32_source;
    } veda_stream_ctx_t;

    typedef struct
    {
        uint8_t key32[VEDA_KEY32_BYTES];
        uint8_t seed8[VEDA_SEED8_BYTES];
        uint32_t tweak0;
        uint32_t tweak1;
        int key_valid;
        int seed_valid;
        int tweak_valid;
        veda_key32_source_t source;
        uint32_t score;
    } veda_key_candidate_t;

    typedef struct
    {
        uint8_t primary16[16];
        uint8_t key32_candidate[VEDA_KEY32_BYTES];
        uint32_t mix0;
        uint32_t mix1;

        int primary16_valid;
        int key32_candidate_valid;
        int tweak_valid;
    } veda_case8_proof_t;

    struct veda_context_t
    {
        veda_hypothesis_t hypothesis;

        uint8_t cps_key16[16];
        int cps_key_valid;

        veda_ms_candidate_t ms;
        veda_kx_split_result_t kx;
        veda_stream_ctx_t stream;

        uint32_t voice_try_count;
        uint32_t key32_candidate_count;

        int debug;
        int ms_only;
        int initialized;

        veda_key_candidate_t key_candidate;
    };

    /* Global experimental context accessors.
     * Keeping the test context here avoids expanding dsd_state while the new runner
     * is still unstable.
     */
    veda_context_t *veda_get_context(void);
    void veda_init_context(veda_context_t *v);
    void veda_reset_global_context(void);

    const char *veda_hypothesis_name(veda_hypothesis_t h);
    veda_hypothesis_t veda_hypothesis_from_string(const char *s);
    void veda_set_hypothesis(veda_hypothesis_t h);
    void veda_set_debug(int debug);
    void veda_set_cps_key16(const uint8_t key16[VEDA_CPS_KEY16_BYTES]);

    /* MS-only collector API. */
    void veda_ms_reset(veda_context_t *v);
    void veda_ms_collect_vlc(const uint8_t *raw, int len, int crc_ok, int fec_err);
    void veda_ms_collect_emb(const uint8_t *raw, int len, int crc_ok, int fec_err);
    void veda_ms_collect_mi32(uint32_t mi32, const char *source);
    void veda_ms_collect_voice_dyn32(uint32_t voice_dyn32, const char *source);
    void veda_ms_collect_ids(uint32_t id_a, uint32_t id_b);
    void veda_ms_set_position(uint32_t superframe, uint32_t burst_index, uint32_t seq);
    void veda_ms_collect_candidate(uint8_t source_type, const uint8_t *payload, uint8_t payload_len, int sf_cur);
    void veda_ms_collect_kx64(const uint8_t *payload64);

    int veda_try_build_temporary_key_candidate(veda_context_t *v, veda_key_candidate_t *kc);
    int veda_stream_init_from_key_candidate(veda_context_t *v, veda_key_candidate_t *kc);
    int veda_stream_generate_keystream_model(veda_stream_ctx_t *ctx, uint8_t *out, size_t len);

    int veda_rx_pack_ms_payload216(dsd_state *state, uint8_t out27[27]);
    int veda_rx_unpack_ms_payload216(dsd_state *state, const uint8_t in27[27]);

    int veda_try_build_key32_main_tree(veda_context_t *v, veda_key_candidate_t *kc);
    /* Main hook for dmr_ms.c, called before processMbeFrame().
     * opts/state are void* intentionally: veda.h must not depend on dsd.h because
     * current dsd.h uses anonymous typedef structs for dsd_opts/dsd_state.
     * For collect mode this must not modify AMBE frames.
     */

    int veda_ms_on_voice_triplet(
        dsd_opts *opts,
        dsd_state *state,
        int slot,
        char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS],
        char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS],
        char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS]);

    int veda_hypothesis_run_ms(
        void *opts,
        void *state,
        int slot,
        char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS],
        char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS],
        char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS]);

    int veda_build_seed_tweak_from_candidate(veda_context_t *v, veda_seed_tweak_profile_t profile,
                                             uint8_t seed8[VEDA_SEED8_BYTES],
                                             uint32_t *tweak0, uint32_t *tweak1);
    int veda_rx_try_payload216(veda_context_t *v, dsd_state *state);

    int veda_rx_rebuild_ms_ambe_from_payload(dsd_state *state, char ambe_fr[4][24], char ambe_fr2[4][24], char ambe_fr3[4][24]);
    /* Firmware-like model function prototypes.
     * These mirror names/layers found in the firmware pseudocode. Some are still
     * skeletons and intentionally return VEDA_RC_NOT_IMPLEMENTED until ASM/dataflow
     * proof is available.
     */
    int veda_kx_init_context_model(veda_context_t *v);
    int veda_kx_npsk1_initiator_model(veda_context_t *v);
    int veda_kx_npsk1_responder_model(veda_context_t *v);
    int veda_kx_split_model(veda_context_t *v, veda_kx_split_result_t *out);

    int veda_stream_init_model(
        veda_stream_ctx_t *ctx,
        const uint8_t *key32,
        const uint8_t *seed8,
        uint32_t tweak0,
        uint32_t tweak1,
        veda_key32_source_t key32_source);

    int veda_stream256_init_model(
        veda_stream_ctx_t *ctx,
        const uint8_t *seed8,
        const uint8_t *key32);

    int veda_stream256_apply_tweak64_model(
        veda_stream_ctx_t *ctx,
        uint32_t tweak0,
        uint32_t tweak1);

    int veda_stream_cfb128_crypt_model(
        veda_stream_ctx_t *ctx,
        uint8_t *buf,
        size_t len);
    int veda_try_voice_candidate(veda_context_t *v, veda_stream_ctx_t *sc, char ambe_fr[VEDA_AMBE_ROWS][VEDA_AMBE_COLS],
                                 char ambe_fr2[VEDA_AMBE_ROWS][VEDA_AMBE_COLS], char ambe_fr3[VEDA_AMBE_ROWS][VEDA_AMBE_COLS]);

    int veda_key_primary_material_init_model(veda_case8_proof_t *case8);
    int veda_case8_build_key32_candidate_model(veda_case8_proof_t *case8);
    int veda_case8_build_tweak64_candidate_model(veda_case8_proof_t *case8);

#ifdef __cplusplus
}
#endif

#endif /* VEDA_H */
