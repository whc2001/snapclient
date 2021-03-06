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

#include "snapcast.h"

#include <math.h>

#include <sys/time.h>

#define TEST_WIRECHUNK_TO_PCM_PART		1

#define COLLECT_RUNTIME_STATS	0

// @ 48kHz, 2ch, 16bit audio data and 24ms wirechunks (hardcoded for now) we expect 0.024 * 2 * 16/8 * 48000 = 4608 Bytes
#define WIRE_CHUNK_DURATION_MS		24UL		// stream read chunk size [ms]
#define SAMPLE_RATE					48000UL
#define CHANNELS					2UL
#define BYTE_PER_SAMPLE				2UL


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
	{16, 48005, 213, 212, 5, 6},		// ~ 48kHz * 1.0001
	{16, 47995, 84, 212, 5, 6},		// ~ 48kHz * 0.9999
};

i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();

audio_pipeline_handle_t flacDecodePipeline;
audio_element_handle_t raw_stream_writer_to_decoder, decoder, raw_stream_reader_from_decoder;
audio_pipeline_handle_t playbackPipeline;
audio_element_handle_t raw_stream_writer_to_i2s, i2s_stream_writer;


uint64_t wirechnkCnt = 0;
uint64_t pcmchnkCnt = 0;

TaskHandle_t syncTaskHandle = NULL;

#define CONFIG_USE_SNTP		0

#define DAC_OUT_BUFFER_TIME_US		24000		//TODO: not sure about this... I2S DMA buffer length ???
												//		Has to be measured probably because I can't find any information regarding this in codec's datasheet

static const char *TAG = "SC";

static int sntp_synced = 0;

char *codecString = NULL;

// configMAX_PRIORITIES - 1

// TODO: what are the best values here?
#define SYNC_TASK_PRIORITY		6//configMAX_PRIORITIES - 2
#define SYNC_TASK_CORE_ID  		tskNO_AFFINITY//1//tskNO_AFFINITY

#define TIMESTAMP_TASK_PRIORITY	6
#define TIMESTAMP_TASK_CORE_ID 	tskNO_AFFINITY// 0//1//tskNO_AFFINITY

#define HTTP_TASK_PRIORITY		6
#define HTTP_TASK_CORE_ID  		tskNO_AFFINITY//0//tskNO_AFFINITY

#define I2S_TASK_PRIORITY  		6//6//configMAX_PRIORITIES - 1
#define I2S_TASK_CORE_ID  		tskNO_AFFINITY//1//tskNO_AFFINITY

#define FLAC_DECODER_PRIORITY	6
#define FLAC_DECODER_CORE_ID	tskNO_AFFINITY//0//tskNO_AFFINITY



QueueHandle_t timestampQueueHandle;
#define TIMESTAMP_QUEUE_LENGTH 	100
static StaticQueue_t timestampQueue;
uint8_t timestampQueueStorageArea[ TIMESTAMP_QUEUE_LENGTH * sizeof(tv_t) ];

QueueHandle_t pcmChunkQueueHandle;
#define PCM_CHNK_QUEUE_LENGTH 250	// TODO: one chunk is hardcoded to 24ms, change it to be dynamically adjustable. 1s buffer ~ 42
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

SemaphoreHandle_t diffBufSemaphoreHandle = NULL;

SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

SemaphoreHandle_t timer0_syncSampleSemaphoreHandle = NULL;

#define DIFF_BUF_LEN					1
uint8_t diffBufCnt = 0;
static int8_t diffBuffFull = 0;
static struct timeval diffToServer = {0, 0};	// median diff to server in µs
static struct timeval diffBuf[DIFF_BUF_LEN] = {0};	// collected diff's to server
//static struct timeval *medianArray = NULL;	// temp median calculation data is stored at this location
static struct timeval medianArray[DIFF_BUF_LEN] = {0};	// temp median calculation data is stored at this location

uint32_t buffer_ms = 400;
uint8_t  muteCH[4] = {0};
audio_board_handle_t board_handle;

/* Constants that aren't configurable in menuconfig */
#define HOST "192.168.1.6"
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

#define MY_SSID		"....."
#define MY_WPA2_PSK "xxxxxxxx"

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
			.ssid = MY_SSID,//CONFIG_ESP_WIFI_SSID,
			.password = MY_WPA2_PSK,//CONFIG_ESP_WIFI_PASSWORD,
			.bssid_set = false
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(	s_wifi_event_group,
											WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
											pdFALSE,
											pdFALSE,
											portMAX_DELAY );

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to AP ...");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    //ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    //ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    //vEventGroupDelete(s_wifi_event_group);
}

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
void quick_sort_timeval(struct timeval *a, int left, int right) {
	int i = left;
	int j = right;
	struct timeval temp = a[i];

	if( left < right ) {
		while(i < j) {
			while(timercmp(&a[j], &temp, >=) && (i < j)) {
				j--;
			}
			a[i] = a[j];

			while(timercmp(&a[j], &temp, <=) && (i < j)) {
				i++;
			}
			a[j] = a[i];
		}
		a[i] = temp;

		quick_sort_timeval( a, left, i - 1 );
		quick_sort_timeval( a, j + 1, right );
	}
}

/**
 *
 */
void quick_sort_int32(int32_t *a, int left, int right) {
	int i = left;
	int j = right;
	int32_t temp = a[i];

	if( left < right ) {
		while(i < j) {
			while(a[j] >= temp && (i < j)) {
				j--;
			}
			a[i] = a[j];

			while(a[j] <= temp && (i < j)) {
				i++;
			}
			a[j] = a[i];
		}
		a[i] = temp;

		quick_sort_int32( a, left, i - 1 );
		quick_sort_int32( a, j + 1, right );
	}
}

/**
 *
 */
void quick_sort_int64(int64_t *a, int left, int right) {
	int i = left;
	int j = right;
	int64_t temp = a[i];

	if( left < right ) {
		while(i < j) {
			while(a[j] >= temp && (i < j)) {
				j--;
			}
			a[i] = a[j];

			while(a[j] <= temp && (i < j)) {
				i++;
			}
			a[j] = a[i];
		}
		a[i] = temp;

		quick_sort_int64( a, left, i - 1 );
		quick_sort_int64( a, j + 1, right );
	}
}


/**
 *
 */
int8_t get_median( const struct timeval *tDiff, size_t n, struct timeval *result ) {
	struct timeval median;

	if (tDiff == NULL) {
		ESP_LOGE(TAG, "get_median: buffer error");

		return -1;
	}

	if (n == 0) {
		median = tDiff[0];
		*result = median;

		return 0;
	}

	memcpy( medianArray, tDiff, sizeof(struct timeval) * n );	// TODO: how to avoid this copy?
	quick_sort_timeval(medianArray, 0, n);

//    if( (n % 2) == 0 ) {
//        // if there is an even number of elements, return mean of the two elements in the middle
//    	timeradd(&medianArray[n/2], &medianArray[n/2 - 1], &median);
//    	if ((median.tv_sec / 2) == 0) {
//    		median.tv_sec = 0;
//    		median.tv_usec = (suseconds_t)((int64_t)median.tv_sec * 1000000LL / 2) + median.tv_usec / 2;
//		}
//		else
//		{
//			median.tv_sec /= 2;
//			median.tv_usec /= 2;
//		}
//    }
//    else
    {
        // else return the element in the middle
    	median = medianArray[n/2];
    }

	*result = median;

	return 0;
}

