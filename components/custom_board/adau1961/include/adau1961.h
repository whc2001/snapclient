/*
 * ANALOG adau1961 audio hal
 */

#ifndef _ADAU1961_H_
#define _ADAU1961_H_

#include "audio_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize adau1961 codec chip
 */
esp_err_t adau1961_init(audio_hal_codec_config_t *codec_cfg);

/**
 * Deinitialize adau1961 codec chip
 */
esp_err_t adau1961_deinit(void);

/**
 * Set volume - NOT AVAILABLE
 */
esp_err_t adau1961_set_volume(int vol);

/**
 * Get volume - NOT AVAILABLE
 */
esp_err_t adau1961_get_volume(int *vol);

/**
 * Set adau1961 mute or not
 */
esp_err_t adau1961_set_mute(bool enable);

/**
 * Get adau1961 mute status - NOT IMPLEMENTED
 */
esp_err_t adau1961_get_mute(bool *enabled);

#ifdef __cplusplus
}
#endif

#endif  // _ADAU1961_H_
