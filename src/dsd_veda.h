#ifndef DSD_VEDA_H
#define DSD_VEDA_H

#include "dsd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Все инклуды С-библиотек только здесь
#include <hydrogen.h>

// Все прототипы только здесь и только один раз
void veda_permute_384(uint32_t *state, uint8_t domain);
void veda_stream_init(dsd_state *state, int slot, uint8_t *session_key);
void veda_apply_mi(dsd_state *state, int slot, uint64_t mi);
void veda_decrypt_ambe(dsd_state *state, int slot, char ambe_fr[4][24]);
void handle_veda_kx_packet(dsd_opts *opts, dsd_state *state, uint8_t *payload);
// void veda_decrypt_bits(dsd_state *state, int slot, uint8_t *bits, int count);
// void veda_decrypt_ambe_matrix(dsd_opts *opts, dsd_state *state, int slot, char ambe_fr[4][24]);
void veda_prepare_voice_ctx(dsd_opts *opts, dsd_state *state, int slot, uint64_t mi);

uint64_t veda_get_effective_mi(dsd_state *state, int slot);

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