/**
 *
 */
int8_t set_diff_to_server( struct timeval *tDiff, size_t len) {
	int8_t ret = -1;
	struct timeval tmpDiffToServer;

    ret = get_median(tDiff, len, &tmpDiffToServer);
    if (ret < 0) {
    	ESP_LOGW(TAG, "set_diff_to_server: get median failed");
    }

    //ESP_LOGI(TAG, "set_diff_to_server: median is %ld.%06ld", tmpDiffToServer.tv_sec, tmpDiffToServer.tv_usec);

    if (len >= (sizeof(diffBuf) / (sizeof(struct timeval)))) {
    	diffBuffFull = 1;
    }
    else {
    	diffBuffFull = 0;
    }

    diffToServer =  tmpDiffToServer;

	return ret;
}
// TODO: implement diff buffer using some sort of fifo
//		 current implementation isn't very good nor user friendly

int8_t reset_diff_buffer(void) {
	if (xSemaphoreTake( diffBufSemaphoreHandle, 1 ) == pdFALSE) {
		ESP_LOGW(TAG, "reset_diff_buffer: can't take semaphore");

		return -1;
	}

	memset(diffBuf, 0, sizeof(diffBuf));
	diffBufCnt = 0;
	diffBuffFull = false;

	xSemaphoreGive( diffBufSemaphoreHandle );

	return 0;
}

int8_t add_to_diff_buffer(struct timeval tv) {
	size_t bufLen;

	if (xSemaphoreTake( diffBufSemaphoreHandle, 1 ) == pdFALSE) {
		ESP_LOGW(TAG, "add_to_diff_buffer: can't take semaphore");

		return -1;
	}

	diffBuf[diffBufCnt++] = tv;
	if (diffBufCnt >= DIFF_BUF_LEN) {
		diffBuffFull = true;

		diffBufCnt = 0;
	}

	if (diffBuffFull == true) {
		bufLen = DIFF_BUF_LEN;
	}
	else {
		bufLen = diffBufCnt;
	}

	set_diff_to_server(diffBuf, bufLen);

	xSemaphoreGive( diffBufSemaphoreHandle );

	return 0;
}

/**
 *
 */
int8_t diff_buffer_full(void) {
	int8_t tmp;

	if (xSemaphoreTake( diffBufSemaphoreHandle, 0) == pdFALSE) {
		//ESP_LOGW(TAG, "diff_buffer_full: can't take semaphore");

		return -1;
	}

	tmp = diffBuffFull;

	xSemaphoreGive( diffBufSemaphoreHandle );

	return tmp;
}

/**
 *
 */
int8_t get_diff_to_server( struct timeval *tDiff ) {
	static struct timeval lastDiff = { 0, 0 };

	if (diffBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "get_diff_to_server: diffBufSemaphoreHandle == NULL");

		return -1;
	}

	if (xSemaphoreTake( diffBufSemaphoreHandle, 0 ) == pdFALSE) {
		*tDiff = lastDiff;

		//ESP_LOGW(TAG, "get_diff_to_server: can't take semaphore. Old diff retreived");

		return -2;
	}

	*tDiff = diffToServer;
	lastDiff = diffToServer;		// store value, so we can return a value if semaphore couldn't be taken

	xSemaphoreGive( diffBufSemaphoreHandle );

	return 0;
}

/**
 *
 */
int8_t server_now( struct timeval *sNow ) {
	struct timeval now;
	struct timeval diff;

	// get current time
	if (gettimeofday(&now, NULL)) {
		ESP_LOGE(TAG, "server_now: Failed to get time of day");

		return -1;
	}

	if (get_diff_to_server(&diff) == -1) {
		ESP_LOGE(TAG, "server_now: can't get diff to server");

		return -1;
	}

	if ((diff.tv_sec == 0) && (diff.tv_usec == 0)) {
		//ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

		return -1;
	}

	timeradd(&now, &diff, sNow);

//	ESP_LOGI(TAG, "now: %lldus", (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec);
//	ESP_LOGI(TAG, "diff: %lldus", (int64_t)diff.tv_sec * 1000000LL + (int64_t)diff.tv_usec);
//	ESP_LOGI(TAG, "serverNow: %lldus", (int64_t)sNow->tv_sec * 1000000LL + (int64_t)sNow->tv_usec);

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
                            1,
                            eSetBits,
                            &xHigherPriorityTaskWoken );
    }

    timer_spinlock_give(TIMER_GROUP_0);

	if( xHigherPriorityTaskWoken )	{
		portYIELD_FROM_ISR ();
	}
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

static void tg0_timer1_start(uint64_t alarm_value) {
	timer_pause(TIMER_GROUP_0, TIMER_1);
	//timer_set_counter_value(TIMER_GROUP_0, TIMER_1, timer_start_value_us);
	timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, alarm_value);
    timer_set_alarm(TIMER_GROUP_0, TIMER_1, TIMER_ALARM_EN);
    timer_start(TIMER_GROUP_0, TIMER_1);

//    ESP_LOGI(TAG, "started age timer");
}




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

//buffer_.setSize(500);
//    shortBuffer_.setSize(100);
//    miniBuffer_.setSize(20);

#define MAX_SHORT_BUFFER_COUNT	20
int64_t short_buffer[MAX_SHORT_BUFFER_COUNT];
int64_t short_buffer_median[MAX_SHORT_BUFFER_COUNT];
int short_buffer_cnt = 0;
int short_buffer_full = 0;

//#define MAX_MINI_BUFFER_COUNT	50
//int64_t mini_buffer[MAX_MINI_BUFFER_COUNT];
//int64_t mini_buffer_median[MAX_MINI_BUFFER_COUNT];
//int mini_buffer_cnt = 0;

int8_t currentDir = 0;

