/*
 * TI PCM5102A audio hal
 */

#ifndef _PCM5102A_H_
#define _PCM5102A_H_

#include "audio_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize PCM5102A codec chip
 */
esp_err_t pcm5102a_init(audio_hal_codec_config_t *codec_cfg);

/**
 * Deinitialize PCM5102A codec chip
 */
esp_err_t pcm5102a_deinit(void);

/**
 * Set volume - NOT AVAILABLE
 */
esp_err_t pcm5102a_set_volume(int vol);

/**
 * Get volume - NOT AVAILABLE
 */
esp_err_t pcm5102a_get_volume(int *value);

/**
 * Set PCM5102A mute or not
 */
esp_err_t pcm5102a_set_mute(bool enable);

/**
 * Get PCM5102A mute status - NOT IMPLEMENTED
 */
esp_err_t pcm5102a_get_mute(bool *enabled);

#ifdef __cplusplus
}
#endif

#endif
