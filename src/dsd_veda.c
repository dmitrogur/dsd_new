#include "dsd_veda.h"

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

void veda_note_raw_src_tgt(dsd_state *state, int slot, uint32_t source, uint32_t target)
{
    if (!state || slot < 0 || slot > 1)
        return;

    state->veda_raw_src[slot] = source;
    state->veda_raw_tgt[slot] = target;
}

int veda_try_build_tx_subst_frame(dsd_state *state, int slot)
{
    uint32_t id24;
    uint8_t *buf;

    if (!state || slot < 0 || slot > 1)
        return 0;

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

    /*
      Нас интересует только "семейство" заголовков,
      которое в extract уходит в ветку:
        sub_80008BA / sub_8000968 / sub_8000E44
      то есть (b0 & 0x60) == 0x20.
    */
    if ((hdr->b0 & 0x60) != 0x20)
        return 0;

    switch (hdr->b1)
    {
    /*
      Аналог sub_80008BA():
      - если w6 == 0 и state в {2,5} -> переход в 6
      - если w6 != 0 и state в {2,5} -> сохранить w2/w6 и перейти в 3
    */
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

    /*
      Аналог sub_8000968() в упрощённом виде:
      тут не строим substitution frame,
      а только обновляем state/len.
    */
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

    /*
      Аналог sub_8000E44():
      - при sm == 3 и len_hi != 0 -> переход в 4 и попытка собрать 6-байтный buf
      - при sm == 3 и len_hi == 0 -> переход в 5
      - при sm == 6 -> переход в 7
    */
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

            /*
              Упрощённый селектор:
              exact-логика в прошивке ещё зависит от текущего шаблона команды
              (0x41 / 0x91 / 0xD5 / 0xB4 / 0xB3 / 0xB2 и т.д.).
              Пока берём:
                len_lo == 0 -> sel=1
                иначе       -> sel=0
            */
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

    void veda_dump_state(dsd_state * state, int slot)
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
}