void adjust_apll(int8_t direction) {
	int sdm0, sdm1, sdm2, o_div;

	// only change if necessary
	if (currentDir == direction) {
		return;
	}

	if (direction == 1) {
		// speed up
		sdm0 = apll_predefine_48k_corr[2][2];
		sdm1= apll_predefine_48k_corr[2][3];
		sdm2 = apll_predefine_48k_corr[2][4];
		o_div = apll_predefine_48k_corr[2][5];
	}
	else if (direction == -1) {
		// slow down
		sdm0 = apll_predefine_48k_corr[3][2];
		sdm1= apll_predefine_48k_corr[3][3];
		sdm2 = apll_predefine_48k_corr[3][4];
		o_div = apll_predefine_48k_corr[3][5];
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
	snapcast_sync_task_cfg_t *taskCfg = (snapcast_sync_task_cfg_t *)pvParameters;
	wire_chunk_message_t *chnk = NULL;
	struct timeval serverNow = {0, 0};
	int64_t age;
	BaseType_t ret;
	int64_t chunkDuration_us = 24000;
	int64_t chunkDuration_ns = 24000 * 1000;
	int64_t sampleDuration_ns = (1000000 / 48); // 16bit, 2ch, 48kHz (in nano seconds)
	char *p_payload = NULL;
	size_t size = 0;
	uint32_t notifiedValue;
	uint64_t timer_val;
	const int32_t alarmValSubBase = 500;
	int32_t alarmValSub = 0;
	int bytesWritten = 0;
	int initialSync = 0;
	int sdm0, sdm1, sdm2, o_div;
	int64_t avg;
	struct timeval latencyInitialSync = {0, 0};

	ESP_LOGI(TAG, "started sync task");

	tg0_timer_init();		// initialize sample sync timer

	initialSync = 0;

	short_buffer_cnt = 0;
	memset(short_buffer, 0, sizeof(short_buffer));

	// use 48kHz sample rate
	sdm0 = apll_predefine[5][2];
	sdm1= apll_predefine[5][3];
	sdm2 = apll_predefine[5][4];
	o_div = apll_predefine[5][5];

	rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

	while(1) {
		if (chnk == NULL) {
//			ESP_LOGE(TAG, "msg waiting pcm %d ts %d", uxQueueMessagesWaiting(pcmChunkQueueHandle), uxQueueMessagesWaiting(timestampQueueHandle));

			ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(2000) );
			if( ret == pdPASS )	{
				//ESP_LOGW(TAG, "pcm chunks waiting %d", uxQueueMessagesWaiting(pcmChunkQueueHandle));
			}
		}
		else {
			ret = pdPASS;
		}

		if( ret == pdPASS )	{
			// wait for early time syncs to be ready
			if ( diff_buffer_full() <= 0 ) {
				// free chunk so we can get next one
				free(chnk->payload);
				free(chnk);
				chnk = NULL;

				vTaskDelay(10);

				continue;
			}


			if (server_now(&serverNow) >= 0) {
//				if (initialSync == 0)
				{
					// first chunk needs to be at exact time point
					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
							((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
							(int64_t)taskCfg->buffer_us +
							(int64_t)taskCfg->outputBufferDacTime_us;
				}
//				else {
//					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
//							((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
//							(int64_t)taskCfg->buffer_us +
//							(int64_t)taskCfg->outputBufferDacTime_us + chunkDuration_us;
//				}

				if ((age < 0) && (initialSync == 0)) {		// get initial sync using hardware timer
					//tg0_timer1_start((-age * 10) - (alarmValSubBase + alarmValSub));	// alarm a little earlier to account for context switch duration from freeRTOS
					tg0_timer1_start(-age - alarmValSub);	// alarm a little earlier to account for context switch duration from freeRTOS

					// Wait to be notified of an interrupt.
					xTaskNotifyWait( pdFALSE,    		// Don't clear bits on entry.
									 ULONG_MAX,        	// Clear all bits on exit.
									 &notifiedValue, 	// Stores the notified value.
									 portMAX_DELAY
									);

					// get timer value so we can get the real age
					timer_get_counter_value(TIMER_GROUP_0, TIMER_1, &timer_val);
					timer_pause(TIMER_GROUP_0, TIMER_1);

					//age = ((int64_t)timer_val - (-age) * 10) / 10;
					age = (int64_t)timer_val - (-age);
					if (((age < -11LL) || (age > 0)) && (initialSync == 0)) {
						if (age < -11LL) {
							alarmValSub--;
						}
						else {
							alarmValSub++;
						}

						// free chunk so we can get next one
						free(chnk->payload);
						free(chnk);
						chnk = NULL;

						struct timeval t;
						get_diff_to_server(&t);
						ESP_LOGI(TAG, "no hard sync, age %lldus, %lldus", age, ((int64_t)t.tv_sec * 1000000LL + (int64_t)t.tv_usec));

						continue;
					}
					else if (initialSync == 0) {
//						i2s_start(i2s_cfg.i2s_port);

						ESP_LOGW(TAG, "start");
					}

					get_diff_to_server(&latencyInitialSync);

					initialSync = 1;

					p_payload = chnk->payload;
					size = chnk->size;

					// write data to I2S
					bytesWritten = raw_stream_write(raw_stream_writer_to_i2s, p_payload, size);
					if (bytesWritten < 0) {
						ESP_LOGE(TAG, "i2s raw writer ring buf timeout");
					}
					else if (bytesWritten < size) {
						ESP_LOGE(TAG, "i2s raw writer ring buf full");
					}

//					ESP_LOGI(TAG, "bytesWritten %d", bytesWritten);

					ESP_LOGI(TAG, "%lldus, %d, %d, %d %lldus", age, sdm0, sdm1, sdm2, ((int64_t)latencyInitialSync.tv_sec * 1000000LL + (int64_t)latencyInitialSync.tv_usec));

					continue;
				}
				else {
					//if ((age < -2 * chunkDuration_us) || (age > 2 * chunkDuration_us) || (initialSync == 0)) {
					if ((age < -2 * chunkDuration_us) || (age > 2 * chunkDuration_us) || (initialSync == 0)) {
						free(chnk->payload);
						free(chnk);
						chnk = NULL;

						struct timeval t;
						get_diff_to_server(&t);
						ESP_LOGW(TAG, "RESYNCING HARD %lldus, %lldus", age, ((int64_t)t.tv_sec * 1000000LL + (int64_t)t.tv_usec));
//						ESP_LOGW(TAG, "RESYNCING HARD %lldus, %lldus", age);

						//reset_diff_buffer();

						if (initialSync == 1) {
//							i2s_stop(i2s_cfg.i2s_port);
//							i2s_zero_dma_buffer(i2s_cfg.i2s_port);

							audio_element_reset_input_ringbuf(i2s_stream_writer);

							// reset to normal playback speed
							sdm0 = apll_predefine[5][2];
							sdm1= apll_predefine[5][3];
							sdm2 = apll_predefine[5][4];
							o_div = apll_predefine[5][5];
							rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

							ESP_LOGW(TAG, "stop");
						}

						memset( short_buffer, 0, sizeof(short_buffer) );
						short_buffer_cnt = 0;
						short_buffer_full = 0;

						initialSync = 0;
						alarmValSub = 0;

						continue;
					}
				}

				int64_t age_expect;
				// NOT 100% SURE ABOUT THE FOLLOWING CONTROL LOOP, PROBABLY BETTER WAYS TO DO IT, STILL TESTING
				if (initialSync == 1)
				{	// got initial sync, decrease / increase playback speed if age != 0

					// TODO:
					// BADAIX original code uses 3 buffers and median on them to do sample rate adjustment calculations
					// so for sure there is a better implementation than the following
					short_buffer[short_buffer_cnt++] = age;
					if (short_buffer_cnt >= MAX_SHORT_BUFFER_COUNT ) {
						short_buffer_full = 1;

						short_buffer_cnt = 0;
					}

					int l;
					if (short_buffer_full) {
						l = MAX_SHORT_BUFFER_COUNT;
					}
					else {
						l = short_buffer_cnt;
					}

					memcpy( short_buffer_median, short_buffer, sizeof(short_buffer) );
					quick_sort_int64(short_buffer_median, 0, l);
					avg = short_buffer_median[l/2];

					int dir = 0;
					age_expect = 4 * chunkDuration_us;

					// void rtc_clk_apll_enable(bool enable, uint32_t sdm0, uint32_t sdm1, uint32_t sdm2, uint32_t o_div);
					// apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2)
					// xtal == 40MHz on lyrat v4.3
					// I2S bit_clock = rate * (number of channels) * bits_per_sample
					if (avg < -(age_expect + 1000)) {
//					if (avg < -100) {
//						sdm0 = apll_predefine_48k_corr[2][2];
//						sdm1= apll_predefine_48k_corr[2][3];
//						sdm2 = apll_predefine_48k_corr[2][4];
//						o_div = apll_predefine_48k_corr[2][5];

						dir = -1;
					}
					else if (avg > -(age_expect - 1000)) {
//					else if (avg > 100) {
//						sdm0 = apll_predefine_48k_corr[3][2];
//						sdm1= apll_predefine_48k_corr[3][3];
//						sdm2 = apll_predefine_48k_corr[3][4];
//						o_div = apll_predefine_48k_corr[3][5];

						dir = 1;
					}
					else if ((avg <= -(age_expect - 1000)) && (avg >= -(age_expect + 1000))) {
//					else if ((avg >= -100) && (avg <= 100)) {
//					else {
						// reset to normal playback speed
//						sdm0 = apll_predefine[5][2];
//						sdm1= apll_predefine[5][3];
//						sdm2 = apll_predefine[5][4];
//						o_div = apll_predefine[5][5];
						dir = 0;
					}

//					adjust_apll(dir);

					ESP_LOGI(TAG, "%lldus", avg);

					//rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);
				}

				p_payload = chnk->payload;
				size = chnk->size;

				// write data to I2S
				bytesWritten = 0;
				bytesWritten += raw_stream_write(raw_stream_writer_to_i2s, p_payload, size);
				if (bytesWritten < size) {
					ESP_LOGE(TAG, "i2s raw writer ring buf full");
				}

//				ESP_LOGI(TAG, "\ns %d a %d s %d a %d\ns %d a %d s %d a %d",
//									rb_get_size(audio_element_get_input_ringbuf(raw_stream_writer_to_i2s)), rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_writer_to_i2s)),
//									rb_get_size(audio_element_get_output_ringbuf(raw_stream_writer_to_i2s)), rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_i2s)),
//									rb_get_size(audio_element_get_input_ringbuf(i2s_stream_writer)), rb_bytes_filled(audio_element_get_input_ringbuf(i2s_stream_writer)),
//									rb_get_size(audio_element_get_output_ringbuf(i2s_stream_writer)), rb_bytes_filled(audio_element_get_output_ringbuf(i2s_stream_writer)));

				//ESP_LOGI(TAG, "%d", rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_i2s)));

//				ESP_LOGI(TAG, "bytesWritten %d", bytesWritten);
				struct timeval t;
				get_diff_to_server(&t);
				//ESP_LOGI(TAG, "%lldus, %d, %d, %d %lldus", age, sdm0, sdm1, sdm2, ((int64_t)t.tv_sec * 1000000LL + (int64_t)t.tv_usec));
				ESP_LOGI(TAG, "%lldus, %lldus, %d, %d, %d %lldus", age, age + age_expect, sdm0, sdm1, sdm2, ((int64_t)t.tv_sec * 1000000LL + (int64_t)t.tv_usec));
			}
			else {
				ESP_LOGW(TAG, "couldn't get server now");

				vTaskDelay( pdMS_TO_TICKS(10) );

				continue;
			}

			free(chnk->payload);
			free(chnk);
			chnk = NULL;
		}
		else {
			ESP_LOGE(TAG, "Couldn't get PCM chunk, recv: messages waiting %d, %d", uxQueueMessagesWaiting(pcmChunkQueueHandle), uxQueueMessagesWaiting(timestampQueueHandle));

//			ESP_LOGI(TAG, "\n1: s %d f %d s %d f %d\n2: s %d f %d s %d f %d\n3: s %d f %d s %d f %d",
//					rb_get_size(audio_element_get_input_ringbuf(raw_stream_writer_to_decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_writer_to_decoder)),
//					rb_get_size(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)),
//					rb_get_size(audio_element_get_input_ringbuf(decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(decoder)),
//					rb_get_size(audio_element_get_output_ringbuf(decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(decoder)),
//					rb_get_size(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)),
//					rb_get_size(audio_element_get_output_ringbuf(raw_stream_reader_from_decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_reader_from_decoder)));

//			// probably the stream stopped, so we need to reset decoder's buffers here
//			audio_element_reset_output_ringbuf(raw_stream_writer_to_decoder);
//			audio_element_reset_input_ringbuf(raw_stream_writer_to_decoder);
//
//			audio_element_reset_output_ringbuf(decoder);
//			audio_element_reset_input_ringbuf(decoder);
//
//			audio_element_reset_output_ringbuf(raw_stream_writer_to_i2s);
//			audio_element_reset_input_ringbuf(raw_stream_writer_to_i2s);
//
//			audio_element_reset_output_ringbuf(i2s_stream_writer);
//			audio_element_reset_input_ringbuf(i2s_stream_writer);
//
//			wirechnkCnt = 0;
//			pcmchnkCnt = 0;

			initialSync = 0;
			alarmValSub = 0;

			vTaskDelay( pdMS_TO_TICKS(100) );
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
    int sockfd;
    char base_message_serialized[BASE_MESSAGE_SIZE];
    char *hello_message_serialized;
    int result, size, id_counter;
    struct timeval now, tv1, tv2, tv3;
    time_message_t time_message;
    struct timeval tmpDiffToServer;
    const int64_t outputBufferDacTime_us = DAC_OUT_BUFFER_TIME_US;	// in ms
    snapcast_sync_task_cfg_t snapcastTaskCfg;
	struct timeval lastTimeSync = { 0, 0 };

	wire_chunk_message_t wire_chunk_message_last = {{0,0}, 0, NULL};
	esp_timer_handle_t timeSyncMessageTimer;
	const esp_timer_create_args_t tSyncArgs = 	{
													.callback = &time_sync_msg_cb,
													.name = "tSyncMsg"
												};

    // create semaphore for time diff buffer to server
    diffBufSemaphoreHandle = xSemaphoreCreateMutex();

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

    while(1) {
    	memset((void *)diffBuf, 0, sizeof(diffBuf));
    	diffBufCnt = 0;

        /* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                            false, true, portMAX_DELAY);
        ESP_LOGI(TAG, "Connected to AP");

        // Find snapcast server
        // Connect to first snapcast server found
        ESP_LOGI(TAG, "Enable mdns") ;
        mdns_init();
        mdns_result_t * r = NULL;
        esp_err_t err = 0;
        while ( !r || err )
        {  ESP_LOGI(TAG, "Lookup snapcast service on network");
           esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20,  &r);
           if(err){
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
											"0.0.2",
											"libsnapcast",
											"esp32",
											"xtensa",
											1,
											mac_address,
											2,
										 };

        hello_message_serialized = hello_message_serialize(&hello_message, (size_t*) &(base_message.size));
        if (!hello_message_serialized) {
            ESP_LOGI(TAG, "Failed to serialize hello message\r\b");
            return;
        }

        result = base_message_serialize(
            &base_message,
            base_message_serialized,
            BASE_MESSAGE_SIZE
        );
        if (result) {
            ESP_LOGI(TAG, "Failed to serialize base message\r\n");
            return;
        }

        write(sockfd, base_message_serialized, BASE_MESSAGE_SIZE);
        write(sockfd, hello_message_serialized, base_message.size);
        free(hello_message_serialized);

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
            		ESP_LOGI(TAG, "%s", strerror(errno));
            	}

            	break;	// stop for(;;) will try to reconnect then
            }

            if (result > 0) {
				result = gettimeofday(&now, NULL);
				//ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec, now.tv_usec);
				if (result) {
					ESP_LOGI(TAG, "Failed to gettimeofday");
					continue;
				}

				result = base_message_deserialize(&base_message, buff, size);
				if (result) {
					ESP_LOGI(TAG, "Failed to read base message: %d", result);
					continue;
				}

				base_message.received.usec = now.tv_usec;
	//            ESP_LOGI(TAG,"%d %d : %d %d : %d %d",base_message.size, base_message.refersTo,
	//												 base_message.sent.sec,base_message.sent.usec,
	//												 base_message.received.sec,base_message.received.usec);

				//ESP_LOGI(TAG,"Free Heap: %d", xPortGetFreeHeapSize());

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
						ESP_LOGI(TAG, "Failed to read from server: %d", result);

						break;
					}

					size += result;
				}

				if (result < 0) {
					if (errno != 0 ) {
						ESP_LOGI(TAG, "%s", strerror(errno));
					}

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

						if (strcmp(codec_header_message.codec,"flac") == 0) {
							// TODO: maybe restart the whole thing if a new codec header is received while stream session is ongoing

							raw_stream_write(raw_stream_writer_to_decoder, codec_header_message.payload, size);

//							printf("\r\n");
//							for (int i=0; i<size; i++) {
//								printf("%c", codec_header_message.payload[i]);
//							}
//							printf("\r\n");
//							printf("\r\n");
//							for (int i=0; i<size; i++) {
//								printf("%02x", codec_header_message.payload[i]);
//							}
//							printf("\r\n");
						}
						else if (strcmp(codec_header_message.codec,"opus") == 0) {
							// TODO: NOT Implemented yet!
							uint32_t rate;
							memcpy(&rate, start+4,sizeof(rate));
							uint16_t bits;
							memcpy(&bits, start+8,sizeof(bits));
							uint16_t channels;
							memcpy(&channels, start+10,sizeof(channels));

							ESP_LOGI(TAG, "Codec : %s not implemented yet", codec_header_message.codec);

							return;

							//ESP_LOGI(TAG, "Codec setting %d:%d:%d", rate,bits,channels);
						}
						else {
						   ESP_LOGI(TAG, "Codec : %s not supported", codec_header_message.codec);
						   ESP_LOGI(TAG, "Change encoder codec to flac in /etc/snapserver.conf on server");
						   return;
						}

						ESP_LOGI(TAG, "Codec : %s", codec_header_message.codec);

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

						ESP_LOGI(TAG, "syncing clock to server");
						tv1.tv_sec = base_message.sent.sec;
						tv1.tv_usec = base_message.sent.usec;
						settimeofday(&tv1, NULL);

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

						//ESP_LOGI(TAG, "wire chnk with size: %d, timestamp %d.%d", wire_chunk_message.size, wire_chunk_message.timestamp.sec, wire_chunk_message.timestamp.usec);

//						struct timeval tv_d1, tv_d2, tv_d3;
//						tv_d1.tv_sec = wire_chunk_message.timestamp.sec;
//						tv_d1.tv_usec = wire_chunk_message.timestamp.usec;
//						tv_d2.tv_sec = wire_chunk_message_last.timestamp.sec;
//						tv_d2.tv_usec = wire_chunk_message_last.timestamp.usec;
//						timersub(&tv_d1, &tv_d2, &tv_d3);
//						ESP_LOGI(TAG, "chunk duration %ld.%06ld", tv_d3.tv_sec, tv_d3.tv_usec);

						wire_chunk_message_last.timestamp = wire_chunk_message.timestamp;

						// store chunk's timestamp, decoder callback will need it later
						tv_t timestamp;
						timestamp = wire_chunk_message.timestamp;

						#if TEST_WIRECHUNK_TO_PCM_PART == 1
							if (wirechnkCnt == 0) {
								ESP_LOGI(TAG, "set decoder timeout");

								audio_element_set_input_timeout(decoder, pdMS_TO_TICKS(100));
								audio_element_set_output_timeout(decoder, pdMS_TO_TICKS(100));
							}
						#endif

						wirechnkCnt++;
//						ESP_LOGI(TAG, "got wire chunk %d, cnt %lld",(int)wire_chunk_message.size, wirechnkCnt );
//						ESP_LOGI(TAG, "got wire chunk cnt %lld", wirechnkCnt );

//						wirechnkCnt++;
//						ESP_LOGI(TAG, "wirechnkCnt: %lld", wirechnkCnt);

						int bytesWritten;
						bytesWritten = raw_stream_write(raw_stream_writer_to_decoder, wire_chunk_message.payload, (int)wire_chunk_message.size);
						if (bytesWritten < 0) {
							ESP_LOGE(TAG, "wirechnk decode ring buf timeout");
						}
						else if (bytesWritten < (int)wire_chunk_message.size) {
							ESP_LOGE(TAG, "wirechnk decode ring buf full");
						}
						else {
//						#if TEST_WIRECHUNK_TO_PCM_PART == 0
							if (xQueueSendToBack( timestampQueueHandle, &timestamp, pdMS_TO_TICKS(3000)) == pdTRUE) {
								//ESP_LOGW(TAG, "timestamps waiting %d", uxQueueMessagesWaiting(timestampQueueHandle));

								//ESP_LOGI(TAG, "1: s %d f %d chunks %ld",rb_get_size(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)),
								//										rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)),
								//										rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)) / (WIRE_CHUNK_DURATION_MS * CHANNELS * BIT_WIDTH * SAMPLE_RATE / 1000));
							}
							else {
								ESP_LOGW(TAG, "timestamp queue full, messages waiting %d, dropping data ...", uxQueueMessagesWaiting(timestampQueueHandle));
							}
						}
//						#endif

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

//						#if TEST_WIRECHUNK_TO_PCM_PART == 0
							if (syncTaskHandle == NULL) {
								ESP_LOGI(TAG, "Start snapcast_sync_task");

								snapcastTaskCfg.outputBufferDacTime_us = outputBufferDacTime_us;
								snapcastTaskCfg.buffer_us = (int64_t)buffer_ms * 1000LL;
								xTaskCreatePinnedToCore(snapcast_sync_task, "snapcast_sync_task", 8 * 1024, &snapcastTaskCfg, SYNC_TASK_PRIORITY, &syncTaskHandle, SYNC_TASK_CORE_ID);
							}
//						#endif

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

						//ESP_LOGI(TAG, "Current latency: %ld.%06ld", tmpDiffToServer.tv_sec, tmpDiffToServer.tv_usec);

						// following code is storing / initializing / resetting diff to server algorithm
						// we collect a number of latencies. Based on these we can get the median of server now
						{
							struct timeval diff;

							// clear diffBuffer if last update is older than a minute
							timersub(&now, &lastTimeSync, &diff);

							if ( diff.tv_sec > 60 ) {
								ESP_LOGW(TAG, "Last time sync older than a minute. Clearing time buffer");

								reset_diff_buffer();
							}

							add_to_diff_buffer(tmpDiffToServer);

							// store current time
							lastTimeSync.tv_sec = now.tv_sec;
							lastTimeSync.tv_usec = now.tv_usec;

							// we don't care if it was already taken, just make sure it is taken at this point
							xSemaphoreTake( timeSyncSemaphoreHandle, 0 );

							if (diff_buffer_full() > 0) {
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

					//ESP_LOGI(TAG, "sent time sync message %ld.%06ld", now.tv_sec, now.tv_usec);
				}
            }
        }

        esp_timer_stop(timeSyncMessageTimer);
        esp_timer_delete(timeSyncMessageTimer);
        xSemaphoreGive(timeSyncSemaphoreHandle);

        if (syncTaskHandle != NULL) {
			vTaskDelete(syncTaskHandle);
			syncTaskHandle = NULL;

			xQueueReset(timestampQueueHandle);
			//xQueueReset(pcmChunkQueueHandle);

			wirechnkCnt = 0;
			pcmchnkCnt = 0;
		}

        ESP_LOGI(TAG, "... closing socket\r\n");
        close(sockfd);
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
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
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



/**
 *
 */
static void wirechunk_to_pcm_timestamp_task(void *pvParameters) {
	wire_chunk_message_t *pcm_chunk_message;
	tv_t timestamp;
	const int size_expect = (WIRE_CHUNK_DURATION_MS * CHANNELS * BYTE_PER_SAMPLE * SAMPLE_RATE) / 1000;
	int itemsRead = 0;
	ringbuf_handle_t raw_stream_reader_from_decoder_rb_handle = audio_element_get_input_ringbuf(raw_stream_reader_from_decoder);
	ringbuf_handle_t decoder_rb_handle = audio_element_get_input_ringbuf(decoder);
	ringbuf_handle_t raw_stream_writer_to_decoder_rb_handle = audio_element_get_output_ringbuf(raw_stream_writer_to_decoder);

	while (1) {
		if (xQueueReceive( timestampQueueHandle, &timestamp, pdMS_TO_TICKS(10000) ) == pdPASS) {
//			ESP_LOGI(TAG, "pcm2ts out %d", rb_bytes_filled(raw_stream_writer_to_decoder_rb_handle));
//			ESP_LOGI(TAG, "ts msg: %d", uxQueueMessagesWaiting(timestampQueueHandle));

			//pcm_chunk_message = (wire_chunk_message_t *)malloc(sizeof(wire_chunk_message_t));
			pcm_chunk_message = (wire_chunk_message_t *)heap_caps_malloc(sizeof(wire_chunk_message_t), MALLOC_CAP_SPIRAM);
			if (pcm_chunk_message == NULL) {
				ESP_LOGE(TAG, "wirechunk_to_pcm_timestamp_task: Failed to allocate memory for pcm chunk message");

				continue;
			}

			//pcm_chunk_message->payload = (char *)malloc(sizeof(char) * size_expect);
			pcm_chunk_message->payload = (char *)heap_caps_malloc(sizeof(char) * size_expect, MALLOC_CAP_SPIRAM);
			if (pcm_chunk_message->payload == NULL) {
				ESP_LOGE(TAG, "wirechunk_to_pcm_timestamp_task: Failed to allocate memory for pcm chunk payload");

				free(pcm_chunk_message);

				continue;
			}

			pcm_chunk_message->size = size_expect;
			pcm_chunk_message->timestamp = timestamp;

//			ESP_LOGI(TAG, "\n1: s %d f %d s %d f %d\n2: s %d f %d s %d f %d\n3: s %d f %d s %d f %d",
//									rb_get_size(audio_element_get_input_ringbuf(raw_stream_writer_to_decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_writer_to_decoder)),
//									rb_get_size(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_writer_to_decoder)),
//									rb_get_size(audio_element_get_input_ringbuf(decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(decoder)),
//									rb_get_size(audio_element_get_output_ringbuf(decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(decoder)),
//									rb_get_size(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)), rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)),
//									rb_get_size(audio_element_get_output_ringbuf(raw_stream_reader_from_decoder)), rb_bytes_filled(audio_element_get_output_ringbuf(raw_stream_reader_from_decoder)));

//			ESP_LOGI(TAG, "raw_stream_reader_from_decoder: s %d f %d chunks %d", rb_get_size(audio_element_get_out_ringbuf(raw_stream_reader_from_decoder)),
//												 rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)),
//												 rb_bytes_filled(audio_element_get_input_ringbuf(raw_stream_reader_from_decoder)) / size_expect);
			itemsRead = raw_stream_read(raw_stream_reader_from_decoder, pcm_chunk_message->payload, size_expect);
			if (itemsRead < size_expect) {
//				// probably the stream stopped, so we need to reset decoder's buffers here
//				audio_element_reset_output_ringbuf(raw_stream_writer_to_decoder);
//				audio_element_reset_input_ringbuf(raw_stream_writer_to_decoder);
//
//				audio_element_reset_output_ringbuf(decoder);
//				audio_element_reset_input_ringbuf(decoder);
//
//				audio_element_reset_output_ringbuf(raw_stream_writer_to_i2s);
//				audio_element_reset_input_ringbuf(raw_stream_writer_to_i2s);
//
//				audio_element_reset_output_ringbuf(i2s_stream_writer);
//				audio_element_reset_input_ringbuf(i2s_stream_writer);
//
//				wirechnkCnt = 0;
//				pcmchnkCnt = 0;
//
//				xQueueReset(timestampQueueHandle);

				ESP_LOGE(TAG, "wirechunk_to_pcm_timestamp_task: read %d, expected %d", itemsRead, size_expect);
			}
			else {
				if (xQueueSendToBack( pcmChunkQueueHandle, &pcm_chunk_message, pdMS_TO_TICKS(1000)) != pdTRUE) {
					ESP_LOGW(TAG, "wirechunk_to_pcm_timestamp_task: send: pcmChunkQueue full, messages waiting %d", uxQueueMessagesWaiting(pcmChunkQueueHandle));
				}
			}
		}
		else {
//			// probably the stream stopped, so we need to reset decoder's buffers here
//			audio_element_reset_output_ringbuf(raw_stream_writer_to_decoder);
//			audio_element_reset_input_ringbuf(raw_stream_writer_to_decoder);
//
//			audio_element_reset_output_ringbuf(decoder);
//			audio_element_reset_input_ringbuf(decoder);
//
//			audio_element_reset_output_ringbuf(raw_stream_writer_to_i2s);
//			audio_element_reset_input_ringbuf(raw_stream_writer_to_i2s);
//
//			audio_element_reset_output_ringbuf(i2s_stream_writer);
//			audio_element_reset_input_ringbuf(i2s_stream_writer);
//
//			wirechnkCnt = 0;
//			pcmchnkCnt = 0;
//
//			xQueueReset(timestampQueueHandle);

			ESP_LOGW(TAG, "wirechunk_to_pcm_timestamp_task: failed to get timestamp for it, messages waiting %d, %d", uxQueueMessagesWaiting(timestampQueueHandle), uxQueueMessagesWaiting(pcmChunkQueueHandle));

			vTaskDelay(pdMS_TO_TICKS(100));
		}
	}

	// should never get here ...
	return;
}

uint64_t flacCnt = 0;
uint64_t receivedByteCnt = 0;

int flac_decoder_write_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx) {
	//ringbuf_handle_t *rb = (ringbuf_handle_t *)ctx;
	wire_chunk_message_t *pcm_chunk_message;
	tv_t timestamp;

	flacCnt++;
	receivedByteCnt += len;

//	ESP_LOGI(TAG, "flac_decoder_write_cb: len %d, cnt %d, received total: %lld", len, flacCnt, receivedByteCnt);
//	ESP_LOGI(TAG, "flac_decoder_write_cb: flac cnt %lld",flacCnt);

	//rb_write(*rb, buf, len, wait_time);


	if (xQueueReceive( timestampQueueHandle, &timestamp, wait_time ) == pdPASS) {
		pcm_chunk_message = (wire_chunk_message_t *)heap_caps_malloc(sizeof(wire_chunk_message_t), MALLOC_CAP_SPIRAM);
		if (pcm_chunk_message == NULL) {
			ESP_LOGE(TAG, "wirechunk_to_pcm_timestamp_task: Failed to allocate memory for pcm chunk message");
		}
		else {
			pcm_chunk_message->payload = (char *)heap_caps_malloc(sizeof(char) * len, MALLOC_CAP_SPIRAM);
			if (pcm_chunk_message->payload == NULL) {
				ESP_LOGE(TAG, "wirechunk_to_pcm_timestamp_task: Failed to allocate memory for pcm chunk payload");

				free(pcm_chunk_message);
			}
			else {
				pcm_chunk_message->size = len;
				pcm_chunk_message->timestamp = timestamp;
				memcpy(pcm_chunk_message->payload ,buf, len);

				if (xQueueSendToBack( pcmChunkQueueHandle, &pcm_chunk_message, pdMS_TO_TICKS(1000)) != pdTRUE) {
					ESP_LOGW(TAG, "wirechunk_to_pcm_timestamp_task: send: pcmChunkQueue full, messages waiting %d", uxQueueMessagesWaiting(pcmChunkQueueHandle));
				}
			}
		}
	}

	if (flacCnt == wirechnkCnt) {
		audio_element_set_input_timeout(decoder, portMAX_DELAY);
		audio_element_set_output_timeout(decoder, portMAX_DELAY);

		wirechnkCnt = 0;
		flacCnt = 0;
		receivedByteCnt = 0;

		ESP_LOGI(TAG, "flac_decoder_write_cb: clear decoder timeout");
	}

	return len;
}

/**
 *
 */
void app_main(void) {
    esp_err_t ret;
    uint8_t base_mac[6];


	ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    esp_timer_init();

	// Get MAC address for WiFi station
	esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
	sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
	ESP_LOGI(TAG, "MAC Adress is: %s", mac_address);

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Start codec chip");
    board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "Create audio pipeline for decoding");
    audio_pipeline_cfg_t flac_dec_pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    flacDecodePipeline = audio_pipeline_init(&flac_dec_pipeline_cfg);
    //flac_dec_pipeline_cfg.rb_size = 16 * 4096;	// TODO: how much is really needed?
    AUDIO_NULL_CHECK(TAG, flacDecodePipeline, return);

    ESP_LOGI(TAG, "Create raw stream to write data from snapserver to decoder");
    raw_stream_cfg_t raw_1_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_1_cfg.type = AUDIO_STREAM_WRITER;
//    raw_1_cfg.out_rb_size = 16 * 1024;	// TODO: how much is really needed?
    raw_stream_writer_to_decoder = raw_stream_init(&raw_1_cfg);
    audio_element_set_output_timeout(raw_stream_writer_to_decoder, pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Create flac decoder to decode flac file and set custom write callback");
	flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
	flac_cfg.task_prio = FLAC_DECODER_PRIORITY;
	flac_cfg.task_core = FLAC_DECODER_CORE_ID;
//	flac_cfg.out_rb_size = 2 * 16 * 1024;	// TODO: how much is really needed?
	decoder = flac_decoder_init(&flac_cfg);

    ESP_LOGI(TAG, "Create raw stream to read data from decoder");
    raw_stream_cfg_t raw_3_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_3_cfg.type = AUDIO_STREAM_READER;
    raw_stream_reader_from_decoder = raw_stream_init(&raw_3_cfg);
    audio_element_set_input_timeout(raw_stream_reader_from_decoder, pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Register all elements to audio pipeline");
    audio_pipeline_register(flacDecodePipeline, raw_stream_writer_to_decoder, "raw_1");
    audio_pipeline_register(flacDecodePipeline, decoder, "decoder");
#if TEST_WIRECHUNK_TO_PCM_PART == 0
    audio_pipeline_register(flacDecodePipeline, raw_stream_reader_from_decoder, "raw_3");
    ESP_LOGI(TAG, "Link it together [snapclient]-->raw_1-->decoder --> raw_3");
	const char *link_tag[] = {"raw_1", "decoder", "raw_3"};
    audio_pipeline_link(flacDecodePipeline, &link_tag[0], 3);
#else

    // TESTING, with write callback, so ripped up pipeline here
//    ringbuf_handle_t test;
//    test = rb_create(1024, 8);
//    audio_element_set_input_ringbuf(raw_stream_reader_from_decoder, test);
//	audio_element_set_write_cb(decoder, flac_decoder_write_cb, &test);
    audio_element_set_write_cb(decoder, flac_decoder_write_cb, NULL);
//    audio_element_set_write_cb(decoder, flac_decoder_write_cb, NULL);

    ESP_LOGI(TAG, "Link it together [snapclient]-->raw_1-->decoder");
	const char *link_tag[] = {"raw_1", "decoder"};
    audio_pipeline_link(flacDecodePipeline, &link_tag[0], 2);
#endif






//	ESP_LOGI(TAG, "Link it together [snapclient]-->raw_1-->decoder");
//	const char *link_tag[] = {"raw_1", "decoder"};
//	audio_pipeline_link(flacDecodePipeline, &link_tag[0], 2);




    ESP_LOGI(TAG, "Create audio pipeline for playback");
	audio_pipeline_cfg_t playback_pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	playbackPipeline = audio_pipeline_init(&playback_pipeline_cfg);
	AUDIO_NULL_CHECK(TAG, playbackPipeline, return);

    ESP_LOGI(TAG, "Create raw stream to write data from decoder to i2s");
    raw_stream_cfg_t raw_2_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_2_cfg.type = AUDIO_STREAM_WRITER;
    raw_2_cfg.out_rb_size = 1 * 4608;// TODO: how much is really needed? effect on age calculation? Probably needs dynamically changeable to chunk size???
    raw_stream_writer_to_i2s = raw_stream_init(&raw_2_cfg);
    audio_element_set_output_timeout(raw_stream_writer_to_i2s, pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Create i2s stream to write data to codec chip");
	i2s_cfg.i2s_config.sample_rate = 48000;
	i2s_cfg.i2s_config.dma_buf_count = 2;		// 2 * 576 = 24ms = 4608 Byte @ 48kHz
	i2s_cfg.i2s_config.dma_buf_len = 576;		// this is number of frames NOT bytes	!!!	// TODO: how much is really needed? effect on age calculation?
	i2s_cfg.task_core = I2S_TASK_CORE_ID;
	i2s_cfg.task_prio = I2S_TASK_PRIORITY;
	i2s_stream_writer = i2s_stream_init(&i2s_cfg);

	audio_pipeline_register(playbackPipeline, raw_stream_writer_to_i2s, "raw_2");
    audio_pipeline_register(playbackPipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "Link it together [sync task]-->raw_2-->i2s_stream-->[codec_chip]");
	const char *link_tag_2[2] = {"raw_2", "i2s"};
    audio_pipeline_link(playbackPipeline, &link_tag_2[0], 2);

    ESP_LOGI(TAG, "Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "Listening event from all elements of pipelines");
    audio_pipeline_set_listener(flacDecodePipeline, evt);
    audio_pipeline_set_listener(playbackPipeline, evt);

    ESP_LOGI(TAG, "Start audio_pipelines");
    audio_pipeline_run(flacDecodePipeline);
    audio_pipeline_run(playbackPipeline);

//    i2s_stop(i2s_cfg.i2s_port);
//    i2s_zero_dma_buffer(i2s_cfg.i2s_port);

    ESP_LOGI(TAG, "Listen for all pipeline events");


#if CONFIG_USE_SNTP == 1
    // syncing to sntp
	vTaskDelay(5000/portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "Syncing to sntp");
	set_time_from_sntp();
#else
	{
		// don't use sntp, if server and client are too different, we get overflowing timevals
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

	xTaskCreatePinnedToCore(http_get_task, "http_get_task", 2*4096, NULL, HTTP_TASK_PRIORITY, NULL, HTTP_TASK_CORE_ID);

//#if TEST_WIRECHUNK_TO_PCM_PART == 0
//	xTaskCreatePinnedToCore(wirechunk_to_pcm_timestamp_task, "timestamp_task", 2*4096, NULL, TIMESTAMP_TASK_PRIORITY, NULL, TIMESTAMP_TASK_CORE_ID);
//#endif

#if COLLECT_RUNTIME_STATS == 1
    xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);
#endif

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret;

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

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);

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

//        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) raw_stream_writer_to_i2s
//			&& msg.cmd == AEL_MSG_CMD_REPORT_STATUS && ((int)msg.data == AEL_STATUS_STATE_PAUSED || (int)msg.data == AEL_STATUS_STATE_RUNNING))
//		{
//        	if ((int)msg.data == AEL_STATUS_STATE_PAUSED) {
//        		ESP_LOGW(TAG, "raw_stream_writer_to_i2s pause event received");
//        	}
//        	else if ((int)msg.data == AEL_STATUS_STATE_RUNNING) {
//        		ESP_LOGW(TAG, "raw_stream_writer_to_i2s run event received");
//        	}
//        	else {
//        		ESP_LOGW(TAG, "raw_stream_writer_to_i2s unknown event received: %d", (int)msg.data);
//        	}
//		}

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)))
        {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "Stop audio_pipeline");
    audio_pipeline_stop(flacDecodePipeline);
    audio_pipeline_wait_for_stop(flacDecodePipeline);
    audio_pipeline_terminate(flacDecodePipeline);
    audio_pipeline_stop(playbackPipeline);
    audio_pipeline_wait_for_stop(playbackPipeline);
    audio_pipeline_terminate(playbackPipeline);

    audio_pipeline_unregister(flacDecodePipeline, raw_stream_writer_to_decoder);
    audio_pipeline_unregister(flacDecodePipeline, decoder);
    audio_pipeline_unregister(flacDecodePipeline, raw_stream_reader_from_decoder);
    audio_pipeline_unregister(playbackPipeline, raw_stream_writer_to_i2s);
    audio_pipeline_unregister(playbackPipeline, i2s_stream_writer);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(flacDecodePipeline);
    audio_pipeline_remove_listener(playbackPipeline);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(flacDecodePipeline);
    audio_element_deinit(raw_stream_writer_to_decoder);
    audio_element_deinit(decoder);
    audio_element_deinit(raw_stream_reader_from_decoder);

    audio_pipeline_deinit(playbackPipeline);
    audio_element_deinit(raw_stream_writer_to_i2s);
    audio_element_deinit(i2s_stream_writer);

    // TODO: clean up all created tasks and delete them
}



