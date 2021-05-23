#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "esp_types.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mp3_decoder.h"
#include "flac_decoder.h"

#include "auto_flac_dec.h"

#include "esp_peripherals.h"
#include "periph_spiffs.h"
#include "board.h"
//#include "es8388.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "mdns.h"
#include "esp_sntp.h"

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "snapcast.h"
#include "MedianFilter.h"

#include <math.h>

#include <sys/time.h>

#include "opus.h"

#define CONFIG_USE_WIFI_PROVISIONING	0
#define COLLECT_RUNTIME_STATS	0

// @ 48kHz, 2ch, 16bit audio data and 24ms wirechunks (hardcoded for now) we expect 0.024 * 2 * 16/8 * 48000 = 4608 Bytes
#define WIRE_CHUNK_DURATION_MS		20UL//24UL		// stream read chunk size [ms]
#define SAMPLE_RATE					48000UL
#define CHANNELS					2UL
#define BITS_PER_SAMPLE			   16UL

const size_t chunkInBytes = (WIRE_CHUNK_DURATION_MS * SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8)) / 1000;

const char *VERSION_STRING = "0.1.0";

/**
 * @brief Pre define APLL parameters, save compute time
 *        | bits_per_sample | rate | sdm0 | sdm1 | sdm2 | odir
 *
 *        apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2)
 *        I2S bit clock is (apll_freq / 16)
 */
static const int apll_predefine[][6] = {
    {16, 11025, 38,  80,  5, 31},
    {16, 16000, 147, 107, 5, 21},
    {16, 22050, 130, 152, 5, 15},
    {16, 32000, 129, 212, 5, 10},
    {16, 44100, 15,  8,   5, 6},
    {16, 48000, 136, 212, 5, 6},
    {16, 96000, 143, 212, 5, 2},
    {0,  0,     0,   0,   0, 0}
};

static const int apll_predefine_48k_corr[][6] = {
	{16, 48048, 27, 215, 5, 6},		// ~ 48kHz * 1.001
	{16, 47952, 20, 210, 5, 6},		// ~ 48kHz * 0.999
	{16, 48005, 213, 212, 5, 6},	// ~ 48kHz * 1.0001
	{16, 47995, 84, 212, 5, 6},		// ~ 48kHz * 0.9999
};

i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();

xQueueHandle i2s_event_queue;

OpusDecoder *opusDecoder = NULL;

audio_pipeline_handle_t flacDecodePipeline;
audio_element_handle_t raw_stream_writer_to_decoder, decoder;

ringbuf_handle_t i2sRingBufferHandle = NULL;

uint64_t wirechnkCnt = 0;
uint64_t pcmchnkCnt = 0;

TaskHandle_t syncTaskHandle = NULL;
TaskHandle_t i2STaskHandle = NULL;

#define CONFIG_USE_SNTP		0

#define DAC_OUT_BUFFER_TIME_US	0

int64_t i2sDmaLAtency		=	0;

static const char *TAG = "SC";

static int sntp_synced = 0;

char *codecString = NULL;

int i2sDmaBufCnt = 0;

// configMAX_PRIORITIES - 1

// TODO: what are the best values here?
#define SYNC_TASK_PRIORITY		7//configMAX_PRIORITIES - 2
#define SYNC_TASK_CORE_ID  		tskNO_AFFINITY//1//tskNO_AFFINITY

#define TIMESTAMP_TASK_PRIORITY	6
#define TIMESTAMP_TASK_CORE_ID 	tskNO_AFFINITY//1//tskNO_AFFINITY

#define HTTP_TASK_PRIORITY		6
#define HTTP_TASK_CORE_ID  		tskNO_AFFINITY//0//tskNO_AFFINITY

#define I2S_TASK_PRIORITY  		8//6//configMAX_PRIORITIES - 1
#define I2S_TASK_CORE_ID  		tskNO_AFFINITY//1//tskNO_AFFINITY

#define FLAC_DECODER_PRIORITY	6
#define FLAC_DECODER_CORE_ID	tskNO_AFFINITY//0//tskNO_AFFINITY

QueueHandle_t timestampQueueHandle;
#define TIMESTAMP_QUEUE_LENGTH 	1000		//!< needs to be at least ~500 because if silence is received, the espressif's flac decoder won't generate data on its output for a long time
static StaticQueue_t timestampQueue;
uint8_t timestampQueueStorageArea[ TIMESTAMP_QUEUE_LENGTH * sizeof(tv_t) ];

QueueHandle_t pcmChunkQueueHandle;
#define PCM_CHNK_QUEUE_LENGTH 500	// TODO: one chunk is hardcoded to 24ms, change it to be dynamically adjustable. 1s buffer ~ 42
static StaticQueue_t pcmChunkQueue;
uint8_t pcmChunkQueueStorageArea[ PCM_CHNK_QUEUE_LENGTH * sizeof(wire_chunk_message_t *) ];


typedef struct snapcast_sync_task_cfg_s {
	audio_element_handle_t *p_raw_stream_writer;
	int64_t outputBufferDacTime_us;
	int64_t buffer_us;
} snapcast_sync_task_cfg_t;

typedef struct http_task_cfg_s {
	audio_element_handle_t *p_raw_stream_writer_to_decoder;
	audio_element_handle_t *p_raw_stream_writer_to_i2s;
} http_task_cfg_t;

SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

SemaphoreHandle_t timer0_syncSampleSemaphoreHandle = NULL;

SemaphoreHandle_t latencyBufSemaphoreHandle = NULL;

#define MEDIAN_FILTER_LONG_BUF_LEN 		299
#define MEDIAN_FILTER_MINI_BUF_LEN 		199
#define MEDIAN_FILTER_SHORT_BUF_LEN 	19

uint8_t latencyBufCnt = 0;
static int8_t latencyBuffFull = 0;

static sMedianFilter_t latencyMedianFilterLong;
static sMedianNode_t latencyMedianLongBuffer[MEDIAN_FILTER_LONG_BUF_LEN];

static sMedianFilter_t latencyMedianFilterMini;
static sMedianNode_t latencyMedianMiniBuffer[MEDIAN_FILTER_MINI_BUF_LEN];

static sMedianFilter_t latencyMedianFilterShort;
static sMedianNode_t latencyMedianShortBuffer[MEDIAN_FILTER_SHORT_BUF_LEN];

static int64_t latencyToServer = 0;

//buffer_.setSize(500);
//    shortBuffer_.setSize(100);
//    miniBuffer_.setSize(20);

#define SHORT_BUFFER_LEN	99
int64_t short_buffer[SHORT_BUFFER_LEN];
int64_t short_buffer_median[SHORT_BUFFER_LEN];

#define MINI_BUFFER_LEN	19
int64_t mini_buffer[MINI_BUFFER_LEN];
int64_t mini_buffer_median[MINI_BUFFER_LEN];

static sMedianFilter_t shortMedianFilter;
static sMedianNode_t shortMedianBuffer[SHORT_BUFFER_LEN];

static sMedianFilter_t miniMedianFilter;
static sMedianNode_t miniMedianBuffer[MINI_BUFFER_LEN];

static int8_t currentDir = 0;	//!< current apll direction, see apll_adjust()

uint32_t buffer_ms = 400;
uint8_t  muteCH[4] = {0};
audio_board_handle_t board_handle;

/* Constants that aren't configurable in menuconfig */
#define HOST "192.168.8.1"
#define PORT 1704
#define BUFF_LEN 10000
unsigned int addr;
uint32_t port = 0;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
//static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

static char buff[BUFF_LEN];
//static audio_element_handle_t snapcast_stream;
static char mac_address[18];

#define MY_SSID		"OpenWrt"
#define MY_WPA2_PSK ""

static EventGroupHandle_t s_wifi_event_group;

const int WIFI_CONNECTED_EVENT 	= BIT0;
const int WIFI_FAIL_EVENT  		= BIT1;

static int s_retry_num = 0;

// Event handler for catching system events
static void event_handler(void* arg, esp_event_base_t event_base, int event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}


static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

#if CONFIG_USE_WIFI_PROVISIONING == 1
/**
 *
 */
static void wifi_init_sta(void) {
    // Start Wi-Fi in station mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Waiting until either the connection is established (WIFI_CONNECTED_EVENT) or connection failed for the maximum
    // number of re-tries (WIFI_FAIL_EVENT). The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(	s_wifi_event_group,
											WIFI_CONNECTED_EVENT | WIFI_FAIL_EVENT,
											pdFALSE,
											pdFALSE,
											portMAX_DELAY );

    // xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened.
    if (bits & WIFI_CONNECTED_EVENT) {
        ESP_LOGI(TAG, "connected to ap");
    }
    else if (bits & WIFI_FAIL_EVENT) {
        ESP_LOGI(TAG, "Failed to connect to AP ...");
    }
    else {
    	ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}
}

