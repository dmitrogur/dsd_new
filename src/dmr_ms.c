    /*-------------------------------------------------------------------------------
 * dmr_ms.c
 * DMR MS/Simplex/Direct Mode Voice Handling and Data Gathering Routines
 *
 * DMH/IPP
 *-----------------------------------------------------------------------------*/

#include "dsd.h"
#include "dmr_const.h"
#include "avr_kv.h"
#include "dsd_veda.h"
#include <string.h>
// #define PRINT_AMBE72 //enable to view 72-bit AMBE codewords

static void dmr_ms_unpack_cach_bits_from_dibits(const char *cach_dibits,
                                                uint8_t cach_bits[25])
{
  static const int cach_interleave[24] = {
    0, 7, 8, 9, 1, 10,
    11, 12, 2, 13, 14, 15,
    3, 16, 4, 17, 18, 19,
    5, 20, 21, 22, 6, 23
  };

  int i;

  memset(cach_bits, 0, 25);

  if (cach_dibits == NULL)
    return;

  for (i = 0; i < 12; i++)
  {
    uint8_t dibit = (uint8_t)cach_dibits[i] & 0x03;
    cach_bits[cach_interleave[(i * 2) + 0]] = (uint8_t)((dibit >> 1) & 0x01);
    cach_bits[cach_interleave[(i * 2) + 1]] = (uint8_t)(dibit & 0x01);
  }
}

