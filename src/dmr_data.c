/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "dsd.h"
#include "dsd_veda.h"


static FILE *g_288_hex_fp = NULL;
static FILE *g_288_bit_fp = NULL;
static unsigned g_288_session_no = 0;
static int g_288_is_open = 0;

static void pack_bits_to_bytes(const uint8_t *bits, int nbits, uint8_t *out)
{
  int i;
  int nbytes = (nbits + 7) / 8;

  memset(out, 0, nbytes);

  for (i = 0; i < nbits; i++)
  {
    if (bits[i] & 1)
      out[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
  }
}

static void build_ms_data_raw36(const uint8_t cachdata[25],
                                const char info[196],
                                const char syncdata[48],
                                const unsigned char SlotType[20],
                                uint8_t raw36[36])
{
  uint8_t bits288[288];
  int i;

  memset(bits288, 0, sizeof(bits288));

  for (i = 0; i < 24; i++)
    bits288[i] = cachdata[i] & 1;

  for (i = 0; i < 98; i++)
    bits288[24 + i] = info[i] & 1;

  for (i = 0; i < 10; i++)
    bits288[122 + i] = SlotType[i] & 1;

  for (i = 0; i < 48; i++)
    bits288[132 + i] = syncdata[i] & 1;

  for (i = 0; i < 10; i++)
    bits288[180 + i] = SlotType[10 + i] & 1;

  for (i = 0; i < 98; i++)
    bits288[190 + i] = info[98 + i] & 1;

  pack_bits_to_bytes(bits288, 288, raw36);
}

static void log_ms_data_288(uint8_t db, const uint8_t raw36[36])
{
  int i;

  fprintf(stderr, "[MS DATA 288] db=0x%02X ", db);
  for (i = 0; i < 36; i++)
    fprintf(stderr, "%02X", raw36[i]);
  fprintf(stderr, "\n");
}

static void veda_burst288_close_files(void)
{
  if (g_288_hex_fp != NULL)
  {
    fclose(g_288_hex_fp);
    g_288_hex_fp = NULL;
  }

  if (g_288_bit_fp != NULL)
  {
    fclose(g_288_bit_fp);
    g_288_bit_fp = NULL;
  }
}

static int veda_burst288_open_files(void)
{
  char path_hex[512];
  char path_bit[512];
  const char *home = getenv("HOME");

  if (home == NULL || home[0] == '\0')
    home = ".";

  g_288_session_no++;
  snprintf(path_hex, sizeof(path_hex), "%s/TYT_288_%u.hex", home, g_288_session_no);
  snprintf(path_bit, sizeof(path_bit), "%s/TYT_288_%u.bit", home, g_288_session_no);

  g_288_hex_fp = fopen(path_hex, "wb");
  if (g_288_hex_fp == NULL)
    return 0;

  g_288_bit_fp = fopen(path_bit, "wb");
  if (g_288_bit_fp == NULL)
  {
    fclose(g_288_hex_fp);
    g_288_hex_fp = NULL;
    return 0;
  }

  return 1;
}

void veda_burst288_stream_write(uint8_t db, const uint8_t raw36[36])
{
  int i, b;

 if (db == 0x01 && !g_288_is_open)
  {
    if (!veda_burst288_open_files())
      return;

    g_288_is_open = 1;
  }

  if (g_288_hex_fp == NULL || g_288_bit_fp == NULL)
    return;
  if(db==0xEB || db==0x00) {
    for (i = 0; i < 36; i++)
      fprintf(g_288_hex_fp, "%02X", raw36[i]);

    for (i = 0; i < 36; i++)
    {
      for (b = 7; b >= 0; b--)
        fputc(((raw36[i] >> b) & 1) ? '1' : '0', g_288_bit_fp);
    }
  }
  fflush(g_288_hex_fp);
  fflush(g_288_bit_fp);


  if (db == 0x02 && g_288_is_open)
  {
    veda_burst288_close_files();
    g_288_is_open = 0;
  }
}
/*
static void pack_bits_to_bytes(const uint8_t *bits, int nbits, uint8_t *out)
{
  int i;
  int nbytes = (nbits + 7) / 8;

  memset(out, 0, nbytes);

  for (i = 0; i < nbits; i++)
  {
    if (bits[i] & 1)
      out[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
  }
}

static void log_ms_data_burst_288(uint8_t db,
                                  const uint8_t cachdata[25],
                                  const char info[196],
                                  const char syncdata[48],
                                  const unsigned char SlotType[20])
{
  uint8_t bits288[288];
  uint8_t raw36[36];
  int i;

  memset(bits288, 0, sizeof(bits288));

  // 24 bits CACH
  for (i = 0; i < 24; i++)
    bits288[i] = cachdata[i] & 1;

  // 98 bits info A 
  for (i = 0; i < 98; i++)
    bits288[24 + i] = info[i] & 1;

  // 10 bits slot type A 
  for (i = 0; i < 10; i++)
    bits288[122 + i] = SlotType[i] & 1;

  // 48 bits sync/data sync 
  for (i = 0; i < 48; i++)
    bits288[132 + i] = syncdata[i] & 1;

  // 10 bits slot type B
  for (i = 0; i < 10; i++)
    bits288[180 + i] = SlotType[10 + i] & 1;

  // 98 bits info B
  for (i = 0; i < 98; i++)
    bits288[190 + i] = info[98 + i] & 1;

  pack_bits_to_bytes(bits288, 288, raw36);

  fprintf(stderr, "\n[MS DATA 288] db=0x%02X ", db);
  for (i = 0; i < 36; i++)
    fprintf(stderr, "%02X", raw36[i]);
  fprintf(stderr, "\n");
}
*/

void
dmr_data_sync (dsd_opts * opts, dsd_state * state)
{

  int i, dibit;
  int *dibit_p;
  char sync[25];
  char syncdata[48];
  uint8_t cachdata[25];
  UNUSED(syncdata);

  uint8_t burst;
  char info[196];
  unsigned char SlotType[20];
  unsigned int SlotTypeOk;
  uint8_t cach_err = 0;
  UNUSED(cach_err);

  int cachInterleave[24] =
  {0, 7, 8, 9, 1, 10,
   11, 12, 2, 13, 14,
   15, 3, 16, 4, 17, 18,
   19, 5, 20, 21, 22, 6, 23
  };

  dibit_p = state->dmr_payload_p - 90;

  //collect cach and de-interleave
  for (i = 0; i < 12; i++)
  {
    dibit = *dibit_p;
    dibit_p++;
    if (opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2);
    }
    if (state->dmr_stereo == 1)
    {
      dibit = (int)state->dmr_stereo_payload[i];
    }
    else state->dmr_stereo_payload[i] = dibit;

    cachdata[cachInterleave[(i * 2)]] = (1 & (dibit >> 1)); // bit 1
    cachdata[cachInterleave[(i * 2) + 1]] = (1 & dibit);    // bit 0
  }

  //seperate tact bits from cach
  uint8_t tact_bits[7];
  for (i = 0; i < 7; i++)
  {
    tact_bits[i] = cachdata[i];
  }

  //run hamming on tact bits
  int cach_okay = -1;
  if ( Hamming_7_4_decode (tact_bits) ) cach_okay = 1;
  else
  {
    cach_okay = -1;
    SlotTypeOk = 0;
    goto END;
  }

  state->currentslot = tact_bits[1];

  //in the future, maybe we will remove the hard set on this
  if (state->dmr_ms_mode == 1)
  {
    state->currentslot = 0;
  }

  // Current slot - First half - Data Payload - 1st part
  for (i = 0; i < 49; i++)
  {
    dibit = *dibit_p;
    dibit_p++;
    if (opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2);
    }
    if (state->dmr_stereo == 1)
    {
      dibit = (int)state->dmr_stereo_payload[i+12];
    }
    else state->dmr_stereo_payload[i+12] = dibit;
    info[2*i]     = (1 & (dibit >> 1));  // bit 1
    info[(2*i)+1] = (1 & dibit);         // bit 0
  }

  // slot type
  dibit = *dibit_p;
  dibit_p++;
  if (opts->inverted_dmr == 1)
  {
    dibit = (dibit ^ 2);
  }
  if (state->dmr_stereo == 1)
  {
    dibit = (int)state->dmr_stereo_payload[61];
  }
  else state->dmr_stereo_payload[61] = dibit;

  SlotType[0] = (1 & (dibit >> 1)); // bit 1
  SlotType[1] = (1 & dibit);        // bit 0

  dibit = *dibit_p;
  dibit_p++;
  if (opts->inverted_dmr == 1)
  {
    dibit = (dibit ^ 2);
  }
  if (state->dmr_stereo == 1)
  {
    dibit = (int)state->dmr_stereo_payload[62];
  }
  else state->dmr_stereo_payload[62] = dibit;

  SlotType[2] = (1 & (dibit >> 1)); // bit 1
  SlotType[3] = (1 & dibit);        // bit 0

  dibit = *dibit_p;
  dibit_p++;
  if (opts->inverted_dmr == 1)
  {
    dibit = (dibit ^ 2);
  }
  if (state->dmr_stereo == 1) //state
  {
    dibit = (int)state->dmr_stereo_payload[63];
  }
  else state->dmr_stereo_payload[63] = dibit;

  SlotType[4]  = (1 & (dibit >> 1)); // bit 1
  SlotType[5]  = (1 & dibit);        // bit 0

  dibit = *dibit_p;
  dibit_p++;
  if (opts->inverted_dmr == 1)
  {
    dibit = (dibit ^ 2);
  }
  if (state->dmr_stereo == 1) //state
  {
    dibit = (int)state->dmr_stereo_payload[64];
  }
  else state->dmr_stereo_payload[64] = dibit;

  SlotType[6]  = (1 & (dibit >> 1)); // bit 1
  SlotType[7]  = (1 & dibit);        // bit 0

  // Parity bit
  dibit = *dibit_p;
  dibit_p++;
  if (opts->inverted_dmr == 1)
  {
    dibit = (dibit ^ 2);
  }
  if (state->dmr_stereo == 1)
  {
    dibit = (int)state->dmr_stereo_payload[65];
  }
  else state->dmr_stereo_payload[65] = dibit;
  SlotType[8] = (1 & (dibit >> 1)); // bit 1
  SlotType[9] = (1 & dibit);        // bit 0

  // signaling data or sync
  for (i = 0; i < 24; i++)
  {
    dibit = *dibit_p;
    dibit_p++;
    if (opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2);
    }
    if (state->dmr_stereo == 1)
    {
      dibit = (int)state->dmr_stereo_payload[i+66];
    }
    else state->dmr_stereo_payload[i+66] = dibit;

    syncdata[2*i]     = (1 & (dibit >> 1));  // bit 1
    syncdata[(2*i)+1] = (1 & dibit);         // bit 0
    sync[i] = (dibit | 1) + 48;
  }
  sync[24] = 0;

  if((strcmp (sync, DMR_BS_DATA_SYNC) == 0) )
  {
    if (state->currentslot == 0)
    {
      sprintf(state->slot1light, "[slot1]");
      sprintf(state->slot2light, " slot2 ");
      
      ippl_add("slot", "2");//IPP
    }
    else
    {
      sprintf(state->slot1light, " slot1 ");
      sprintf(state->slot2light, "[slot2]");

      ippl_add("slot", "1");//IPP      
    }
  }

  else if(strcmp (sync, DMR_DIRECT_MODE_TS1_DATA_SYNC) == 0)
  {
    state->currentslot = 0;
    sprintf(state->slot1light, "[sLoT1]");
    sprintf(state->slot2light, "[DMODE]");
    ippl_add("slot_TS", "1");//IPP     
  }

  else if(strcmp (sync, DMR_DIRECT_MODE_TS2_DATA_SYNC) == 0)
  {
    state->currentslot = 1;
    sprintf(state->slot1light, "[DMODE]");
    sprintf(state->slot2light, "[sLoT2]");
    ippl_add("slot_TS", "2");//IPP
  }

  if (state->dmr_ms_mode == 0)
  {
    fprintf(stderr, "%s %s ", state->slot1light, state->slot2light);
  }

  // Slot type - Second part - Parity bit
  for (i = 0; i < 5; i++)
  {
    if (state->dmr_stereo == 0)
    {
      dibit = getDibit(opts, state);
      state->dmr_stereo_payload[i+90] = dibit;
    }
    if (state->dmr_stereo == 1)
    {
      dibit = (int)state->dmr_stereo_payload[i+90];
    }
    SlotType[(i*2) + 10] = (1 & (dibit >> 1)); // bit 1
    SlotType[(i*2) + 11] = (1 & dibit);        // bit 0
  }

  /* Check and correct the SlotType (apply Golay(20,8) FEC check) */

  // golay (20,8) hamming-weight of 6 reliably corrects at most 2 bit-errors
  if( Golay_20_8_decode(SlotType) ) SlotTypeOk = 1;
  else
  {
    SlotTypeOk = 0;
    goto END;
  }

  state->color_code = (SlotType[0] << 3) + (SlotType[1] << 2) +(SlotType[2] << 1) + (SlotType[3] << 0);
  state->color_code_ok = SlotTypeOk;

  //not sure why I still have two variables for this, need to look and see what state->color_code still ties into
  if (SlotTypeOk == 1) state->dmr_color_code = state->color_code;

  /* Reconstitute the burst type */
  burst = (uint8_t)((SlotType[4] << 3) + (SlotType[5] << 2) + (SlotType[6] << 1) + SlotType[7]);
  if (state->currentslot == 0) state->dmrburstL = burst;
  if (state->currentslot == 1) state->dmrburstR = burst;


  // Current slot - Second Half - Data Payload - 2nd part
  for (i = 0; i < 49; i++)
  {
    if (state->dmr_stereo == 0)
    {
      dibit = getDibit(opts, state);
      state->dmr_stereo_payload[i+95] = dibit;
    }

    if (state->dmr_stereo == 1)
    {
      dibit = (int)state->dmr_stereo_payload[i+95];
    }

    info[(2*i) + 98] = (1 & (dibit >> 1));  // bit 1
    info[(2*i) + 99] = (1 & dibit);         // bit 0
  }
  
  if(opts->veda_debug && (burst == 0x01 || burst == 0x02)) {
    // log_ms_data_burst_288(burst, cachdata, info, syncdata, SlotType);
    uint8_t raw36[36];

    build_ms_data_raw36(cachdata, info, syncdata, SlotType, raw36);

    log_ms_data_288(burst, raw36);

    veda_burst288_stream_write(burst, raw36);
  }

  dmr_data_burst_handler(opts, state, (uint8_t *)info, burst);

  //don't run cach on simplex or mono
  if (state->dmr_ms_mode == 0 && opts->dmr_mono == 0)
  {
    cach_err = dmr_cach (opts, state, cachdata);
  }

  //ending line break
  fprintf(stderr, "\n");

  END:
  if (SlotTypeOk == 0 || cach_okay != 1)
  {
    fprintf (stderr, "%s", KRED);
    //IPP
    ippl_add("err", "1"); 
    ippl_add("errv", "CACH/Burst FEC ERR");
        
    fprintf (stderr, "| CACH/Burst FEC ERR");
    fprintf (stderr, "%s", KNRM);
    fprintf (stderr, "\n");
    dmr_reset_blocks (opts, state); //failsafe to reset all data header and blocks when bad tact or slottype
  }

  // Skip cach (24 bit = 12 dibit) and next slot 1st half (98 + 10 bit = 49 + 5 dibit)
  if (state->dmr_stereo == 0)
  {
    skipDibit (opts, state, 12 + 49 + 5);
  }

  #define CON_TUNEAWAY //disable if any unlock issues noted on the logic

  #ifdef CON_TUNEAWAY
  //simplified, if IDLE condition, drop cc and vc sync time so when RF channel tears down, we go back much quicker
  //NOTE: This was adopted, because going back to the CC early and then to be sent back to the channel grant,
  //only for it to tear down and being sent back to the CC again isn't very efficient for trunk tracking
  //NOTE: This will still leave the tuner in the 'locked' state when tuning a voice channel grant on the CC,
  //and will remain locked until a new voice channel grant is received, but its just asthetic, trying to fix it
  //is too much of a hassle and causes other issues like CC hunting, etc.
  if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && state->is_con_plus == 1)
  {
    int clear = 0;
    //IF both slots currently signalling IDLE
    if (state->dmrburstL == 9 && state->dmrburstR == 9) clear = 1;
    if (clear == 1)
    {
      state->last_cc_sync_time = 0;
      state->last_vc_sync_time = 0;
    }
  }
  
  #endif
}