/**
 *
 */
void wifi_init_provisioning(void) {
    // Register our event handler for Wi-Fi, IP and Provisioning related events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Initialize Wi-Fi including netif with default config
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Configuration for the provisioning manager
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };

    // Initialize provisioning manager with the
    // configuration parameters set above
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    /* Let's find out if the device is provisioned */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning");

        // Wi-Fi SSID when scheme is wifi_prov_scheme_softap
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        const char *pop = NULL;//"abcd1234";

        /* What is the service key (could be NULL)
         * This translates to :
         *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
         *     - simply ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = "12345678";

        /* An optional endpoint that applications can create if they expect to
         * get some additional custom data during provisioning workflow.
         * The endpoint name can be anything of your choice.
         * This call must be made before starting the provisioning.
         */
        //wifi_prov_mgr_endpoint_create("custom-data");
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

        /* The handler for the optional endpoint created above.
         * This call must be made after starting the provisioning, and only if the endpoint
         * has already been created above.
         */
        //wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

        /* Uncomment the following to wait for the provisioning to finish and then release
         * the resources of the manager. Since in this case de-initialization is triggered
         * by the default event loop handler, we don't need to call the following */
        // wifi_prov_mgr_wait();
        // wifi_prov_mgr_deinit();
    }
    else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);
}

/**
 *
 */
void wifi_init(void) {
	wifi_init_provisioning();
}

#else
/**
 *
 */
void wifi_init(void) {
    // Register our event handler for Wi-Fi, IP and Provisioning related events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Initialize Wi-Fi including netif with default config
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));



//    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//    ESP_ERROR_CHECK(esp_wifi_init(&cfg));


//    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
//    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
			.ssid = MY_SSID,
			.password = MY_WPA2_PSK,
			.bssid_set = false
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Waiting until either the connection is established (WIFI_CONNECTED_EVENT) or connection failed for the maximum
    // number of re-tries (WIFI_FAIL_EVENT). The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(	s_wifi_event_group,
											WIFI_CONNECTED_EVENT | WIFI_FAIL_EVENT,
											pdFALSE,
											pdFALSE,
											portMAX_DELAY );

    // xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened.
    if (bits & WIFI_CONNECTED_EVENT) {
        ESP_LOGI(TAG, "connected to ap");
    } else if (bits & WIFI_FAIL_EVENT) {
        ESP_LOGI(TAG, "Failed to connect to AP ...");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    //ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    //ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    //vEventGroupDelete(s_wifi_event_group);
}
#endif

static const char * if_str[] = {"STA", "AP", "ETH", "MAX"};
static const char * ip_protocol_str[] = {"V4", "V6", "MAX"};

void mdns_print_results(mdns_result_t * results){
    mdns_result_t * r = results;
    mdns_ip_addr_t * a = NULL;
    int i = 1, t;
    while(r){
        printf("%d: Interface: %s, Type: %s\n", i++, if_str[r->tcpip_if], ip_protocol_str[r->ip_protocol]);
        if(r->instance_name){
            printf("  PTR : %s\n", r->instance_name);
        }
        if(r->hostname){
            printf("  SRV : %s.local:%u\n", r->hostname, r->port);
        }
        if(r->txt_count){
            printf("  TXT : [%u] ", r->txt_count);
            for(t=0; t<r->txt_count; t++){
                printf("%s=%s; ", r->txt[t].key, r->txt[t].value);
            }
            printf("\n");
        }
        a = r->addr;
        while(a){
            if(a->addr.type == IPADDR_TYPE_V6){
                printf("  AAAA: " IPV6STR "\n", IPV62STR(a->addr.u_addr.ip6));
            } else {
                printf("  A   : " IPSTR "\n", IP2STR(&(a->addr.u_addr.ip4)));
            }
            a = a->next;
        }
        r = r->next;
    }

}

void find_mdns_service(const char * service_name, const char * proto) {
    ESP_LOGI(TAG, "Query PTR: %s.%s.local", service_name, proto);

    mdns_result_t * r = NULL;
    esp_err_t err = mdns_query_ptr(service_name, proto, 3000, 20,  &r);
    if(err){
        ESP_LOGE(TAG, "Query Failed");
        return;
    }
    if(!r){
        ESP_LOGW(TAG, "No results found!");
        return;
    }

    if(r->instance_name){
            printf("  PTR : %s\n", r->instance_name);
    }
    if(r->hostname){
            printf("  SRV : %s.local:%u\n", r->hostname, r->port);
      port = r->port;
    }
    mdns_query_results_free(r);
}

/**
 *
 */
int8_t reset_latency_buffer(void) {
	// init diff buff median filter
	latencyMedianFilterLong.numNodes = MEDIAN_FILTER_LONG_BUF_LEN;
    latencyMedianFilterLong.medianBuffer = latencyMedianLongBuffer;
	if (MEDIANFILTER_Init(&latencyMedianFilterLong) < 0) {
		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter long. STOP");

		return -2;
	}

	latencyMedianFilterMini.numNodes = MEDIAN_FILTER_MINI_BUF_LEN;
	latencyMedianFilterMini.medianBuffer = latencyMedianMiniBuffer;
	if (MEDIANFILTER_Init(&latencyMedianFilterMini) < 0) {
		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter mini. STOP");

		return -2;
	}

	latencyMedianFilterShort.numNodes = MEDIAN_FILTER_SHORT_BUF_LEN;
    latencyMedianFilterShort.medianBuffer = latencyMedianShortBuffer;
	if (MEDIANFILTER_Init(&latencyMedianFilterShort) < 0) {
		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter short. STOP");

		return -2;
	}

	if (latencyBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "reset_diff_buffer: latencyBufSemaphoreHandle == NULL");

		return -2;
	}

	if (xSemaphoreTake( latencyBufSemaphoreHandle, portMAX_DELAY ) == pdTRUE) {
		latencyBufCnt = 0;
		latencyBuffFull = false;
		latencyToServer = 0;

		xSemaphoreGive( latencyBufSemaphoreHandle );
	}
	else {
		ESP_LOGW(TAG, "reset_diff_buffer: can't take semaphore");

		return -1;
	}

	return 0;
}

/**
 *
 */
int8_t latency_buffer_full(void) {
	int8_t tmp;

	if (latencyBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "latency_buffer_full: latencyBufSemaphoreHandle == NULL");

		return -2;
	}

	if (xSemaphoreTake( latencyBufSemaphoreHandle, 0) == pdFALSE) {
		ESP_LOGW(TAG, "latency_buffer_full: can't take semaphore");

		return -1;
	}

	tmp = latencyBuffFull;

	xSemaphoreGive( latencyBufSemaphoreHandle );

	return tmp;
}

/**
 *
 */
int8_t get_diff_to_server( int64_t *tDiff ) {
	static int64_t lastDiff = 0;

	if (latencyBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "get_diff_to_server: latencyBufSemaphoreHandle == NULL");

		return -2;
	}

	if (xSemaphoreTake( latencyBufSemaphoreHandle, 0 ) == pdFALSE) {
		*tDiff = lastDiff;

		//ESP_LOGW(TAG, "get_diff_to_server: can't take semaphore. Old diff retreived");

		return -1;
	}

	*tDiff = latencyToServer;
	lastDiff = latencyToServer;		// store value, so we can return a value if semaphore couldn't be taken

	xSemaphoreGive( latencyBufSemaphoreHandle );

	return 0;
}

/**
 *
 */
int8_t server_now( int64_t *sNow ) {
	struct timeval now;
	int64_t diff;

	// get current time
	if (gettimeofday(&now, NULL)) {
		ESP_LOGE(TAG, "server_now: Failed to get time of day");

		return -1;
	}

	if (get_diff_to_server(&diff) == -1) {
		ESP_LOGW(TAG, "server_now: can't get current diff to server. Retrieved old one");
	}

	if (diff == 0) {
		//ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

		return -1;
	}

	*sNow = ((int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec) + diff;

//	ESP_LOGI(TAG, "now: %lldus", (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec);
//	ESP_LOGI(TAG, "diff: %lldus", diff);
//	ESP_LOGI(TAG, "serverNow: %lldus", *snow);

	return 0;
}

/*
 * Timer group0 ISR handler
 *
 * Note:
 * We don't call the timer API here because they are not declared with IRAM_ATTR.
 * If we're okay with the timer irq not being serviced while SPI flash cache is disabled,
 * we can allocate this interrupt without the ESP_INTR_FLAG_IRAM flag and use the normal API.
 */
void IRAM_ATTR timer_group0_isr(void *para) {
    timer_spinlock_take(TIMER_GROUP_0);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Retrieve the interrupt status and the counter value
    //   from the timer that reported the interrupt
    uint32_t timer_intr = timer_group_get_intr_status_in_isr(TIMER_GROUP_0);

    // Clear the interrupt
    //   and update the alarm time for the timer with without reload
    if (timer_intr & TIMER_INTR_T1) {
        timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_1);

        // Notify the task in the task's notification value.
        xTaskNotifyFromISR( syncTaskHandle,
                            0,
							eNoAction,
                            &xHigherPriorityTaskWoken );
    }

    timer_spinlock_give(TIMER_GROUP_0);

	if( xHigherPriorityTaskWoken )	{
		portYIELD_FROM_ISR ();
	}
}