//A subroutine for processing MS voice
void dmrMS (dsd_opts * opts, dsd_state * state)
{

  char * timestr = getTimeC();

  int i, j, dibit;
  char ambe_fr[4][24];
  char ambe_fr2[4][24];
  char ambe_fr3[4][24];
  char ambe_fr4[4][24];

  //memcpy of ambe_fr for late entry
  uint8_t m1[4][24];
  uint8_t m2[4][24];
  uint8_t m3[4][24];

  const int *w, *x, *y, *z;

  uint8_t syncdata[48];
  memset (syncdata, 0, sizeof(syncdata));

  uint8_t emb_pdu[16];
  memset (emb_pdu, 0, sizeof(emb_pdu));

  //cach
  char cachdata[25];

  //cach tact bits
  uint8_t tact_bits[7];

  uint8_t tact_okay = 0;
  uint8_t emb_ok = 0;
  UNUSED(emb_ok);

  uint8_t internalslot;
  uint8_t vc;

  //assign as nonsensical numbers
  uint8_t cc = 25;
  uint8_t power = 9; //power and pre-emption indicator
  uint8_t lcss = 9;
  UNUSED2(cc, lcss);

  //dummy bits to pass to dburst for link control
  uint8_t dummy_bits[196];
  memset (dummy_bits, 0, sizeof (dummy_bits));

  vc = 2;

  //Hardset variables for MS/Mono
  state->currentslot = 0; //0

  //Note: Manual dibit inversion required here since I didn't seperate inverted return from normal return in framesync,
  //so getDibit doesn't know to invert it before it gets here
  //DMH
  // kc_reset(state);
  for (j = 0; j < 6; j++) {
  //IPP
  ipp_last_sample_num();
  
  state->dmrburstL = 16;

  memset (ambe_fr, 0, sizeof(ambe_fr));
  memset (ambe_fr2, 0, sizeof(ambe_fr2));
  memset (ambe_fr3, 0, sizeof(ambe_fr3));

  for(i = 0; i < 12; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;

    cachdata[i] = dibit;
    state->dmr_stereo_payload[i] = dibit;
  }

  for (i = 0; i < 7; i++)
  {
    tact_bits[i] = cachdata[i];
  }

  tact_okay = 0;
  if ( Hamming_7_4_decode (tact_bits) ) tact_okay = 1;
  if (tact_okay != 1)
  {
    //do nothing since we aren't loop locked forever.
  }
  //internalslot = tact_bits[1];
  internalslot = 0;

  //Setup for first AMBE Frame
  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;
  // First AMBE Frame, Full 36  -> dmr_stereo_payload[12..47]
  for(i = 0; i < 36; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;

    state->dmr_stereo_payload[i+12] = dibit;

    ambe_fr[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  //Setup for Second AMBE Frame
  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;

  // Second AMBE Frame, Part 1 (18 dibits) -> dmr_stereo_payload[48..65]
  //Second AMBE Frame, First Half 18 dibits just before Sync or EmbeddedSignalling
  for(i = 0; i < 18; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;

    state->dmr_stereo_payload[i+48] = dibit;
    ambe_fr2[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr2[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  // signaling data or sync
  for(i = 0; i < 24; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2);

    state->dmr_stereo_payload[i+66] = dibit;

    syncdata[(2*i)]   = (1 & (dibit >> 1));  // bit 1
    syncdata[(2*i)+1] = (1 & dibit);         // bit 0

    // load the superframe to do embedded signal processing
    if(vc > 1) //grab on vc2 values 2-5 B C D E, and F
    {
      state->dmr_embedded_signalling[internalslot][vc-1][i*2]   = (1 & (dibit >> 1)); // bit 1
      state->dmr_embedded_signalling[internalslot][vc-1][i*2+1] = (1 & dibit); // bit 0
    }

  }

  for(i = 0; i < 8; i++) emb_pdu[i + 0] = syncdata[i];
  for(i = 0; i < 8; i++) emb_pdu[i + 8] = syncdata[i + 40];

  emb_ok = -1;
  if (QR_16_7_6_decode(emb_pdu))
  {
    emb_ok = 1;
    cc = ((emb_pdu[0] << 3) + (emb_pdu[1] << 2) + (emb_pdu[2] << 1) + emb_pdu[3]);
    power = emb_pdu[4];
    lcss = ((emb_pdu[5] << 1) + emb_pdu[6]);
    state->dmr_color_code = state->color_code = cc;
  }       
  else
  {
    emb_ok = 0;
  }

  //Continue Second AMBE Frame, 18 after Sync or EmbeddedSignalling
  // Second AMBE Frame, Part 2 (18 dibits) -> dmr_stereo_payload[90..107]
  for(i = 0; i < 18; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;

    state->dmr_stereo_payload[i+90] = dibit;
    ambe_fr2[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr2[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }
  //Setup for Third AMBE Frame
  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;

  // Third AMBE Frame, Full 36 -> dmr_stereo_payload[108..143]
  //Third AMBE Frame, Full 36
  for(i = 0; i < 36; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;

    state->dmr_stereo_payload[i+108] = dibit;
    ambe_fr3[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr3[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }
    //DMH
/*
if (opts->run_scout)  
{
  uint8_t enc27[27];
  if (scout_pack_27bytes_from_frames(ambe_fr, ambe_fr2, ambe_fr3, enc27) == 0) {
    // IV берем из текущего стейта (он уже выставлен для этого VC*)
    avr_scout_on_vc(state, enc27, state->aes_iv);
  }
}
*/  
if (opts->verbose > 2 && state->payload_algid == 0x25) // AES-256 (тест)
{
    // 1) Ключ (как в dsd_mbe.c)
    uint8_t aes_key[32];
    for (int i = 0; i < 8; i++) {
      aes_key[i+0]  = (state->A1[0] >> (56-(i*8))) & 0xFF;
      aes_key[i+8]  = (state->A2[0] >> (56-(i*8))) & 0xFF;
      aes_key[i+16] = (state->A3[0] >> (56-(i*8))) & 0xFF;
      aes_key[i+24] = (state->A4[0] >> (56-(i*8))) & 0xFF;
    }

    // 2) РОВНО 216 ЗАШИФРОВАННЫХ БИТ (4 диапазона). НИКАКОГО syncdata!
    uint8_t enc_bits[216];
    int bit_idx = 0;

    for (int i = 12;  i < 48;  i++) { enc_bits[bit_idx++] = (state->dmr_stereo_payload[i] >> 1) & 1;
                                      enc_bits[bit_idx++] =  state->dmr_stereo_payload[i]       & 1; }
    for (int i = 48;  i < 66;  i++) { enc_bits[bit_idx++] = (state->dmr_stereo_payload[i] >> 1) & 1;
                                      enc_bits[bit_idx++] =  state->dmr_stereo_payload[i]       & 1; }
    for (int i = 90;  i < 108; i++) { enc_bits[bit_idx++] = (state->dmr_stereo_payload[i] >> 1) & 1;
                                      enc_bits[bit_idx++] =  state->dmr_stereo_payload[i]       & 1; }
    for (int i = 108; i < 144; i++) { enc_bits[bit_idx++] = (state->dmr_stereo_payload[i] >> 1) & 1;
                                      enc_bits[bit_idx++] =  state->dmr_stereo_payload[i]       & 1; }

    // 216 бит -> 27 байт (важно: та же полярность, что ожидает дешифратор; обычно MSB-first)
    uint8_t enc_bytes[27] = {0};
    // pack_bit_array_into_byte_array_ta(enc_bits, enc_bytes, 27);
    pack_bit_array_into_byte_array(enc_bits, enc_bytes, 27);

    // 3) AES-OFB: KS = AES_ENC(IV); P = C ^ KS; IV := KS
    extern void aes_ecb_bytewise_payload_crypt(uint8_t *in, uint8_t *key,
                                               uint8_t *out, int type, int enc);
    uint8_t ofb[16];
    // если IV хранится per-slot — возьми нужный: state->aes_iv[slot]
    memcpy(ofb, state->aes_iv, 16);

    uint8_t dec_bytes[27];
    size_t produced = 0;
    while (produced < sizeof(dec_bytes)) {
      uint8_t ks_block[16];
      // ваша сигнатура: (in, key, out, type, de/enc) — нам НУЖЕН ENC, т.е. enc=1
      aes_ecb_bytewise_payload_crypt(ofb, aes_key, ks_block, 2, 1);
      size_t take = (sizeof(dec_bytes) - produced > 16) ? 16 : (sizeof(dec_bytes) - produced);
      for (size_t j = 0; j < take; j++)
        dec_bytes[produced + j] = enc_bytes[produced + j] ^ ks_block[j];
      memcpy(ofb, ks_block, 16); // OFB цепочка
      produced += take;
    }

    // 4) Отладочный дамп (временно)
    fprintf(stderr, "\n[OFB] DEC (27B): ");
    for (int j = 0; j < 27; j++) fprintf(stderr, "%02X", dec_bytes[j]);
    fprintf(stderr, "\n");

    // Ничего больше тут не трогаем — основной пайплайн пусть работает как есть.
}

  //'DSP' output to file
  if (opts->use_dsp_output == 1)
  {
    FILE * pFile; //file pointer
    pFile = fopen (opts->dsp_out_file, "a");
    fprintf (pFile, "\n%d 10 ", state->currentslot+1); //0x10 for "voice burst", forced to slot 1
    for (i = 6; i < 72; i++) //33 bytes, no CACH
    {
      int dsp_byte = (state->dmr_stereo_payload[i*2] << 2) | state->dmr_stereo_payload[i*2 + 1];
      fprintf (pFile, "%X", dsp_byte);
    }
    fclose (pFile);
  }
  //DMH
  //'DSP' output to file
  if (opts->use_dsp_output == 1)
  {
    FILE * pFile; //file pointer
    pFile = fopen (opts->dsp_out_file, "a");
    fprintf (pFile, "\n%d 10 ", state->currentslot+1); //0x10 for "voice burst", forced to slot 1
    for (i = 6; i < 72; i++) //33 bytes, no CACH
    {
      int dsp_byte = (state->dmr_stereo_payload[i*2] << 2) | state->dmr_stereo_payload[i*2 + 1];
      fprintf (pFile, "%X", dsp_byte);
    }
    fclose (pFile);
  }

  state->dmr_ms_mode = 1;

  memcpy (ambe_fr4, ambe_fr2, sizeof(ambe_fr2));

  //copy ambe_fr frames first, running process mbe will correct them,
  //but this also leads to issues extracting good le mi values when
  //we go to do correction on them there too
  memcpy (m1, ambe_fr, sizeof(m1));
  memcpy (m2, ambe_fr2, sizeof(m2));
  memcpy (m3, ambe_fr3, sizeof(m3));

    // Явно вызываем дешифратор для каждого голосового фрейма,
  // если мы в режиме AES и ключ был загружен нашей системой.
  if ((state->payload_algid == 0x24  || state->payload_algid == 0x23 || state->payload_algid == 0x25) && state->aes_key_loaded[state->currentslot & 1] == 1)
  {
    // kv_decrypt_ambe_frames(state, ambe_fr, ambe_fr2, ambe_fr3);
  }

  if (state->tyt_bp == 1)
  {
    tyt16_ambe2_codeword_keystream(state, ambe_fr, 0);
    tyt16_ambe2_codeword_keystream(state, ambe_fr2, 1);
    tyt16_ambe2_codeword_keystream(state, ambe_fr3, 0);
  }

  if (state->csi_ee == 1)
  {
    csi72_ambe2_codeword_keystream(state, ambe_fr);
    csi72_ambe2_codeword_keystream(state, ambe_fr2);
    csi72_ambe2_codeword_keystream(state, ambe_fr3);
  }

  int veda_voice_done = 0;
  if (opts->isVEDA)
  {
    veda_voice_done = veda_try_decrypt_voice_triplet(opts, state, 0,
                                                     ambe_fr, ambe_fr2, ambe_fr3);
  }

  #ifdef PRINT_AMBE72
  ambe2_codeword_print_i(opts, ambe_fr);
  ambe2_codeword_print_i(opts, ambe_fr2);
  ambe2_codeword_print_i(opts, ambe_fr3);
  #endif

  processMbeFrame (opts, state, NULL, ambe_fr, NULL);
    memcpy(state->f_l4[0], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[0], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[0], state->s_lu, sizeof(state->s_lu));
    state->kc_frames_total[0]++;
    if (state->payload_algid > 0x00) {
      if (state->errs2 >= 0 && state->errs2 <= 3) {
        state->kc_frames_ok[0]++;
      } else {
        state->kc_uncorrectable[0]++;
      }
    }

  processMbeFrame (opts, state, NULL, ambe_fr2, NULL);
    memcpy(state->f_l4[1], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[1], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[1], state->s_lu, sizeof(state->s_lu));
    state->kc_frames_total[0]++;
    if (state->payload_algid > 0x00) {
      if (state->errs2 >= 0 && state->errs2 <= 3) {
        state->kc_frames_ok[0]++;
      } else {
        state->kc_uncorrectable[0]++;
      }
    }
  processMbeFrame (opts, state, NULL, ambe_fr3, NULL);
    memcpy(state->f_l4[2], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[2], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[2], state->s_lu, sizeof(state->s_lu));
    state->kc_frames_total[0]++;
    if (state->payload_algid > 0x00) { // только когда шифруется
        if (state->errs2 >= 0 && state->errs2 <= 3) { 
          state->kc_frames_ok[0]++;
        } else {
          state->kc_uncorrectable[0]++;
        }
    }    
  
  if (opts->kv_csv_path[0] != '\0')    // Покадровый тикер перебора: реагирует на KEY_SET_NEXT/KEY_FAILED/KEY_VALIDATED
    kv_enum_on_frame(opts, state);    

  //if ((state->payload_algid == 0x24   || state->payload_algid == 0x23 || state->payload_algid == 0x25) &&  
  // if (  (state->currentslot == 0 && state->payload_algid == 0x24 && state->aes_key_loaded[0] == 1 ) || //DMR AES128
  //           (state->currentslot == 0 && state->payload_algid == 0x25 && state->aes_key_loaded[0] == 1 ) || //DMR AES256
            // ... остальная часть условия ...
  //        )
  if (state->payload_algid > 0x00) {
    int kid = state->payload_keyid;
    const int slot = (state->currentslot & 1);
    if (state->dmr_key_validation_status[slot][kid] == KEY_VALIDATED)
    {
      time_ms_t now_ms    = dsd_now_ms();
      time_ms_t t_key_ms  = (state->kv_key_t0_ms[slot][kid] > 0) ? now_ms - state->kv_key_t0_ms[slot][kid] : 0;
      time_ms_t t_total_ms= (state->kv_prog_t0_ms > 0)           ? now_ms - state->kv_prog_t0_ms : 0;
      if (t_key_ms   < 0) t_key_ms = 0;
      if (t_total_ms < 0) t_total_ms = 0;

      fprintf(stderr, "\nKEY_VALIDATED (MS) alg=0x%02X keyid=%02X (t_key=%" PRId64 " ms, t_total=%" PRId64 " ms)\n",
          (unsigned)state->payload_algid, kid, t_key_ms, t_total_ms);

      // kv_result.txt с учётом -jp
      char kvpath[600];
      kv_build_result_path(opts, "kv_result.txt", kvpath, sizeof(kvpath));
      FILE *f = fopen(kvpath, "a");
      if (f) {
        fprintf(f, "KEY_VALIDATED (MS) alg=0x%02X keyid=%d t_key_ms=%" PRId64 " t_total_ms=%" PRId64 " prob=%u\n",
            (unsigned)state->payload_algid, kid, t_key_ms, t_total_ms, (unsigned) state->kv_key_probability[slot][kid]);
      fclose(f);
      } 
      // keyOK_<id>.txt (ручной -H: ord=0, либо свой индекс)
      int ord = 0;
      kv_write_key_ok_file(opts, state, kid, ord);

      state->dmr_key_validation_status[slot][kid] = KEY_SUCCESS;

      if (opts->kv_exit_on_first_ok) {
        exitflag = 1; // аккуратный выход
      }
    }        
    else if (state->dmr_key_validation_status[slot][kid] == KEY_FAILED) {
        // ключ явно признан неверным → выход
        const int slot = state->currentslot & 1;
        time_ms_t now_ms    = dsd_now_ms();
        time_ms_t t_key_ms  = (state->kv_key_t0_ms[slot][kid] > 0) ? now_ms - state->kv_key_t0_ms[slot][kid] : 0;
        time_ms_t t_total_ms= (state->kv_prog_t0_ms > 0)           ? now_ms - state->kv_prog_t0_ms : 0;
        if (t_key_ms   < 0) t_key_ms = 0;
        if (t_total_ms < 0) t_total_ms = 0;

        // kv_result.txt с учётом -jp
        char kvpath[600];
        kv_build_result_path(opts, "kv_result.txt", kvpath, sizeof(kvpath));
        FILE *f = fopen(kvpath, "a");
        if (f) {
        fprintf(f, "KEY_FAILED alg=0x%02X keyid=%02X t_key_ms=%" PRId64 ", t_total_ms=%" PRId64 " prob=%u\n",
            (unsigned)state->payload_algid, kid, t_key_ms, t_total_ms, (unsigned) state->kv_key_probability[slot][kid]);
        fclose(f);
        } 
 
        exitflag = 1; // аккуратный выход при фейле
    }
  }  
  if(state->exit_after_batch) {
    exitflag = 1;
  } 
  /*  
  if (state->payload_algid > 0x00 && state->dmr_key_validation_status[state->payload_keyid] == KEY_UNKNOWN)
  {
    const int slot = state->currentslot & 1;
    const int kid  = (int)state->payload_keyid;

    if (state->kv_key_t0_ms[slot][kid] == 0) {
      state->kv_key_t0_ms[slot][kid] = dsd_now_ms();   // старт этой проверки
    }    
    if (state->kc_frames_ok[0] > 0) {
      float energy = 0.0f;
      int i;

      // Анализируем энергию в аудио-буфере (160 сэмплов типа float)
      //for (i = 0; i < 160; i++) 
      //    energy += state->audio_out_temp_buf[i] * state->audio_out_temp_buf[i];
      }
      // Считаем энергию для ВСЕХ 3-х аудио-фрагментов
      for (i = 0; i < 160; i++) energy += state->f_l4[0][i] * state->f_l4[0][i];
      for (i = 0; i < 160; i++) energy += state->f_l4[1][i] * state->f_l4[1][i];
      for (i = 0; i < 160; i++) energy += state->f_l4[2][i] * state->f_l4[2][i];      

      // Пороговое значение. Тишина будет иметь энергию ~0. Голос - значительно больше.
      // Это значение, возможно, придется немного подстроить, но 100.0f - это безопасное начало.
      float energy_threshold = 1000000.0f;

      if (energy > energy_threshold)
      {
          fprintf(stderr, "\n[+] AES KEY VALIDATION: SUCCESS for KID %d (Audio energy: %.2f, OK frames: %u/%u)\n",
                  state->payload_keyid, energy, state->kc_frames_ok[0], state->kc_frames_total[0]);
          state->dmr_key_validation_status[state->payload_keyid] = KEY_VALIDATED;
      }
      else
      {
          fprintf(stderr, "\n[-] AES KEY VALIDATION: FAILED for KID %d (Silence detected, energy: %.2f)\n",
                  state->payload_keyid, energy);
          state->dmr_key_validation_status[state->payload_keyid] = KEY_FAILED;
      }
    }    
    else
    {
        // Если все фреймы были "плохими", ключ yt проверяем.
        fprintf(stderr, "\n[-] AES KEY VALIDATION: FAILED for KID %d (OK Frames: %d/%d)\n", 
                state->payload_keyid, state->kc_frames_ok[0], state->kc_frames_total[0]);
        state->dmr_key_validation_status[state->payload_keyid] = KEY_UNKNOWN;
    }
  }
  */

  //TODO: Consider copying f_l to f_r for left and right channel saturation on MS mode
  if (opts->floating_point == 0)
  {
    // memcpy (state->s_r4, state->s_l4, sizeof(state->s_l4));
    if(opts->pulse_digi_out_channels == 2)
      playSynthesizedVoiceSS3(opts, state);
  }

  if (opts->floating_point == 1)
  {
    // memcpy (state->f_r4, state->f_l4, sizeof(state->f_l4));
    if(opts->pulse_digi_out_channels == 2)
      playSynthesizedVoiceFS3(opts, state);
  }

if (opts->isVEDA) {
    uint8_t raw_64[36]; // 288 бит полезной нагрузки MS кадра
    memset(raw_64, 0, 36);
    // dmr_stereo_payload хранит 144 дибита (288 бит) кадра
    for (int i = 0; i < 144; i++) {
        uint8_t dibit = state->dmr_stereo_payload[i];
        int bit_idx = i * 2;
        if (dibit & 2) raw_64[bit_idx / 8] |= (1 << (7 - (bit_idx % 8)));
        if (dibit & 1) raw_64[(bit_idx + 1) / 8] |= (1 << (7 - ((bit_idx + 1) % 8)));
    }
    
    fprintf(stderr, "\n[VEDA PHY DUMP] raw=");
    for(int i=0; i<36; i++) fprintf(stderr, "%02X", raw_64[i]);
    fprintf(stderr, "\n");
}
  
  if (vc == 6)
  {
    //this needs to run prior to embedded link control
    if (state->payload_algid == 0x02)
        hytera_enhanced_alg_refresh(state);
        
    dmr_data_burst_handler(opts, state, (uint8_t *)dummy_bits, 0xEB);
    //check the single burst/reverse channel opportunity
    dmr_sbrc (opts, state, power);

    fprintf (stderr, "\n");
    dmr_alg_refresh (opts, state);
    // Проверяем, только если это AES-шифрование и статус ключа еще не известен
    /*
    if (state->payload_algid > 0 && state->dmr_key_validation_status[state->payload_keyid] == KEY_UNKNOWN) // == 0x24 || state->payload_algid == 0x25
    {
        // Если хотя бы один фрейм в суперфрейме был "хорошим"
        if (state->kc_frames_ok[0] > 0)
        {
          float energy = 0.0f;
          int i;
          // Анализируем энергию в аудио-буфере (160 сэмплов типа float)
          for (i = 0; i < 160; i++) {
            energy += state->audio_out_temp_buf[i] * state->audio_out_temp_buf[i];
          }

          // Пороговое значение. Тишина будет иметь энергию ~0. Голос - значительно больше.
          // Это значение, возможно, придется немного подстроить, но 100.0f - это безопасное начало.
          float energy_threshold = 1000000.0f;

          if (energy > energy_threshold)
          {
            fprintf(stderr, "\n[+] AES KEY VALIDATION: SUCCESS for KID %d (Audio energy detected: %.2f)\n",
                  state->payload_keyid, energy);
            state->dmr_key_validation_status[state->payload_keyid] = KEY_VALIDATED;
          }
          else
          {
              fprintf(stderr, "\n[-] AES KEY VALIDATION: FAILED for KID %d (Silence detected, energy: %.2f)\n",
                    state->payload_keyid, energy);
                state->dmr_key_validation_status[state->payload_keyid] = KEY_FAILED;
          }          
        }
        else
        {
            // Если все фреймы были "плохими", ключ yt проверяем.
            fprintf(stderr, "\n[-] AES KEY VALIDATION: FAILED for KID %d (OK Frames: %d/%d)\n", 
                    state->payload_keyid, state->kc_frames_ok[0], state->kc_frames_total[0]);
            state->dmr_key_validation_status[state->payload_keyid] = KEY_UNKNOWN;
        }
    }
   */     
  }
  

  //collect the mi fragment
  if (opts->dmr_le != 2) //if not Hytera Enhanced
    dmr_late_entry_mi_fragment (opts, state, vc, m1, m2, m3);

if (opts->isVEDA && !veda_voice_done)
{
    veda_debug_voice_wait(opts, state, 0,
                          state->indx_SF,
                          state->total_sf[0]);
}
  //errors in ms/mono since we skip the other slot
  // cach_err = dmr_cach (opts, state, cachdata);
  /* Реальный CACH decode вместо debug-only gate */
  if (opts->isVEDA) 
  {
    // uint8_t ms_cach_bits[25];
    // dmr_ms_unpack_cach_bits_from_dibits(cachdata, ms_cach_bits);
    // (void)dmr_cach(opts, state, ms_cach_bits);
  }
  //update voice sync time for trunking purposes (particularly Con+)
  state->last_vc_sync_time = time(NULL);

  vc++;

  //reset emb components
  cc = 25;
  power = 9; //power and pre-emption indicator
  lcss = 9;

  //this is necessary because we need to skip and collect dibits, not just skip them
  if (vc > 6)  goto END;

  skipDibit (opts, state, 144); //skip to next tdma channel

  //since we are in a loop, run ncursesPrinter here
  if (opts->use_ncurses_terminal == 1)
  {
    ncursesPrinter(opts, state);
  }

    ipp_last_sample_num();//IPP
  //slot 1
  watchdog_event_history(opts, state, 0);
  watchdog_event_current(opts, state, 0);

 } // end loop

      if(opts->run_scout) {
        //if (!state->is_simulation_active) {
         // avr_scout_on_superframe(opts, state);
        //}      
      }
  
 END:
 //get first half payload dibits and store them in the payload for the next repitition
 skipDibit (opts, state, 144); //should we have two of these?

 //CACH + First Half Payload = 12 + 54
 for (i = 0; i < 66; i++) //66
 {
   dibit = getDibit(opts, state);
   if (opts->inverted_dmr == 1)
   {
     dibit = (dibit ^ 2) & 3;
   }
   state->dmr_stereo_payload[i] = dibit;

 }

 state->dmr_stereo = 0;
 state->dmr_ms_mode = 0;
 state->directmode = 0; //flag off

 if (timestr != NULL)
 {
  free (timestr);
  timestr = NULL;
 }
 ipp_last_sample_num();//IPP
 
 //reset static ks counter
 state->static_ks_counter[0] = 0;

}

//collect buffered 1st half and get 2nd half voice payload and then jump to full MS Voice decoding.
void dmrMSBootstrap (dsd_opts * opts, dsd_state * state)
{
  // сброс аудио-счётчиков MS
  state->kc_frames_total[0] = 0;
  state->kc_frames_ok[0]    = 0;
  state->kc_uncorrectable[0]= 0;

  // сброс признаков ключа на стороне KV
  // dmr_kv_reset_all(state);

  char * timestr = getTimeC();

  //reset static ks counter
  state->static_ks_counter[0] = 0;

  int i, dibit;
  int *dibit_p;

  char ambe_fr[4][24];
  char ambe_fr2[4][24];
  char ambe_fr3[4][24];
  char ambe_fr4[4][24];

  memset (ambe_fr, 0, sizeof(ambe_fr));
  memset (ambe_fr2, 0, sizeof(ambe_fr2));
  memset (ambe_fr3, 0, sizeof(ambe_fr3));

  //memcpy of ambe_fr for late entry
  uint8_t m1[4][24];
  uint8_t m2[4][24];
  uint8_t m3[4][24];

  const int *w, *x, *y, *z;

  //cach
  char cachdata[25];
  UNUSED(cachdata);

  ipp_last_sample_num();//IPP

  //DMH_KV
  if (opts->kv_csv_path[0] && !getG_enum_active())
  {
     avr_kv_batch_begin(opts, state);         // старт батча кандидатов по ALGID/KID
     // старт таймера для этого ключа (если ещё не стартовал)
     const int slot = state->currentslot & 1;
     const int kid  = (int)state->payload_keyid & 0xFF;
     // if (state->kv_key_t0_ms[slot][kid] == 0)
     state->kv_key_t0_ms[slot][kid] = dsd_now_ms();
  }
  // Вызываем дешифратор, если нужно
  if ((state->payload_algid == 0x24  || state->payload_algid == 0x23 || state->payload_algid == 0x25) && state->aes_key_loaded[state->currentslot & 1] == 1)
  {
    // kv_decrypt_ambe_frames(state, ambe_fr, ambe_fr2, ambe_fr3);
  }

  state->dmrburstL = 16;
  state->currentslot = 0; //force to slot 0

  dibit_p = state->dmr_payload_p - 90;

  //CACH + First Half Payload + Sync = 12 + 54 + 24
  for (i = 0; i < 90; i++) //90
  {
    state->dmr_stereo_payload[i] = *dibit_p;
    dibit_p++;
  }

  for(i = 0; i < 12; i++)
  {
    dibit = state->dmr_stereo_payload[i];
    if(opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2) & 3;
    }
    cachdata[i] = dibit;
  }
  //Setup for first AMBE Frame

  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;

  //First AMBE Frame, Full 36
  for(i = 0; i < 36; i++)
  {
    dibit = state->dmr_stereo_payload[i+12];
    if(opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2) & 3;
    }
    state->dmr_stereo_payload[i+12] = dibit;
    ambe_fr[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  //Setup for Second AMBE Frame

  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;

  //Second AMBE Frame, First Half 18 dibits just before Sync or EmbeddedSignalling
  for(i = 0; i < 18; i++)
  {
    dibit = state->dmr_stereo_payload[i+48];
    if(opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2) & 3;
    }
    ambe_fr2[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr2[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  //Continue Second AMBE Frame, 18 after Sync or EmbeddedSignalling
  for(i = 0; i < 18; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2) & 3;
    }
    state->dmr_stereo_payload[i+90] = dibit;
    ambe_fr2[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr2[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  //Setup for Third AMBE Frame

  //Interleave Schedule
  w = rW;
  x = rX;
  y = rY;
  z = rZ;

  //Third AMBE Frame, Full 36
  for(i = 0; i < 36; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1)
    {
      dibit = (dibit ^ 2) & 3;
    }
    state->dmr_stereo_payload[i+108] = dibit;
    ambe_fr3[*w][*x] = (1 & (dibit >> 1)); // bit 1
    ambe_fr3[*y][*z] = (1 & dibit);        // bit 0

    w++;
    x++;
    y++;
    z++;

  }

  //=============== суперкад
/*  
if (opts->run_scout)  
{
  uint8_t enc27[27];
  if (scout_pack_27bytes_from_frames(ambe_fr, ambe_fr2, ambe_fr3, enc27) == 0) {
    // IV берем из текущего стейта (он уже выставлен для этого VC*)
    avr_scout_on_vc(state, enc27, state->aes_iv);
  }
}
*/
  //'DSP' output to file
  if (opts->use_dsp_output == 1)
  {
    FILE * pFile; //file pointer
    pFile = fopen (opts->dsp_out_file, "a");
    fprintf (pFile, "\n%d 10 ", state->currentslot+1); //0x10 for "voice burst", force to slot 1
    for (i = 6; i < 72; i++) //33 bytes, no CACH
    {
      int dsp_byte = (state->dmr_stereo_payload[i*2] << 2) | state->dmr_stereo_payload[i*2 + 1];
      fprintf (pFile, "%X", dsp_byte);
    }
    fclose (pFile);
  }

  fprintf (stderr, "%s ", timestr);
  //IPP
  ippl_new("sync-ms"); 
  ippl_add("b_type", "ms");
  
  if (opts->inverted_dmr == 0)
  {
    fprintf (stderr,"Sync: +DMR MS/DM MODE/MONO ");
    ippl_add("DMR","+DMR");
  }
  else fprintf (stderr,"Sync: -DMR MS/DM MODE/MONO ");
  if (state->dmr_color_code != 16){
    fprintf (stderr, "| Color Code=%02d ", state->dmr_color_code);
    ippl_addi("tcc", state->dmr_color_code);//IPP
  } else {
     fprintf (stderr, "| Color Code=XX ");
     ippl_add("tcc", "XX"); //IPP
  }   
  fprintf (stderr, "| VC* ");
  fprintf (stderr, "\n");
  //IPP
  ippl_add("vc", "1");
  ippl_add("vp", "VLC");//DMH
  ippl_add("slot", "1");

  //alg reset
  //dmr_alg_reset (opts, state);

  memcpy (ambe_fr4, ambe_fr2, sizeof(ambe_fr2));

  //copy ambe_fr frames first, running process mbe will correct them,
  //but this also leads to issues extracting good le mi values when
  //we go to do correction on them there too
  memcpy (m1, ambe_fr, sizeof(m1));
  memcpy (m2, ambe_fr2, sizeof(m2));
  memcpy (m3, ambe_fr3, sizeof(m3));

  if (state->tyt_bp == 1)
  {
    tyt16_ambe2_codeword_keystream(state, ambe_fr, 0);
    tyt16_ambe2_codeword_keystream(state, ambe_fr2, 1);
    tyt16_ambe2_codeword_keystream(state, ambe_fr3, 0);
  }


  if (state->csi_ee == 1)
  {
    csi72_ambe2_codeword_keystream(state, ambe_fr);
    csi72_ambe2_codeword_keystream(state, ambe_fr2);
    csi72_ambe2_codeword_keystream(state, ambe_fr3);
  }

  int veda_voice_done = 0;
  if (opts->isVEDA)
  {
    veda_voice_done = veda_try_decrypt_voice_triplet(opts, state, 0,
                                                     ambe_fr, ambe_fr2, ambe_fr3);
  }

  #ifdef PRINT_AMBE72
  ambe2_codeword_print_i(opts, ambe_fr);
  ambe2_codeword_print_i(opts, ambe_fr2);
  ambe2_codeword_print_i(opts, ambe_fr3);
  #endif

  processMbeFrame (opts, state, NULL, ambe_fr, NULL);
    memcpy(state->f_l4[0], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[0], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[0], state->s_lu, sizeof(state->s_lu));
    if (state->payload_algid > 0x00) { // только когда шифруется
        if (state->errs2 >= 0 && state->errs2 <= 3) { 
          state->kc_frames_ok[0]++;
        } else {
          state->kc_uncorrectable[0]++;
        }
    }    

  processMbeFrame (opts, state, NULL, ambe_fr2, NULL);
    memcpy(state->f_l4[1], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[1], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[1], state->s_lu, sizeof(state->s_lu));
    if (state->payload_algid > 0x00) { // только когда шифруется
        if (state->errs2 >= 0 && state->errs2 <= 3) { 
          state->kc_frames_ok[0]++;
        } else {
          state->kc_uncorrectable[0]++;
        }
    }    

  processMbeFrame (opts, state, NULL, ambe_fr3, NULL);
    memcpy(state->f_l4[2], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
    memcpy(state->s_l4[2], state->s_l, sizeof(state->s_l));
    memcpy(state->s_l4u[2], state->s_lu, sizeof(state->s_lu));
    if (state->payload_algid > 0x00) { // только когда шифруется
        if (state->errs2 >= 0 && state->errs2 <= 3) { 
          state->kc_frames_ok[0]++;
        } else {
          state->kc_uncorrectable[0]++;
        }
    }        
  /*  
  //if ((state->payload_algid == 0x24 || state->payload_algid == 0x25) &&
  if (state->payload_algid > 0 && state->dmr_key_validation_status[state->payload_keyid] == KEY_UNKNOWN)
  {
      float energy = 0.0f;
      int i;
      // Анализируем энергию в аудио-буфере (160 сэмплов типа float)
      for (i = 0; i < 160; i++) {
          energy += state->audio_out_temp_buf[i] * state->audio_out_temp_buf[i];
      }

      // Пороговое значение. Тишина будет иметь энергию ~0. Голос - значительно больше.
      // Это значение, возможно, придется немного подстроить, но 100.0f - это безопасное начало.
      float energy_threshold = 1000000.0f;

      if (energy > energy_threshold)
      {
          fprintf(stderr, "\n[+] AES KEY VALIDATION: SUCCESS for KID %d (Audio energy detected: %.2f)\n",
                  state->payload_keyid, energy);
          state->dmr_key_validation_status[state->payload_keyid] = KEY_VALIDATED;
      }
      else
      {
          fprintf(stderr, "\n[-] AES KEY VALIDATION: FAILED for KID %d (Silence detected, energy: %.2f)\n",
                  state->payload_keyid, energy);
          state->dmr_key_validation_status[state->payload_keyid] = KEY_FAILED;
      }
  }
  */
 
  //TODO: Consider copying f_l to f_r for left and right channel saturation on MS mode
  if (opts->floating_point == 0)
  {
    // memcpy (state->s_r4, state->s_l4, sizeof(state->s_l4));
    if(opts->pulse_digi_out_channels == 2)
      playSynthesizedVoiceSS3(opts, state);
  }

  if (opts->floating_point == 1)
  {
    // memcpy (state->f_r4, state->f_l4, sizeof(state->f_l4));
    if(opts->pulse_digi_out_channels == 2)
      playSynthesizedVoiceFS3(opts, state);
  }

  //collect the mi fragment
  if (opts->dmr_le != 2) //if not Hytera Enhanced
    dmr_late_entry_mi_fragment (opts, state, 1, m1, m2, m3);

if (opts->isVEDA && !veda_voice_done)
{
    veda_debug_voice_wait(opts, state, 0,
                          state->indx_SF,
                          state->total_sf[0]);
}

  //errors due to skipping other slot
  // cach_err = dmr_cach (opts, state, cachdata);
  /* Реально декодируем CACH и запускаем обычный Short LC path в MS/simplex */
  if (opts->isVEDA)  
  {
    // uint8_t ms_cach_bits[25];
    // dmr_ms_unpack_cach_bits_from_dibits(cachdata, ms_cach_bits);
    // (void)dmr_cach(opts, state, ms_cach_bits);
  }  
  if (timestr != NULL)
  {
    free (timestr);
    timestr = NULL;
  }

  skipDibit (opts, state, 144); //skip to next TDMA slot
  ipp_last_sample_num();//IPP
  dmrMS (opts, state); //bootstrap into full TDMA frame
  
  if(!opts->kv_batch_enable)  
    avr_scout_flush(opts, state, true);
}

//simplied to a simple data collector, and then passed on to dmr_data_sync for the usual processing
void dmrMSData (dsd_opts * opts, dsd_state * state)
{

  char * timestr = getTimeC();

  int i;
  int dibit;
  int *dibit_p;

  //CACH + First Half Payload + Sync = 12 + 54 + 24
  dibit_p = state->dmr_payload_p - 90;
  for (i = 0; i < 90; i++) //90
  {
    dibit = *dibit_p;
    dibit_p++;
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;
    state->dmr_stereo_payload[i] = dibit;
  }

  for (i = 0; i < 54; i++)
  {
    dibit = getDibit(opts, state);
    if(opts->inverted_dmr == 1) dibit = (dibit ^ 2) & 3;
    state->dmr_stereo_payload[i+90] = dibit;
  }

  fprintf (stderr, "%s ", timestr);
  //IPP
  ippl_new("sync-ms-data"); 
  ippl_add("b_type", "ms");

  if (opts->inverted_dmr == 0)
  {
    fprintf (stderr,"Sync: +DMR MS/DM MODE/MONO ");
    ippl_add("DMR","+DMR");
  }
  else fprintf (stderr,"Sync: -DMR MS/DM MODE/MONO ");
  if (state->dmr_color_code != 16)
    fprintf (stderr, "| Color Code=%02d ", state->dmr_color_code);
  else fprintf (stderr, "| Color Code=XX ");

  if (state->dmr_color_code != 16){
    fprintf (stderr, "| Color Code=%02d ", state->dmr_color_code);\
    ippl_addi("tcc", state->dmr_color_code);
  } else {
     fprintf (stderr, "| Color Code=XX ");
     ippl_add("tcc", "XX");
  }
  ippl_add("slot", "1"); //DMH
  sprintf(state->slot1light, "%s", "");
  sprintf(state->slot2light, "%s", "");

  //process data
  state->dmr_stereo = 1;
  state->dmr_ms_mode = 1;

  dmr_data_sync (opts, state);

  state->dmr_stereo = 0;
  state->dmr_ms_mode = 0;
  state->directmode = 0; //flag off

  //should just be loaded in the dmr_payload_buffer instead now
  //but we want to run getDibit so the buffer has actual good values in it
  for (i = 0; i < 144; i++) //66
  {
    dibit = getDibit(opts, state);
    state->dmr_stereo_payload[i] = 1; //set to one so first frame will fail intentionally instead of zero fill
  }
  //CACH + First Half Payload = 12 + 54
  for (i = 0; i < 66; i++) //66
  {
    dibit = getDibit(opts, state);
    state->dmr_stereo_payload[i+66] = 1; ////set to one so first frame will fail intentionally instead of zero fill
  }

  if (timestr != NULL)
  {
    free (timestr);
    timestr = NULL;
  }

  if(!opts->kv_batch_enable)
    avr_scout_flush(opts, state, true);

}
