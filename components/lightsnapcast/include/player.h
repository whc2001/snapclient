#ifndef __PLAYER_H__
#define __PLAYER_H__

#include "freertos/FreeRTOS.h"
#include "esp_types.h"
#include "sdkconfig.h"
#include "snapcast.h"
#include "i2s.h"

#define I2S_PORT I2S_NUM_0

extern volatile int64_t clientDacLatency;

#define LATENCY_MEDIAN_FILTER_LEN 99

#define SHORT_BUFFER_LEN 29

typedef struct pcm_chunk_fragment pcm_chunk_fragment_t;
struct pcm_chunk_fragment {
  size_t size;
  char *payload;
  pcm_chunk_fragment_t *nextFragment;
};

typedef struct pcm_chunk_message {
  tv_t timestamp;
  pcm_chunk_fragment_t *fragment;
} pcm_chunk_message_t;

typedef enum codec_type_e { NONE, PCM, FLAC, OGG, OPUS } codec_type_t;

typedef struct snapcastSetting_s {
	uint32_t buffer_ms;
	uint32_t chunkDuration_ms;

	codec_type_t codec;
	int32_t sampleRate;
	uint8_t  channels;
	i2s_bits_per_sample_t  bits;

	bool muted;
	uint32_t volume;
} snapcastSetting_t;

QueueHandle_t init_player(void);
int deinit_player(void);

int8_t insert_pcm_chunk(wire_chunk_message_t *decodedWireChunk);
int8_t free_pcm_chunk(pcm_chunk_message_t *pcmChunk);

int8_t player_latency_insert(int64_t newValue);
int8_t player_notify_buffer_ms(uint32_t ms);
int8_t player_send_snapcast_setting(snapcastSetting_t setting);

int8_t reset_latency_buffer(void);
int8_t latency_buffer_full(void);
int8_t get_diff_to_server(int64_t *tDiff);
int8_t server_now(int64_t *sNow);

#endif  // __PLAYER_H__