static void tg0_timer_deinit(void) {
	timer_deinit(TIMER_GROUP_0, TIMER_1);
}

/*
 *
 */
static void tg0_timer_init(void) {
    // Select and initialize basic parameters of the timer
    timer_config_t config = {
        //.divider = 8,		// 100ns ticks
    	.divider = 80,		// 1µs ticks
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = TIMER_AUTORELOAD_DIS,
    }; // default clock source is APB
    timer_init(TIMER_GROUP_0, TIMER_1, &config);

    // Configure the alarm value and the interrupt on alarm.
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, 0);
    timer_enable_intr(TIMER_GROUP_0, TIMER_1);
    //timer_isr_register(TIMER_GROUP_0, TIMER_1, timer_group0_isr, NULL, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3, NULL);
    timer_isr_register(TIMER_GROUP_0, TIMER_1, timer_group0_isr, NULL, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1, NULL);
}

/**
 *
 */
static void tg0_timer1_start(uint64_t alarm_value) {
	timer_pause(TIMER_GROUP_0, TIMER_1);
	timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, alarm_value);
    timer_set_alarm(TIMER_GROUP_0, TIMER_1, TIMER_ALARM_EN);
    timer_start(TIMER_GROUP_0, TIMER_1);

//    ESP_LOGI(TAG, "started age timer");
}


#if COLLECT_RUNTIME_STATS == 1

#define STATS_TASK_PRIO     3
#define STATS_TICKS         pdMS_TO_TICKS(5000)
#define ARRAY_SIZE_OFFSET   5   //Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE

//static char task_names[15][configMAX_TASK_NAME_LEN];

/**
 * @brief   Function to print the CPU usage of tasks over a given duration.
 *
 * This function will measure and print the CPU usage of tasks over a specified
 * number of ticks (i.e. real time stats). This is implemented by simply calling
 * uxTaskGetSystemState() twice separated by a delay, then calculating the
 * differences of task run times before and after the delay.
 *
 * @note    If any tasks are added or removed during the delay, the stats of
 *          those tasks will not be printed.
 * @note    This function should be called from a high priority task to minimize
 *          inaccuracies with delays.
 * @note    When running in dual core mode, each core will correspond to 50% of
 *          the run time.
 *
 * @param   xTicksToWait    Period of stats measurement
 *
 * @return
 *  - ESP_OK                Success
 *  - ESP_ERR_NO_MEM        Insufficient memory to allocated internal arrays
 *  - ESP_ERR_INVALID_SIZE  Insufficient array size for uxTaskGetSystemState. Trying increasing ARRAY_SIZE_OFFSET
 *  - ESP_ERR_INVALID_STATE Delay duration too short
 */
