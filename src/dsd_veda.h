#ifndef DSD_VEDA_H
#define DSD_VEDA_H

#include "dsd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Все инклуды С-библиотек только здесь
#include <hydrogen.h>
static const uint32_t veda_masks[] = {0x17C20B2A, 0x56456023, 0x4794E038, 0x8BC3C444};

typedef struct {
  uint8_t last_db[3];
  uint8_t db_count;
  uint32_t db_01_eb_01_hits;
  uint8_t db_pattern_seen;
  uint32_t a37_hits[6][2];
  uint32_t a37_total[6];
  uint8_t a37_seen;  
} veda_trait_slot_t;

static veda_trait_slot_t g_veda_trait[2];

void veda_trait_reset_all(void);
void veda_trait_reset_slot(int slot);
void veda_trait_note_db(int slot, uint8_t databurst);
int veda_trait_db_pattern_seen(int slot);
uint32_t veda_trait_db_pattern_hits(int slot);
void veda_trait_note_a37_slot(int slot, int burst_phase_1_6, uint8_t a37_bit);
int veda_trait_a37_seen(int slot);
uint32_t veda_trait_a37_hits(int slot, int phase_1_6, int bit);
uint32_t veda_trait_a37_total(int slot, int phase_1_6);
void veda_trait_note_ms_a37(int slot, uint32_t sf_idx, const char ambe_fr[4][24]);
int veda_trait_a37_phase_majority_bit(int slot, int phase_1_6);
int veda_trait_a37_phase_conf_pct(int slot, int phase_1_6);
int veda_trait_a37_seen_enough(int slot);

// Все прототипы только здесь и только один раз
void veda_permute_384(uint32_t *state, uint8_t domain);
void veda_stream_init(dsd_state *state, int slot, uint8_t *session_key);
void veda_apply_mi(dsd_state *state, int slot, uint64_t mi);
void veda_decrypt_ambe(dsd_state *state, int slot, char ambe_fr[4][24]);
void handle_veda_kx_packet(dsd_opts *opts, dsd_state *state, uint8_t *payload);
// void veda_decrypt_bits(dsd_state *state, int slot, uint8_t *bits, int count);
// void veda_decrypt_ambe_matrix(dsd_opts *opts, dsd_state *state, int slot, char ambe_fr[4][24]);
void veda_prepare_voice_ctx(dsd_opts *opts, dsd_state *state, int slot, uint64_t mi);

int veda_try_decrypt_voice_triplet(dsd_opts *opts,
                                   dsd_state *state,
                                   int slot,
                                   char ambe_fr[4][24],
                                   char ambe_fr2[4][24],
                                   char ambe_fr3[4][24]);

void veda_debug_voice_wait(dsd_opts *opts,
                           dsd_state *state,
                           int slot,
                           int sf_cur,
                           int sf_total);

void veda_trace_probe_air_header(dsd_opts *opts,
                                 dsd_state *state,
                                 int slot,
                                 const uint8_t *buf,
                                 uint8_t len,
                                 const char *tag,
                                 int sf_cur);

void veda_trace_rejected_air_header(dsd_opts *opts,
                                    dsd_state *state,
                                    int slot,
                                    uint8_t databurst,
                                    const uint8_t *buf,
                                    uint8_t len,
                                    uint32_t crc_ok,
                                    uint32_t irr_err,
                                    int sf_cur);

uint64_t veda_get_effective_mi(dsd_state *state, int slot);

int veda_try_session_bridge(dsd_opts *opts, dsd_state *state, int slot);

int veda_session_key_valid(const dsd_state *state, int slot);
int veda_stream_ctx_valid(const dsd_state *state, int slot);
uint8_t *veda_session_material_ptr(dsd_state *state, int slot);
const uint8_t *veda_session_material_cptr(const dsd_state *state, int slot);

void veda_trace_baseline(dsd_opts *opts,
                         dsd_state *state,
                         int slot,
                         const char *tag,
                         int sf_cur,
                         int sf_total);

void veda_note_candidate(dsd_opts *opts,
                         dsd_state *state,
                         int slot,
                         uint8_t source_type,
                         const uint8_t *payload,
                         uint8_t payload_len,
                         int sf_cur);

void veda_clear_candidate(dsd_state *state, int slot);                         


void veda_raw_reset_slot(dsd_state *state, int slot);
void veda_raw_begin_if_needed(dsd_state *state, int slot, uint16_t sf, uint8_t first_kind);
void veda_raw_close_if_needed(dsd_state *state, int slot, uint16_t sf, uint8_t reason);

void veda_raw_log_mbc(dsd_opts *opts, dsd_state *state, int slot,
                      uint8_t kind, uint8_t databurst,
                      const uint8_t *raw, uint8_t len,
                      uint8_t crc_ok, uint8_t irr_err,
                      uint8_t blockcounter, uint8_t blocks,
                      uint8_t lb, uint8_t pf, uint16_t sf);

void veda_raw_log_db(dsd_opts *opts, dsd_state *state, int slot,
                     uint8_t databurst,
                     const uint8_t *raw, uint8_t len,
                     uint8_t crc_ok, uint8_t irr_err,
                     uint16_t sf);

void veda_raw_log_lc(dsd_opts *opts, dsd_state *state, int slot,
                     uint8_t kind,
                     const uint8_t *raw, uint8_t len,
                     uint8_t crc_ok, uint8_t irr_err,
                     uint8_t flco, uint8_t fid, uint8_t so,
                     uint16_t sf);

void veda_raw_log_cach(dsd_opts *opts, dsd_state *state, int slot,
                       uint32_t tact_raw,
                       uint8_t tact_ok, uint8_t at, uint8_t tdma_slot, uint8_t lcss,
                       const uint8_t *raw, uint8_t len,
                       uint16_t sf);

void veda_raw_log_mi(dsd_opts *opts, dsd_state *state, int slot,
                     uint32_t mi32, uint16_t sf);

void veda_bridge_voice_dyn32(uint32_t voice_dyn32, const char *source);
// инлайны
static inline uint32_t dmr_u24_le_read(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static inline void dmr_u24_le_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF);
}
static inline void veda_pack_selected_id24(uint8_t out6[6], uint32_t id24) {
    dmr_u24_le_write(&out6[1], id24 & 0xFFFFFFu);
}
static inline uint32_t veda_pick_profile_id24(const dsd_state *state, int slot, int sel) {
    if (sel & 1) return state->veda_id24_a[slot] & 0xFFFFFFu;
    else return state->veda_id24_b[slot] & 0xFFFFFFu;
}


#ifdef __cplusplus
}
#endif

#endif