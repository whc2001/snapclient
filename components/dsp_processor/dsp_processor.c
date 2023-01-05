

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include "driver/i2s.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "esp_log.h"

#include "board_pins_config.h"
#include "driver/dac.h"
#include "driver/i2s.h"
#include "dsp_processor.h"
#include "hal/i2s_hal.h"

#ifdef CONFIG_USE_BIQUAD_ASM
#define BIQUAD dsps_biquad_f32_ae32
#else
#define BIQUAD dsps_biquad_f32
#endif

static const char *TAG = "dspProc";

static uint32_t currentSamplerate = 0;
static uint32_t currentChunkInFrames = 0;

static ptype_t bq[12];

static double dynamic_vol = 1.0;

#define DSP_PROCESSOR_LEN 16

int dsp_processor(char *audio, size_t chunk_size, dspFlows_t dspFlow) {
  int16_t len = chunk_size / 4;
  int16_t valint;
  uint16_t i;
  volatile uint32_t *audio_tmp =
      (uint32_t *)audio;  // volatile needed to ensure 32 bit access
  float *sbuffer0 = NULL;
  float *sbufout0 = NULL;

  // only process data if it is valid
  if (audio_tmp) {
    sbuffer0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbuffer0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbuffer0");

      return -1;
    }

    sbufout0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbufout0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbufout0");

      free(sbuffer0);

      return -1;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);

          // channel 0
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / 32768;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[8].coeffs, bq[8].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, DSP_PROCESSOR_LEN, bq[9].coeffs, bq[9].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbuffer0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // channel 1
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          32768;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[10].coeffs,
                 bq[10].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, DSP_PROCESSOR_LEN, bq[11].coeffs,
                 bq[11].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbuffer0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspfStereo: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);

          // set volume
          if (dynamic_vol != 1.0) {
            for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
              tmp[i] =
                  ((uint32_t)(dynamic_vol *
                              ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))))
                   << 16) +
                  (uint32_t)(dynamic_vol *
                             ((float)((int16_t)(tmp[i] & 0xFFFF))));
            }
          }
        }

        break;
      }

      case dspfBassBoost: {  // CH0 low shelf 6dB @ 400Hz
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);

          // channel 0
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / 32768;
          }
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[6].coeffs, bq[6].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbufout0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // channel 1
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          32768;
          }
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[7].coeffs, bq[7].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbufout0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspfBiamp: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);

          // Process audio ch0 LOW PASS FILTER
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / 32768;
          }
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[0].coeffs, bq[0].w);
          BIQUAD(sbufout0, sbuffer0, DSP_PROCESSOR_LEN, bq[1].coeffs, bq[1].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbuffer0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // Process audio ch1 HIGH PASS FILTER
          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          32768;
          }
          BIQUAD(sbuffer0, sbufout0, DSP_PROCESSOR_LEN, bq[2].coeffs, bq[2].w);
          BIQUAD(sbufout0, sbuffer0, DSP_PROCESSOR_LEN, bq[3].coeffs, bq[3].w);

          for (i = 0; i < DSP_PROCESSOR_LEN; i++) {
            valint = (int16_t)(sbuffer0[i] * 32768);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        /*
           BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
           BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

           // Process audio L HIGH PASS FILTER
           BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
           BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

           // Process audio R HIGH PASS FILTER
           BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
           BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

           int16_t valint[5];
           for (uint16_t i = 0; i < len; i++) {
             valint[0] =
                 (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] *
           32768); valint[1] = (muteCH[1] == 1) ? (int16_t)0 :
           (int16_t)(sbufout1[i] * 32768); valint[2] = (muteCH[2] == 1) ?
           (int16_t)0 : (int16_t)(sbufout2[i] * 32768); dsp_audio[i * 4 + 0] =
           (valint[2] & 0xff); dsp_audio[i * 4 + 1] = ((valint[2] & 0xff00) >>
           8); dsp_audio[i * 4 + 2] = 0; dsp_audio[i * 4 + 3] = 0;

             dsp_audio1[i * 4 + 0] = (valint[0] & 0xff);
             dsp_audio1[i * 4 + 1] = ((valint[0] & 0xff00) >> 8);
             dsp_audio1[i * 4 + 2] = (valint[1] & 0xff);
             dsp_audio1[i * 4 + 3] = ((valint[1] & 0xff00) >> 8);
           }

           // TODO: this copy could be avoided if dsp_audio buffers are
           // allocated dynamically and pointers are exchanged after
           // audio was freed
           memcpy(audio, dsp_audio, chunk_size);

           ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
     */
        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        /*
          BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
          BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

          // Process audio L HIGH PASS FILTER
          BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
          BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

          // Process audio R HIGH PASS FILTER
          BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
          BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

          uint16_t scale = 16384;  // 32768
          int16_t valint[5];
          for (uint16_t i = 0; i < len; i++) {
            valint[0] =
                (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] * scale);
            valint[1] =
                (muteCH[1] == 1) ? (int16_t)0 : (int16_t)(sbufout1[i] * scale);
            valint[2] =
                (muteCH[2] == 1) ? (int16_t)0 : (int16_t)(sbufout2[i] * scale);
            valint[3] = valint[0] + valint[2];
            valint[4] = -valint[2];
            valint[5] = -valint[1] - valint[2];
            dsp_audio[i * 4 + 0] = (valint[3] & 0xff);
            dsp_audio[i * 4 + 1] = ((valint[3] & 0xff00) >> 8);
            dsp_audio[i * 4 + 2] = (valint[2] & 0xff);
            dsp_audio[i * 4 + 3] = ((valint[2] & 0xff00) >> 8);

            dsp_audio1[i * 4 + 0] = (valint[4] & 0xff);
            dsp_audio1[i * 4 + 1] = ((valint[4] & 0xff00) >> 8);
            dsp_audio1[i * 4 + 2] = (valint[5] & 0xff);
            dsp_audio1[i * 4 + 3] = ((valint[5] & 0xff00) >> 8);
          }

          // TODO: this copy could be avoided if dsp_audio buffers are
          // allocated dynamically and pointers are exchanged after
          // audio was freed
          memcpy(audio, dsp_audio, chunk_size);

          ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
          */
        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
      } break;

      default: { } break; }

    free(sbuffer0);
    sbuffer0 = NULL;

    free(sbufout0);
    sbufout0 = NULL;
  }

  return 0;
}