static esp_err_t print_real_time_stats(TickType_t xTicksToWait)
{
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    uint32_t start_run_time, end_run_time;
    esp_err_t ret;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * portNUM_PROCESSORS);
            printf("| %s | %d | %d%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

/**
 *
 */
static void stats_task(void *arg) {
    //Print real time stats periodically
    while (1) {
        printf("\n\nGetting real time stats over %d ticks\n", STATS_TICKS);
        if (print_real_time_stats(STATS_TICKS) == ESP_OK) {
            printf("Real time stats obtained\n");
        } else {
            printf("Error getting real time stats\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif	// COLLECT_RUNTIME_STATS


// void rtc_clk_apll_enable(bool enable, uint32_t sdm0, uint32_t sdm1, uint32_t sdm2, uint32_t o_div);
// apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2)
// xtal == 40MHz on lyrat v4.3
// I2S bit_clock = rate * (number of channels) * bits_per_sample
void adjust_apll(int8_t direction) {
	int sdm0, sdm1, sdm2, o_div;
	int index = 2;		// 2 for slow adjustment, 0 for fast adjustment

	// only change if necessary
	if (currentDir == direction) {
		return;
	}

	if (direction == 1) {
		// speed up
		sdm0 = apll_predefine_48k_corr[index][2];
		sdm1= apll_predefine_48k_corr[index][3];
		sdm2 = apll_predefine_48k_corr[index][4];
		o_div = apll_predefine_48k_corr[index][5];
	}
	else if (direction == -1) {
		// slow down
		sdm0 = apll_predefine_48k_corr[index + 1][2];
		sdm1= apll_predefine_48k_corr[index + 1][3];
		sdm2 = apll_predefine_48k_corr[index + 1][4];
		o_div = apll_predefine_48k_corr[index + 1][5];
	}
	else {
		// reset to normal playback speed
		sdm0 = apll_predefine[5][2];
		sdm1= apll_predefine[5][3];
		sdm2 = apll_predefine[5][4];
		o_div = apll_predefine[5][5];

		direction = 0;
	}

	rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

	currentDir = direction;
}

/**
 *
 */
static void snapcast_sync_task(void *pvParameters) {
//	snapcast_sync_task_cfg_t *taskCfg = (snapcast_sync_task_cfg_t *)pvParameters;
	wire_chunk_message_t *chnk = NULL;
	int64_t serverNow = 0;
	int64_t age;
	BaseType_t ret;
	int64_t chunkDuration_us = WIRE_CHUNK_DURATION_MS * 1000;
	int64_t sampleDuration_ns = (1000000 / 48); // 16bit, 2ch, 48kHz (in nano seconds)
	char *p_payload = NULL;
	size_t size = 0;
	uint32_t notifiedValue;
	uint64_t timer_val;
	int32_t alarmValSub = 0;
	int initialSync = 0;
	int64_t avg = 0;
	int64_t latencyInitialSync = 0;
	int dir = 0;
	i2s_event_t i2sEvent;
	uint32_t i2sDmaBufferCnt = 0;
	int writtenBytes, bytesAvailable;
	int64_t buffer_ms_local = buffer_ms;

	ESP_LOGI(TAG, "started sync task");

//	tg0_timer_init();		// initialize initial sync timer

	initialSync = 0;

	currentDir = 1;		// force adjust_apll to set correct playback speed
	adjust_apll(0);

	miniMedianFilter.numNodes = MINI_BUFFER_LEN;
	miniMedianFilter.medianBuffer = miniMedianBuffer;
	if (MEDIANFILTER_Init(&miniMedianFilter) ) {
		ESP_LOGE(TAG, "snapcast_sync_task: couldn't init miniMedianFilter. STOP");

		return;
	}

	shortMedianFilter.numNodes = SHORT_BUFFER_LEN;
	shortMedianFilter.medianBuffer = shortMedianBuffer;
	if (MEDIANFILTER_Init(&shortMedianFilter) ) {
		ESP_LOGE(TAG, "snapcast_sync_task: couldn't init shortMedianFilter. STOP");

		return;
	}

	while(1) {
		// get notification value which holds buffer_ms as communicated by snapserver
		xTaskNotifyWait( pdFALSE,    		// Don't clear bits on entry.
						 pdFALSE,        	// Don't clear bits on exit
						 &notifiedValue, 	// Stores the notified value.
						 0
						);

		buffer_ms_local = (int64_t)notifiedValue * 1000LL;

		if (chnk == NULL) {
			ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(2000) );
			if( ret != pdFAIL )	{
//				ESP_LOGW(TAG, "got pcm chunk");
			}
		}
		else {
//			ESP_LOGW(TAG, "already retrieved chunk needs service");
			ret = pdPASS;
		}

		if( ret != pdFAIL )	{
			if (server_now(&serverNow) >= 0) {
				age =   serverNow -
						((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
						(int64_t)buffer_ms_local +
						(int64_t)i2sDmaLAtency +
						(int64_t)DAC_OUT_BUFFER_TIME_US;
			}
			else {
				ESP_LOGW(TAG, "couldn't get server now");

				if (chnk != NULL) {
					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}

				vTaskDelay( pdMS_TO_TICKS(1) );

				continue;
			}

			/*
			// wait for early time syncs to be ready
			int tmp = latency_buffer_full();
			if ( tmp <= 0 ) {
				if (tmp < 0) {
					ESP_LOGW(TAG, "test");

					vTaskDelay(1);

					continue;
				}

				if (age >= 0) {
					if (chnk != NULL) {
						free(chnk->payload);
						free(chnk);
						chnk = NULL;
					}
				}

				ESP_LOGW(TAG, "diff buffer not full");

				vTaskDelay( pdMS_TO_TICKS(10) );

				continue;
			}
			*/

			if (chnk != NULL) {
				p_payload = chnk->payload;
				size = chnk->size;
			}

			if (age < 0) {		// get initial sync using hardware timer
				if (initialSync == 0) {
					if (MEDIANFILTER_Init(&shortMedianFilter) ) {
						ESP_LOGE(TAG, "snapcast_sync_task: couldn't init shortMedianFilter. STOP");

						return;
					}

					/*
					ESP_LOGI(TAG, "age before sync %lld", age);

					// ensure enough time for resync
					if (age > -(int64_t)WIRE_CHUNK_DURATION_MS * 1000 * 2) {
						if (chnk != NULL) {
							free(chnk->payload);
							free(chnk);
							chnk = NULL;
						}

						ESP_LOGI(TAG, "trying to resync");

						vTaskDelay(1);

						continue;
					}
					*/

					adjust_apll(0);	// reset to normal playback speed

					i2s_zero_dma_buffer(i2s_cfg.i2s_port);
					i2s_stop(i2s_cfg.i2s_port);

					size_t written;
					if (i2s_write(I2S_NUM_0, p_payload, (size_t)size, &written, 0) != ESP_OK) {
						ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
					}
					size -= written;
					p_payload += written;

					if ((chnk != NULL) && (size == 0)) {
						free(chnk->payload);
						free(chnk);
						chnk = NULL;
					}

					//tg0_timer1_start((-age * 10) - alarmValSub));	// alarm a little earlier to account for context switch duration from freeRTOS, timer with 100ns ticks
					tg0_timer1_start(-age - alarmValSub);			// alarm a little earlier to account for context switch duration from freeRTOS, timer with 1µs ticks

					// Wait to be notified of a timer interrupt.
					xTaskNotifyWait( pdFALSE,    		// Don't clear bits on entry.
									 pdFALSE,        	// Don't clear bits on exit.
									 &notifiedValue, 	// Stores the notified value.
									 portMAX_DELAY
									);

					i2s_start(i2s_cfg.i2s_port);

					// get timer value so we can get the real age
					timer_get_counter_value(TIMER_GROUP_0, TIMER_1, &timer_val);
					timer_pause(TIMER_GROUP_0, TIMER_1);

					// get actual age after alarm
					//age = ((int64_t)timer_val - (-age) * 10) / 10;	// timer with 100ns ticks
					age = (int64_t)timer_val - (-age);					// timer with 1µs ticks

					if (i2s_write(I2S_NUM_0, p_payload, (size_t)size, &written, portMAX_DELAY) != ESP_OK) {
						ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
					}
					size -= written;
					p_payload += written;

					if ((chnk != NULL) && (size == 0))
					{
						free(chnk->payload);
						free(chnk);
						chnk = NULL;

						ret = xQueueReceive(pcmChunkQueueHandle, &chnk, portMAX_DELAY);
						if( ret != pdFAIL )	{
							p_payload = chnk->payload;
							size = chnk->size;
							if (i2s_write(I2S_NUM_0, p_payload, (size_t)size, &written, portMAX_DELAY) != ESP_OK) {
								ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
							}
							size -= written;
							p_payload += written;

							if ((chnk != NULL) && (size == 0)) {
								free(chnk->payload);
								free(chnk);
								chnk = NULL;
							}
						}
					}

					initialSync = 1;

					ESP_LOGI(TAG, "initial sync %lldus", age);

					continue;
				}
			}
			else if ((age > 0) && (initialSync == 0)) {
				if (chnk != NULL) {
					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}

				int64_t t;
				get_diff_to_server(&t);
				ESP_LOGW(TAG, "RESYNCING HARD 1 %lldus, %lldus", age, t);

				dir = 0;

				initialSync = 0;
				alarmValSub = 0;

				continue;
			}

			if (initialSync == 1) {
				const uint8_t enableControlLoop = 1;
				const int64_t age_expect = -chunkDuration_us * 2;
				const int64_t maxOffset =  100;									//µs, softsync 1
				const int64_t maxOffset_dropSample = 1000;						//µs, softsync 2
				const int64_t hardResyncThreshold = 1500;						//µs, hard sync

				avg = MEDIANFILTER_Insert(&shortMedianFilter, age + (-age_expect));
				if ( MEDIANFILTER_isFull(&shortMedianFilter) == 0 ) {
					avg = age + (-age_expect);
				}
				else {
					// resync hard if we are off too far
					if ((avg < -hardResyncThreshold) || (avg > hardResyncThreshold) || (initialSync == 0)) {
						if (chnk != NULL) {
							free(chnk->payload);
							free(chnk);
							chnk = NULL;
						}

						int64_t t;
						get_diff_to_server(&t);
						ESP_LOGW(TAG, "RESYNCING HARD 2 %lldus, %lldus, %lldus", age, avg, t);

						initialSync = 0;
						alarmValSub = 0;

						i2sDmaBufferCnt = i2sDmaBufCnt * 1;	// ensure dma is empty
						do {
							// wait until DMA queue is empty
							ret = xQueueReceive(i2s_event_queue, &i2sEvent, portMAX_DELAY );
							if( ret != pdFAIL )	{
								if (i2sEvent.type == I2S_EVENT_TX_DONE) {
									ESP_LOGI(TAG, "I2S_EVENT_TX_DONE, %u", i2sDmaBufferCnt);

									i2sDmaBufferCnt--;
								}
							}
						} while(i2sDmaBufferCnt > 0);

						continue;
					}
				}

				int samples = 1;
				int sampleSize = 4;
				int ageDiff = 0;
				size_t written;

				if (enableControlLoop == 1) {
					if (avg < -maxOffset) {		// we are early
						dir = -1;

						/*
						//if ( MEDIANFILTER_isFull(&shortMedianFilter))
						{
							if (avg < -maxOffset_dropSample) {
								//ageDiff = (int)(age_expect - avg);
								ageDiff = -(int)avg;
								samples = ageDiff / (sampleDuration_ns / 1000);
								if (samples > 4) {
									samples = 4;
								}

								ESP_LOGI(TAG, "insert %d samples", samples);

								// TODO: clean insert at periodic positions, so stretching won't be audible.
								char *newBuf = NULL;
								newBuf = (char *)heap_caps_malloc(sizeof(char) * (size + samples * sampleSize), MALLOC_CAP_SPIRAM);
								memcpy(newBuf, p_payload, size);
								free(p_payload);
								p_payload = newBuf;
								memcpy(&p_payload[size], &p_payload[size - 1 - samples * sampleSize], samples * sampleSize);
								size += samples * sampleSize;

								chnk->payload = p_payload;
								chnk->size = size;
							}
						}
						*/
					}
					else if ((avg >= -maxOffset) && (avg <= maxOffset)) {
						dir = 0;
					}
					else if (avg > maxOffset) {		// we are late
						dir = 1;

						/*
						//if ( MEDIANFILTER_isFull(&shortMedianFilter))
						{
							if (avg > maxOffset_dropSample) {
								//ageDiff = (int)(avg - age_expect);
								ageDiff = (int)avg;
								samples = ageDiff / (sampleDuration_ns / 1000);
								if (samples > 4) {
									samples = 4;
								}

								if (size >= samples * sampleSize) {
									// drop samples
									p_payload += samples * sampleSize;
									size -= samples * sampleSize;

									ESP_LOGI(TAG, "drop %d samples", samples);
								}
							}
						}
						*/
					}

					adjust_apll(dir);
				}

				if (i2s_write(I2S_NUM_0, p_payload, (size_t)size, &written, portMAX_DELAY) != ESP_OK) {
					ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
				}
				size -= written;
				p_payload += written;

				if ((chnk != NULL) && (size == 0)) {
					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}
			}

			int64_t t;
			get_diff_to_server(&t);
			ESP_LOGI(TAG, "%d: %lldus, %lldus %lldus", dir, age, avg, t);

			if ((chnk != NULL) && (size == 0)) {
				free(chnk->payload);
				free(chnk);
				chnk = NULL;
			}
		}
		else {
			int64_t t;
			get_diff_to_server(&t);
			ESP_LOGE(TAG, "Couldn't get PCM chunk, recv: messages waiting %d, %d, latency %lldus", uxQueueMessagesWaiting(pcmChunkQueueHandle), uxQueueMessagesWaiting(timestampQueueHandle), t);

			dir = 0;

			initialSync = 0;
			alarmValSub = 0;
		}
	}
}

/**
 *
 */
void time_sync_msg_cb(void *args) {
	static BaseType_t xHigherPriorityTaskWoken;

	//xSemaphoreGive(timeSyncSemaphoreHandle);		// causes kernel panic, which shouldn't happen though? Isn't it called from timer task instead of ISR?
	xSemaphoreGiveFromISR(timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
}


/**
 *
 */
static void http_get_task(void *pvParameters) {
    struct sockaddr_in servaddr;
    char *start;
    int sockfd = -1;
    char base_message_serialized[BASE_MESSAGE_SIZE];
    char *hello_message_serialized = NULL;
    int result, size, id_counter;
    struct timeval now, tv1, tv2, tv3;
    time_message_t time_message;
    struct timeval tmpDiffToServer;
    const int64_t outputBufferDacTime_us = DAC_OUT_BUFFER_TIME_US;	// in ms
//    snapcast_sync_task_cfg_t snapcastTaskCfg;
	struct timeval lastTimeSync = { 0, 0 };
	wire_chunk_message_t wire_chunk_message_last = {{0,0}, 0, NULL};
	esp_timer_handle_t timeSyncMessageTimer;
	const esp_timer_create_args_t tSyncArgs = 	{
													.callback = &time_sync_msg_cb,
													.name = "tSyncMsg"
												};
	int16_t frameSize = 960;		// 960*2: 20ms, 960*1: 10ms
	int16_t *audio = NULL;
	int16_t pcm_size = 120;
	uint16_t channels = CHANNELS;

    // create semaphore for time diff buffer to server
    latencyBufSemaphoreHandle = xSemaphoreCreateMutex();

    // create a timer to send time sync messages every x µs
    esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);
	timeSyncSemaphoreHandle = xSemaphoreCreateMutex();
	xSemaphoreGive(timeSyncSemaphoreHandle);

    id_counter = 0;

    // create snapcast receive buffer
    pcmChunkQueueHandle = xQueueCreateStatic( PCM_CHNK_QUEUE_LENGTH,
											  sizeof(wire_chunk_message_t *),
											  pcmChunkQueueStorageArea,
											  &pcmChunkQueue
											);

    ESP_LOGI(TAG, "Enable mdns") ;
    mdns_init();

    while(1) {
    	latencyBufCnt = 0;

    	// init diff buff median filter
    	latencyMedianFilterLong.numNodes = MEDIAN_FILTER_LONG_BUF_LEN;
        latencyMedianFilterLong.medianBuffer = latencyMedianLongBuffer;
    	if (MEDIANFILTER_Init(&latencyMedianFilterLong) < 0) {
    		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter long. STOP");

    		return;
    	}

    	latencyMedianFilterMini.numNodes = MEDIAN_FILTER_MINI_BUF_LEN;
    	latencyMedianFilterMini.medianBuffer = latencyMedianMiniBuffer;
    	if (MEDIANFILTER_Init(&latencyMedianFilterMini) < 0) {
    		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter mini. STOP");

    		return;
    	}

    	latencyMedianFilterShort.numNodes = MEDIAN_FILTER_SHORT_BUF_LEN;
        latencyMedianFilterShort.medianBuffer = latencyMedianShortBuffer;
    	if (MEDIANFILTER_Init(&latencyMedianFilterShort) < 0) {
    		ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter short. STOP");

    		return;
    	}

        /* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
//        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT,
//                            false, true, portMAX_DELAY);
//        ESP_LOGI(TAG, "Connected to AP");

        // Find snapcast server
        // Connect to first snapcast server found
        mdns_result_t * r = NULL;
        esp_err_t err = 0;
        while ( !r || err ) {
        	ESP_LOGI(TAG, "Lookup snapcast service on network");
           esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20,  &r);
           if(err) {
             ESP_LOGE(TAG, "Query Failed");
           }
           if(!r){
             ESP_LOGW(TAG, "No results found!");
           }
           vTaskDelay(1000/portTICK_PERIOD_MS);
        }
//        ESP_LOGI(TAG,"Found %08x", r->addr->addr.u_addr.ip4.addr);
        ESP_LOGI(TAG,"Found %s", inet_ntoa(r->addr->addr.u_addr.ip4.addr));

        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = r->addr->addr.u_addr.ip4.addr;          // inet_addr("192.168.1.158");
        servaddr.sin_port = htons(r->port);
        mdns_query_results_free(r);

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0) {
            ESP_LOGE(TAG, "%s", strerror(errno));
            close(sockfd);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");

        codec_header_message_t codec_header_message;
        server_settings_message_t server_settings_message;

        result = gettimeofday(&now, NULL);
        if (result) {
            ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
            return;
        }

        bool received_header = false;
        base_message_t base_message = 	{
											SNAPCAST_MESSAGE_HELLO,
											0x0,
											0x0,
											{ now.tv_sec, now.tv_usec },
											{ 0x0, 0x0 },
											0x0,
										};

        hello_message_t hello_message =  {
											mac_address,
											"ESP32-Caster",
											VERSION_STRING,
											"libsnapcast",
											"esp32",
											"xtensa",
											1,
											mac_address,
											2,
										 };

        if (hello_message_serialized == NULL) {
			hello_message_serialized = hello_message_serialize(&hello_message, (size_t*) &(base_message.size));
			if (!hello_message_serialized) {
				ESP_LOGE(TAG, "Failed to serialize hello message\r\b");
				return;
			}
        }

        result = base_message_serialize(
											&base_message,
											base_message_serialized,
											BASE_MESSAGE_SIZE
										);
        if (result) {
            ESP_LOGE(TAG, "Failed to serialize base message\r\n");
            return;
        }

        result = write(sockfd, base_message_serialized, BASE_MESSAGE_SIZE);
        if (result < 0) {
        	ESP_LOGW(TAG, "error writing base msg to socket: %s", strerror(errno));
        	free(hello_message_serialized);
        	hello_message_serialized = NULL;
			continue;
		}

        result = write(sockfd, hello_message_serialized, base_message.size);
        if (result < 0) {
        	ESP_LOGW(TAG, "error writing hello msg to socket: %s", strerror(errno));
        	free(hello_message_serialized);
        	hello_message_serialized = NULL;
			continue;
		}

        free(hello_message_serialized);
        hello_message_serialized = NULL;

        for (;;) {
            size = 0;
            result = 0;
            while (size < BASE_MESSAGE_SIZE) {
            	result = read(sockfd, &(buff[size]), BASE_MESSAGE_SIZE - size);
                if (result < 0) {
                    break;

                }
                size += result;
            }

            if (result < 0) {
            	if (errno != 0 ) {
            		ESP_LOGW(TAG, "%s, %d", strerror(errno), errno);
            	}

            	// TODO: error handling needed for robust socket application

            	break;	// stop for(;;) will try to reconnect then
            }

            if (result > 0) {
				result = gettimeofday(&now, NULL);
				//ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec, now.tv_usec);
				if (result) {
					ESP_LOGW(TAG, "Failed to gettimeofday");
					continue;
				}

				result = base_message_deserialize(&base_message, buff, size);
				if (result) {
					ESP_LOGW(TAG, "Failed to read base message: %d", result);
					continue;
				}

				base_message.received.usec = now.tv_usec;
	//            ESP_LOGI(TAG,"%d %d : %d %d : %d %d",base_message.size, base_message.refersTo,
	//												 base_message.sent.sec,base_message.sent.usec,
	//												 base_message.received.sec,base_message.received.usec);

				start = buff;
				size = 0;

				// TODO: dynamically allocate memory for the next read!!!
				// 		 generate an error for now if we try to read more than BUFF_LEN in next lines
				if (base_message.size > BUFF_LEN) {
					ESP_LOGE(TAG, "base_message.size too big %d", base_message.size);

					return;
				}

				while (size < base_message.size) {
					if (size >= BUFF_LEN) {
						ESP_LOGE(TAG, "Index too high");

						return;
					}

					result = read(sockfd, &(buff[size]), base_message.size - size);
					if (result < 0) {
						ESP_LOGW(TAG, "Failed to read from server: %d", result);

						break;
					}

					size += result;
				}

				if (result < 0) {
					if (errno != 0 ) {
						ESP_LOGI(TAG, "%s, %d", strerror(errno), errno);
					}

					// TODO: error handling needed for robust socket application

					break;	// stop for(;;) will try to reconnect then
				}

				switch (base_message.type) {
					case SNAPCAST_MESSAGE_CODEC_HEADER:
						result = codec_header_message_deserialize(&codec_header_message, start, size);
						if (result) {
							ESP_LOGI(TAG, "Failed to read codec header: %d", result);
							return;
						}

						size = codec_header_message.size;
						start = codec_header_message.payload;

						//ESP_LOGI(TAG, "Received codec header message with size %d", codec_header_message.size);

						if (strcmp(codec_header_message.codec, "flac") == 0) {
							// TODO: maybe restart the whole thing if a new codec header is received while stream session is ongoing

							ESP_LOGI(TAG, "Codec : %s", codec_header_message.codec);

							raw_stream_write(raw_stream_writer_to_decoder, codec_header_message.payload, size);
						}
						else if (strcmp(codec_header_message.codec, "opus") == 0) {
							uint32_t rate;
							memcpy(&rate, start+4,sizeof(rate));
							uint16_t bits;
							memcpy(&bits, start+8,sizeof(bits));
							memcpy(&channels, start+10,sizeof(channels));
							ESP_LOGI(TAG, "%s sampleformat: %d:%d:%d",codec_header_message.codec, rate, bits, channels);

							int error = 0;
							if (opusDecoder != NULL) {
								opus_decoder_destroy(opusDecoder);
								opusDecoder = NULL;
							}
							opusDecoder = opus_decoder_create(rate, channels, &error);
							if (error != 0) {
								ESP_LOGI(TAG, "Failed to init %s decoder", codec_header_message.codec);

							}
							ESP_LOGI(TAG, "Initialized %s decoder", codec_header_message.codec);

							//ESP_LOGI(TAG, "Codec : %s not implemented yet", codec_header_message.codec);

//							return;

							//ESP_LOGI(TAG, "Codec setting %d:%d:%d", rate,bits,channels);
						}
						else {
						   ESP_LOGI(TAG, "Codec : %s not supported", codec_header_message.codec);
						   ESP_LOGI(TAG, "Change encoder codec to flac in /etc/snapserver.conf on server");
						   return;
						}

						if (codecString != NULL) {
							free(codecString);
							codecString = NULL;
						}

						codecString = (char *)calloc(strlen(codec_header_message.codec) + 1, sizeof(char));
						if (codecString == NULL) {
							ESP_LOGW(TAG, "couldn't get memory for codec String");
						}
						else {
							strcpy(codecString, codec_header_message.codec);
						}

						tv1.tv_sec = base_message.sent.sec;
						tv1.tv_usec = base_message.sent.usec;
						settimeofday(&tv1, NULL);
						ESP_LOGI(TAG, "syncing clock to server %ld.%06ld", tv1.tv_sec, tv1.tv_usec);
//						gettimeofday(&tv1, NULL);
//						ESP_LOGI(TAG, "get time of day %ld.%06ld", tv1.tv_sec, tv1.tv_usec);

						codec_header_message_free(&codec_header_message);

						received_header = true;

					break;

					case SNAPCAST_MESSAGE_WIRE_CHUNK:
					{
						if (!received_header) {
							continue;
						}

						wire_chunk_message_t wire_chunk_message;

						result = wire_chunk_message_deserialize(&wire_chunk_message, start, size);
						if (result) {
							ESP_LOGI(TAG, "Failed to read wire chunk: %d\r\n", result);

							wire_chunk_message_free(&wire_chunk_message);
							break;
						}

//						ESP_LOGI(TAG, "wire chnk with size: %d, timestamp %d.%d", wire_chunk_message.size, wire_chunk_message.timestamp.sec, wire_chunk_message.timestamp.usec);

						// TODO: detect pcm chunk duration dynamically and allocate buffers accordingly.
						struct timeval tv_d1, tv_d2, tv_d3;
						tv_d1.tv_sec = wire_chunk_message.timestamp.sec;
						tv_d1.tv_usec = wire_chunk_message.timestamp.usec;
						tv_d2.tv_sec = wire_chunk_message_last.timestamp.sec;
						tv_d2.tv_usec = wire_chunk_message_last.timestamp.usec;
						timersub(&tv_d1, &tv_d2, &tv_d3);
						wire_chunk_message_last.timestamp = wire_chunk_message.timestamp;
//						ESP_LOGI(TAG, "chunk duration %ld.%06ld", tv_d3.tv_sec, tv_d3.tv_usec);
						if ((tv_d3.tv_sec * 1000000 + tv_d3.tv_usec) > (WIRE_CHUNK_DURATION_MS * 1000)) {
							ESP_LOGE(TAG, "wire chnk with size: %d, timestamp %d.%d, duration %ld.%06ld", wire_chunk_message.size, wire_chunk_message.timestamp.sec, wire_chunk_message.timestamp.usec, tv_d3.tv_sec, tv_d3.tv_usec);

							wire_chunk_message_free(&wire_chunk_message);

							continue;
						}

//
//						// store chunk's timestamp, decoder callback will need it later
						tv_t timestamp;
						timestamp = wire_chunk_message.timestamp;

//						if (wirechnkCnt == 0) {
//							ESP_LOGI(TAG, "set decoder timeout");
//
////								audio_element_set_input_timeout(decoder, pdMS_TO_TICKS(100));
////								audio_element_set_output_timeout(decoder, pdMS_TO_TICKS(100));
//						}
//						wirechnkCnt++;
//						ESP_LOGI(TAG, "got wire chunk %d, cnt %lld",(int)wire_chunk_message.size, wirechnkCnt );
//						ESP_LOGI(TAG, "got wire chunk cnt %lld", wirechnkCnt );
//						ESP_LOGI(TAG, "wirechnkCnt: %lld", wirechnkCnt);

						//if (strcmp(codecString, "flac") == 0) {
						if (0) {		// flac not supported
							int bytesWritten;
							bytesWritten = raw_stream_write(raw_stream_writer_to_decoder, wire_chunk_message.payload, (int)wire_chunk_message.size);
							if (bytesWritten < 0) {
								ESP_LOGE(TAG, "wirechnk decode ring buf timeout. bytes in buffer: %d/%d", rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)),
																										  audio_element_get_output_ringbuf_size(raw_stream_writer_to_decoder));
							}
							else if (bytesWritten < (int)wire_chunk_message.size) {
								ESP_LOGE(TAG, "wirechnk decode ring buf full");
							}
							else {
								if (xQueueSendToBack( timestampQueueHandle, &timestamp, pdMS_TO_TICKS(3000)) != pdTRUE) {
									ESP_LOGW(TAG, "timestamp queue full, messages waiting %d, dropping data ...", uxQueueMessagesWaiting(timestampQueueHandle));
								}
								else {
									//ESP_LOGI(TAG, "timestamps waiting %d", uxQueueMessagesWaiting(timestampQueueHandle));
								}
							}
						}
						else if (strcmp(codecString, "opus") == 0) {
							int frame_size = 0;

							audio = (int16_t *)heap_caps_malloc(frameSize * CHANNELS * (BITS_PER_SAMPLE / 8), MALLOC_CAP_SPIRAM); // 960*2: 20ms, 960*1: 10ms
							if (audio == NULL) {
								ESP_LOGE(TAG, "Failed to allocate memory for opus audio decoder");
							}
							else {
								size = wire_chunk_message.size;
								start = (wire_chunk_message.payload);

								while ((frame_size = opus_decode(opusDecoder, (unsigned char *)start, size, (opus_int16*)audio,
																 pcm_size/channels, 0)) == OPUS_BUFFER_TOO_SMALL)
								{
									pcm_size = pcm_size * 2;

									audio = (int16_t *)realloc(audio, pcm_size * CHANNELS * sizeof(int16_t)); // 960*2: 20ms, 960*1: 10ms

									ESP_LOGI(TAG, "OPUS encoding buffer too small, resizing to %d samples per channel", pcm_size/channels);
								}

								if (frame_size < 0 ) {
									ESP_LOGE(TAG, "Decode error : %d, %d, %s, %s, %d\n", frame_size, size, start, (char *)audio, pcm_size/channels);

									free(audio);
								}
								else {
									wire_chunk_message_t *pcm_chunk_message;

									pcm_chunk_message = (wire_chunk_message_t *)heap_caps_malloc(sizeof(wire_chunk_message_t), MALLOC_CAP_SPIRAM);
									if (pcm_chunk_message == NULL) {
										ESP_LOGE(TAG, "Failed to allocate memory for pcm chunk message");
									}
									else {
										pcm_chunk_message->size = frame_size * 2 * sizeof(uint16_t);
										pcm_chunk_message->timestamp = timestamp;
										pcm_chunk_message->payload = (char *)audio;
										if (xQueueSendToBack( pcmChunkQueueHandle, &pcm_chunk_message, pdMS_TO_TICKS(1000)) != pdTRUE) {
											ESP_LOGW(TAG, "send: pcmChunkQueue full, messages waiting %d", uxQueueMessagesWaiting(pcmChunkQueueHandle));

//											free(pcm_chunk_message->payload);
//											free(pcm_chunk_message);

											/*
											// free all memory,
											do {
												wire_chunk_message_t *chnk = NULL;

												BaseType_t ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(1) );
												if( ret != pdFAIL )	{
													if (chnk != NULL) {
														free(chnk->payload);
														free(chnk);
														chnk = NULL;
													}
												}
											} while (uxQueueMessagesWaiting(pcmChunkQueueHandle) > 0);
											xQueueReset(pcmChunkQueueHandle);
											*/
										}
									}
								}
							}
						}
						else {
							ESP_LOGE(TAG, "Decoder not supported");
						}

						wire_chunk_message_free(&wire_chunk_message);

						break;
					}

					case SNAPCAST_MESSAGE_SERVER_SETTINGS:
						// The first 4 bytes in the buffer are the size of the string.
						// We don't need this, so we'll shift the entire buffer over 4 bytes
						// and use the extra room to add a null character so cJSON can pares it.
						memmove(start, start + 4, size - 4);
						start[size - 3] = '\0';
						result = server_settings_message_deserialize(&server_settings_message, start);
						if (result) {
							ESP_LOGI(TAG, "Failed to read server settings: %d\r\n", result);
							return;
						}
						// log mute state, buffer, latency
						buffer_ms = server_settings_message.buffer_ms;
						ESP_LOGI(TAG, "Buffer length:  %d", server_settings_message.buffer_ms);
						//ESP_LOGI(TAG, "Ringbuffer size:%d", server_settings_message.buffer_ms*48*4);
						ESP_LOGI(TAG, "Latency:        %d", server_settings_message.latency);
						ESP_LOGI(TAG, "Mute:           %d", server_settings_message.muted);
						ESP_LOGI(TAG, "Setting volume: %d", server_settings_message.volume);
						muteCH[0] = server_settings_message.muted;
						muteCH[1] = server_settings_message.muted;
						muteCH[2] = server_settings_message.muted;
						muteCH[3] = server_settings_message.muted;

						// Volume setting using ADF HAL abstraction
						audio_hal_set_mute(board_handle->audio_hal, server_settings_message.muted);
						audio_hal_set_volume(board_handle->audio_hal, server_settings_message.volume);

						if (syncTaskHandle == NULL) {
							ESP_LOGI(TAG, "Start snapcast_sync_task");

//							snapcastTaskCfg.outputBufferDacTime_us = outputBufferDacTime_us;
//							snapcastTaskCfg.buffer_us = (int64_t)buffer_ms * 1000LL;
							xTaskCreatePinnedToCore(snapcast_sync_task, "snapcast_sync_task", 8 * 1024, NULL, SYNC_TASK_PRIORITY, &syncTaskHandle, SYNC_TASK_CORE_ID);
						}

						// notify task of changed parameters
						xTaskNotify( syncTaskHandle,
									 buffer_ms,
									 eSetBits
									);

					break;

					case SNAPCAST_MESSAGE_TIME:
						result = time_message_deserialize(&time_message, start, size);
						if (result) {
							ESP_LOGI(TAG, "Failed to deserialize time message\r\n");
							return;
						}
	//                    ESP_LOGI(TAG, "BaseTX     : %d %d ", base_message.sent.sec , base_message.sent.usec);
	//                    ESP_LOGI(TAG, "BaseRX     : %d %d ", base_message.received.sec , base_message.received.usec);
	//                    ESP_LOGI(TAG, "baseTX->RX : %d s ", (base_message.received.sec - base_message.sent.sec));
	//                    ESP_LOGI(TAG, "baseTX->RX : %d ms ", (base_message.received.usec - base_message.sent.usec)/1000);
	//                    ESP_LOGI(TAG, "Latency : %d.%d ", time_message.latency.sec,  time_message.latency.usec/1000);

						// tv == server to client latency (s2c)
						// time_message.latency == client to server latency(c2s)
						// TODO the fact that I have to do this simple conversion means
						// I should probably use the timeval struct instead of my own
						tv1.tv_sec = base_message.received.sec;
						tv1.tv_usec = base_message.received.usec;
						tv3.tv_sec = base_message.sent.sec;
						tv3.tv_usec = base_message.sent.usec;
						timersub(&tv1, &tv3, &tv2);
						tv1.tv_sec = time_message.latency.sec;
						tv1.tv_usec = time_message.latency.usec;

						// tv1 == c2s: client to server
						// tv2 == s2c: server to client
	//                    ESP_LOGI(TAG, "c2s: %ld %ld", tv1.tv_sec, tv1.tv_usec);
	//                    ESP_LOGI(TAG, "s2c:  %ld %ld", tv2.tv_sec, tv2.tv_usec);

						timersub(&tv1, &tv2, &tmpDiffToServer);
						if ((tmpDiffToServer.tv_sec / 2) == 0) {
							tmpDiffToServer.tv_sec = 0;
							tmpDiffToServer.tv_usec = (suseconds_t)((int64_t)tmpDiffToServer.tv_sec * 1000000LL / 2) + tmpDiffToServer.tv_usec / 2;
						}
						else
						{
							tmpDiffToServer.tv_sec /= 2;
							tmpDiffToServer.tv_usec /= 2;
						}

//						ESP_LOGI(TAG, "Current latency: %ld.%06ld", tmpDiffToServer.tv_sec, tmpDiffToServer.tv_usec);

						// following code is storing / initializing / resetting diff to server algorithm
						// we collect a number of latencies and apply a median filter. Based on these we can get server now
						{
							struct timeval diff;
							int64_t medianValue, newValue;

							// clear diffBuffer if last update is older than a minute
							timersub(&now, &lastTimeSync, &diff);

							if ( diff.tv_sec > 60 ) {
								ESP_LOGW(TAG, "Last time sync older than a minute. Clearing time buffer");

								reset_latency_buffer();
							}

							newValue = ((int64_t)tmpDiffToServer.tv_sec * 1000000LL + (int64_t)tmpDiffToServer.tv_usec);
							medianValue = MEDIANFILTER_Insert(&latencyMedianFilterLong, newValue);
							if (xSemaphoreTake( latencyBufSemaphoreHandle, pdMS_TO_TICKS(1) ) == pdTRUE) {
								if (MEDIANFILTER_isFull(&latencyMedianFilterLong)) {
									latencyBuffFull = true;
								}

								latencyToServer =  medianValue;

								xSemaphoreGive( latencyBufSemaphoreHandle );
							}
							else {
								ESP_LOGW(TAG, "couldn't set latencyToServer =  medianValue");
							}

							// store current time
							lastTimeSync.tv_sec = now.tv_sec;
							lastTimeSync.tv_usec = now.tv_usec;

							// we don't care if it was already taken, just make sure it is taken at this point
							xSemaphoreTake( timeSyncSemaphoreHandle, 0 );

							if (latency_buffer_full() > 0) {
								// we give timeSyncSemaphoreHandle after x µs through timer
								esp_timer_start_periodic(timeSyncMessageTimer, 1000000);
							}
							else {
								// Do a initial time sync with the server at boot
								// give semaphore for immediate next run
								// we need to fill diffBuff fast so we get a good estimate of latency
								xSemaphoreGive(timeSyncSemaphoreHandle);
							}
						}

					break;
				}
            }

            if (received_header == true) {
            	if (xSemaphoreTake(timeSyncSemaphoreHandle, 0) == pdTRUE) {
            		result = gettimeofday(&now, NULL);
					//ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec, now.tv_usec);
					if (result) {
						ESP_LOGI(TAG, "Failed to gettimeofday");
						continue;
					}

					base_message.type = SNAPCAST_MESSAGE_TIME;
					base_message.id = id_counter++;
					base_message.refersTo = 0;
					base_message.received.sec = 0;
					base_message.received.usec = 0;
					base_message.sent.sec = now.tv_sec;
					base_message.sent.usec = now.tv_usec;
					base_message.size = TIME_MESSAGE_SIZE;

					result = base_message_serialize(
														&base_message,
														base_message_serialized,
														BASE_MESSAGE_SIZE
													);
					if (result) {
						ESP_LOGE(TAG, "Failed to serialize base message for time\r\n");
						continue;
					}

					result = time_message_serialize(&time_message, buff, BUFF_LEN);
					if (result) {
						ESP_LOGI(TAG, "Failed to serialize time message\r\b");
						continue;
					}

					write(sockfd, base_message_serialized, BASE_MESSAGE_SIZE);
					write(sockfd, buff, TIME_MESSAGE_SIZE);

//					ESP_LOGI(TAG, "sent time sync message %ld.%06ld", now.tv_sec, now.tv_usec);
				}
            }
        }

        esp_timer_stop(timeSyncMessageTimer);
        esp_timer_delete(timeSyncMessageTimer);
        xSemaphoreGive(timeSyncSemaphoreHandle);

