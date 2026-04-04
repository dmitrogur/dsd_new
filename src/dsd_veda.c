#include "dsd_veda.h"

static int veda_get_live_ids(const dsd_state *state, int slot, uint32_t *id24_a, uint32_t *id24_b);
static void veda_refresh_profile_from_live_ids(dsd_state *state, int slot);
static int veda_get_live_ids(const dsd_state *state, int slot, uint32_t *id24_a, uint32_t *id24_b);
static void veda_refresh_profile_from_live_ids(dsd_state *state, int slot);

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
                "\nVEDA SM slot=%d sm=%u len_lo=%u len_hi=%u",
                slot + 1,
                state->veda_sm[slot],
                state->veda_len_lo[slot],
                state->veda_len_hi[slot]);
        break;

    case 3:
        fprintf(stderr,
                "\nVEDA IDS slot=%d id_a=0x%06X id_b=0x%06X",
                slot + 1,
                state->veda_id24_a[slot] & 0xFFFFFFu,
                state->veda_id24_b[slot] & 0xFFFFFFu);
        break;

    case 4:
        fprintf(stderr,
                "\nVEDA SUBST slot=%d sel=%u raw_src=%u raw_tgt=%u id24=0x%06X buf=%02X %02X %02X %02X %02X %02X",
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
                "\nVEDA WARN slot=%d subst-build-skipped sm=%u len_hi=%u",
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
    case VEDA_HDRSRC_TLC:     return 50;
    case VEDA_HDRSRC_VLC:     return 50;
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

    if (!opts || !state || !hdr || slot < 0 || slot > 1)
        return -1;

    if (!opts->isVEDA)
        return 0;

    veda_store_last_hdr(state, slot, hdr, src_kind);

    rc = veda_control_header_handler(opts, state, slot, hdr);

    if (rc == 0 && veda_can_try_normalized_b0(src_kind, hdr))
    {
        veda_air_header_t norm = *hdr;
        norm.b0 = (uint8_t)((norm.b0 & 0x9Fu) | 0x20u);

        if (norm.b0 != hdr->b0)
        {
            int rc2 = veda_control_header_handler(opts, state, slot, &norm);
            if (rc2 != 0)
            {
                veda_store_last_hdr(state, slot, &norm, src_kind);
                rc = rc2;

                if (state->veda_debug)
                {
                    fprintf(stderr,
                            "\nVEDA HDR slot=%d src=%u rc=%d norm=1 b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X",
                            slot + 1, src_kind, rc,
                            norm.b0, norm.b1, norm.w2, norm.w4, norm.w6);
                }

                return rc;
            }
        }
    }

    if (state->veda_debug && rc != 0)
    {
        fprintf(stderr,
                "\nVEDA HDR slot=%d src=%u rc=%d norm=0 b0=%02X b1=%02X w2=%04X w4=%04X w6=%04X",
                slot + 1, src_kind, rc,
                hdr->b0, hdr->b1, hdr->w2, hdr->w4, hdr->w6);
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
            "\nVEDA STATE slot=%d sm=%u len_lo=%u len_hi=%u raw_src=%u raw_tgt=%u id_a=0x%06X id_b=0x%06X subst=%u",
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