// ESP32 DSP processor
//======================================================
// Each time a buffer of audio is passed to the DSP - samples are
// processed according to a dynamic list of audio processing nodes.

// Each audio processor node consist of a data struct holding the
// required weights and states for processing an automomous processing
// function. The high level parameters is maintained in the structure
// as well

// Release - Prove off concept
// ----------------------------------------
// Fixed 2x2 biquad flow Xover for biAmp systems
// Interface for cross over frequency and level
void dsp_setup_flow(double freq, uint32_t samplerate, uint32_t chunkInFrames) {
  float f = freq / samplerate / 2.0;

  if (((currentSamplerate == samplerate) &&
       (currentChunkInFrames == chunkInFrames)) ||
      (samplerate == 0) || (chunkInFrames == 0)) {
    return;
  }

  currentSamplerate = samplerate;
  currentChunkInFrames = chunkInFrames;

  bq[0] = (ptype_t){LPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[1] = (ptype_t){LPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[2] = (ptype_t){HPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[3] = (ptype_t){HPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[4] = (ptype_t){HPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[5] = (ptype_t){HPF, f, 0, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[6] = (ptype_t){LOWSHELF, f, 6, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};
  bq[7] = (ptype_t){LOWSHELF, f, 6, 0.707, NULL, NULL, {0, 0, 0, 0, 0}, {0, 0}};

  // TODO: make this (frequency and gain) dynamically adjustable
  // test simple EQ control of low and high frequencies (bass, treble)
  float bass_fc = 300.0 / samplerate;
  float bass_gain = 6.0;
  float treble_fc = 4000.0 / samplerate;
  float treble_gain = 6.0;
  // filters for CH 0
  bq[8] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                    NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
  bq[9] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                    NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};
  // filters for CH 1
  bq[10] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                     NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
  bq[11] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                     NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};

  for (int n = 0; n < sizeof(bq) / sizeof(bq[0]); n++) {
    switch (bq[n].filtertype) {
      case HIGHSHELF:
        dsps_biquad_gen_highShelf_f32(bq[n].coeffs, bq[n].freq, bq[n].gain,
                                      bq[n].q);
        break;

      case LOWSHELF:
        dsps_biquad_gen_lowShelf_f32(bq[n].coeffs, bq[n].freq, bq[n].gain,
                                     bq[n].q);
        break;

      case LPF:
        dsps_biquad_gen_lpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
        break;

      case HPF:
        dsps_biquad_gen_hpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
        break;

      default:
        break;
    }
    //    for (uint8_t i = 0; i <= 4; i++) {
    //      printf("%.6f ", bq[n].coeffs[i]);
    //    }
    //    printf("\n");
  }
}

void dsp_set_xoverfreq(uint8_t freqh, uint8_t freql, uint32_t samplerate) {
  float freq = freqh * 256 + freql;
  //  printf("%f\n", freq);
  float f = freq / samplerate / 2.;
  for (int8_t n = 0; n <= 5; n++) {
    bq[n].freq = f;
    switch (bq[n].filtertype) {
      case LPF:
        //        for (uint8_t i = 0; i <= 4; i++) {
        //          printf("%.6f ", bq[n].coeffs[i]);
        //        }
        //        printf("\n");
        dsps_biquad_gen_lpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
        //        for (uint8_t i = 0; i <= 4; i++) {
        //          printf("%.6f ", bq[n].coeffs[i]);
        //        }
        //        printf("%f \n", bq[n].freq);
        break;
      case HPF:
        dsps_biquad_gen_hpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
        break;
      default:
        break;
    }
  }
}

void dsp_set_vol(double volume) {
  if (volume >= 0 && volume <= 1.0) {
    ESP_LOGI(TAG, "Set volume to %f", volume);
    dynamic_vol = volume;
  }
}
#endif