//        if (syncTaskHandle != NULL) {
			do {
				wire_chunk_message_t *chnk = NULL;

				BaseType_t ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(1) );
				if( ret != pdFAIL )	{
					if (chnk != NULL) {
						free(chnk->payload);
						free(chnk);
						chnk = NULL;
					}
				}
			} while (uxQueueMessagesWaiting(pcmChunkQueueHandle) > 0);
//			xQueueReset(pcmChunkQueueHandle);

//			vTaskDelete(syncTaskHandle);
//			syncTaskHandle = NULL;
//		}

//        tg0_timer_deinit();

        if (opusDecoder != NULL) {
			opus_decoder_destroy(opusDecoder);
			opusDecoder = NULL;
		}

        reset_latency_buffer();
        if (xSemaphoreTake( latencyBufSemaphoreHandle, pdMS_TO_TICKS(1) ) == pdTRUE) {
			latencyBuffFull = false;
			latencyToServer =  0;

			xSemaphoreGive( latencyBufSemaphoreHandle );
		}
		else {
			ESP_LOGW(TAG, "couldn't reset latency");
		}

        if (sockfd != -1) {
        	shutdown(sockfd, 0);
        	//close(sockfd);
        	closesocket(sockfd);

        	sockfd = -1;
        }


        ESP_LOGI(TAG, "... closing socket\r\n");
    }
}


