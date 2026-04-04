#ifndef DSD_VEDA_H
#define DSD_VEDA_H

#include "dsd.h"

/* только то, чего нет в dsd.h */

static inline uint32_t dmr_u24_le_read(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16);
}

static inline void dmr_u24_le_write(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
}

static inline void veda_pack_selected_id24(uint8_t out6[6], uint32_t id24)
{
  dmr_u24_le_write(&out6[1], id24 & 0xFFFFFFu);
}

static inline uint32_t veda_pick_profile_id24(const dsd_state *state, int slot, int sel)
{
  if (sel & 1)
    return state->veda_id24_a[slot] & 0xFFFFFFu;
  else
    return state->veda_id24_b[slot] & 0xFFFFFFu;
}

#endif