/**
 *
 */
void sntp_sync_time(struct timeval *tv_ntp) {
  if ((sntp_synced%10) == 0) {
    settimeofday(tv_ntp,NULL);
    sntp_synced++;
    ESP_LOGI(TAG,"SNTP time set from server number :%d",sntp_synced);
    return;
  }
  sntp_synced++;
  struct timeval tv_esp;
  gettimeofday(&tv_esp, NULL);
  //ESP_LOGI(TAG,"SNTP diff  s: %ld , %ld ", tv_esp.tv_sec , tv_ntp->tv_sec);
  ESP_LOGI(TAG,"SNTP diff us: %ld , %ld ", tv_esp.tv_usec , tv_ntp->tv_usec);
  ESP_LOGI(TAG,"SNTP diff us: %.2f", (double)((tv_esp.tv_usec - tv_ntp->tv_usec)/1000.0));

}

/**
 *
 */
void sntp_cb(struct timeval *tv) {
	struct tm timeinfo = { 0 };
    time_t now = tv->tv_sec;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "sntp_cb called :%s", strftime_buf);
}

/**
 *
 */
void set_time_from_sntp() {
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT,
                        false, true, portMAX_DELAY);
    //ESP_LOGI(TAG, "clock %");

    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "europe.pool.ntp.org");
	sntp_init();
    sntp_set_time_sync_notification_cb(sntp_cb);
    setenv("TZ", "UTC-2", 1);
    tzset();

    /*
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    char strftime_buf[64];

    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time in UTC is: %s", strftime_buf);
     */
}


#define CONFIG_MASTER_I2S_BCK_PIN		5
#define CONFIG_MASTER_I2S_LRCK_PIN		25
#define CONFIG_MASTER_I2S_DATAOUT_PIN	26
#define CONFIG_SLAVE_I2S_BCK_PIN		26
#define CONFIG_SLAVE_I2S_LRCK_PIN		12
#define CONFIG_SLAVE_I2S_DATAOUT_PIN	5

esp_err_t setup_dsp_i2s(uint32_t sample_rate, i2s_port_t i2sNum)
{
	int chunkInFrames = chunkInBytes /  (CHANNELS * (BITS_PER_SAMPLE / 8));
	int __dmaBufCnt;
	int __dmaBufLen;
	const int __dmaBufMaxLen = 1024;

	__dmaBufCnt = 1;
	__dmaBufLen = chunkInFrames;
	while ((__dmaBufLen >= __dmaBufMaxLen) || (__dmaBufCnt <= 1)) {
		if (( __dmaBufLen % 2 ) == 0) {
			__dmaBufCnt *= 2;
			__dmaBufLen /= 2;
		}
		else {
			ESP_LOGE(TAG, "setup_dsp_i2s: Can't setup i2s with this configuration");

			return -1;
		}
	}

	i2sDmaLAtency = (1000000LL * __dmaBufLen / SAMPLE_RATE) * 0.8;		// this value depends on __dmaBufLen, optimized for __dmaBufLen = 480 @opus, 192000bps, complexity 5
																		// it will be smaller for lower values of __dmaBufLen. The delay was measured against raspberry client
																		// TODO: find a way to calculate this value without the need to measure delay to another client

	i2sDmaBufCnt = __dmaBufCnt * 2 + 1;	// we do double buffering of chunks at I2S DMA, +1 needed because of the way i2s driver works, it will only allocate (i2sDmaBufCnt - 1) queue elements

	ESP_LOGI(TAG, "setup_dsp_i2s: dma_buf_len is %d, dma_buf_count is %d", __dmaBufLen, __dmaBufCnt*2);

	i2s_config_t i2s_config0 = {
									.mode = I2S_MODE_MASTER | I2S_MODE_TX,                                  // Only TX
									.sample_rate = sample_rate,
									.bits_per_sample = BITS_PER_SAMPLE,
									.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           // 2-channels
									.communication_format = I2S_COMM_FORMAT_STAND_I2S,
									.dma_buf_count = i2sDmaBufCnt,
									.dma_buf_len = __dmaBufLen,
									.intr_alloc_flags = 1,                                                  //Default interrupt priority
									.use_apll = true,
									.fixed_mclk = 0,
									.tx_desc_auto_clear = true                                              // Auto clear tx descriptor on underflow
								};

	i2s_pin_config_t pin_config0 =	{
										.bck_io_num   = CONFIG_MASTER_I2S_BCK_PIN,
										.ws_io_num    = CONFIG_MASTER_I2S_LRCK_PIN,
										.data_out_num = CONFIG_MASTER_I2S_DATAOUT_PIN,
										.data_in_num  = -1                                                       //Not used
									};

	i2s_driver_install(i2sNum, &i2s_config0, 1, &i2s_event_queue);
	i2s_set_pin(i2sNum, &pin_config0);

 	return 0;
}

/**
 *
 */
void app_main(void) {
    esp_err_t ret;
    uint8_t base_mac[6];

    //ESP_ERROR_CHECK(nvs_flash_erase());

    ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// NVS partition was truncated
		// and needs to be erased
		ESP_ERROR_CHECK(nvs_flash_erase());

		/* Retry nvs_flash_init */
		ESP_ERROR_CHECK(nvs_flash_init());
	}

	{	// init wifi
		// Initialize TCP/IP
		ESP_ERROR_CHECK(esp_netif_init());
		// Initialize the event loop
		ESP_ERROR_CHECK(esp_event_loop_create_default());
		s_wifi_event_group = xEventGroupCreate();
		wifi_init();
	}

    esp_timer_init();

    tg0_timer_init();		// initialize initial sync timer

	// Get MAC address for WiFi station
	esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
	sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
	ESP_LOGI(TAG, "MAC Adress is: %s", mac_address);

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Start codec chip");
    board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    i2s_mclk_gpio_select(I2S_NUM_0, GPIO_NUM_0);
    ret = setup_dsp_i2s(48000, I2S_NUM_0);
    if (ret < 0) {
    	return;
    }

    ESP_LOGI(TAG, "Listen for all pipeline events");


#if CONFIG_USE_SNTP == 1
    // syncing to sntp
	vTaskDelay(5000/portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "Syncing to sntp");
	set_time_from_sntp();
#else
	{
		// don't use sntp, if server and client are too different, we get overflowing timevals,
		// insted we sync our clock to the server on reception of codec header
		struct timeval tv = {
				.tv_sec = 0,
				.tv_usec = 0,
		};
		char tmbuf[64], buf[128];
		struct tm *nowtm;
		time_t nowtime;

		settimeofday(&tv, NULL);

		nowtime = tv.tv_sec;
		nowtm = localtime(&nowtime);
		strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", nowtm);
		sprintf(buf, "%s.%06ld", tmbuf, tv.tv_usec);
		ESP_LOGI(TAG, "Current time is %s", buf);
	}
#endif

	ESP_LOGI(TAG, "Start snapclient task");

	timestampQueueHandle = xQueueCreateStatic( TIMESTAMP_QUEUE_LENGTH,
											   sizeof(tv_t),
											   timestampQueueStorageArea,
											   &timestampQueue
											 );

	xTaskCreatePinnedToCore(http_get_task, "http_get_task", 4*4096, NULL, HTTP_TASK_PRIORITY, NULL, HTTP_TASK_CORE_ID);

#if COLLECT_RUNTIME_STATS == 1
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret;

        vTaskDelay(portMAX_DELAY);

        /*
        // listen to events
        ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) decoder &&
        	msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
        {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(decoder, &music_info);

            if (codecString != NULL) {
				ESP_LOGI(TAG, "[ * ] Receive music info from %s decoder, sample_rates=%d, bits=%d, ch=%d",
									 codecString, music_info.sample_rates, music_info.bits, music_info.channels);
        	}
            else {
            	ESP_LOGI(TAG, "[ * ] Receive music info from decoder, sample_rates=%d, bits=%d, ch=%d",
            						 music_info.sample_rates, music_info.bits, music_info.channels);
            }

            continue;
        }
        else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) decoder &&
        		 msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
        {
        	ESP_LOGW(TAG, "report status: %d", (int)msg.data);
        }
        else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) decoder) {
        	ESP_LOGW(TAG, "decoder status: %d", msg.cmd);
        }
        */
    }

    // TODO: clean up all created tasks and delete them
}



