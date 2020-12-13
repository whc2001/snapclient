#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "mp3_decoder.h"
#include "flac_decoder.h"

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

//#include "driver/timer.h"

#include <sys/time.h>

#define CONFIG_USE_SNTP		0

#define DAC_OUT_BUFFER_TIME_US		3 * 1526LL		//TODO: not sure about this... // @48kHz, 16bit samples, 2 channels and a DMA buffer length of 300 Byte and 3 buffers. 300 Byte / (48000 * 2 Byte * 2 channels)

static const char *TAG = "SNAPCLIENT";

static int sntp_synced = 0;

char *codecString = NULL;

// configMAX_PRIORITIES - 1

// TODO: what are the best values here?
#define SYNC_TASK_PRIORITY		6
#define SYNC_TASK_CORE_ID  		1

#define HTTP_TASK_PRIORITY		1
#define HTTP_TASK_CORE_ID  		0

#define I2S_TASK_PRIORITY  		0
#define I2S_TASK_CORE_ID  		1

#define AGE_THRESHOLD	50LL	// in µs


#define WIRE_CHNK_QUEUE_LENGTH 50	// TODO: one chunk is hardcoded to 20ms, change it to be dynamically adjustable. 1s buffer = 50
static StaticQueue_t wireChunkQueue;
uint8_t wireChunkQueueStorageArea[ WIRE_CHNK_QUEUE_LENGTH * sizeof(wire_chunk_message_t *) ];

typedef struct snapcast_sync_task_cfg_s {
	QueueHandle_t *sync_queue_handle;
	audio_element_handle_t *p_raw_stream_reader;
	int64_t outputBufferDacTime_us;
	int64_t buffer_us;
} snapcast_sync_task_cfg_t;

SemaphoreHandle_t diffBufSemaphoreHandle = NULL;

static struct timeval diffToServer = {0, 0};	// median diff to server in µs
static struct timeval diffBuf[50] = {0};	// collected diff's to server
//static struct timeval *medianArray = NULL;	// temp median calculation data is stored at this location
static struct timeval medianArray[50] = {0};	// temp median calculation data is stored at this location
static int indexOldest = 0;

xQueueHandle i2s_queue;
uint32_t buffer_ms = 400;
uint8_t  muteCH[4] = {0};
audio_board_handle_t board_handle;
/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/

/* Constants that aren't configurable in menuconfig */
#define HOST "192.168.1.6"
#define PORT 1704
#define BUFF_LEN 4000
unsigned int addr;
uint32_t port = 0;
/* Logging tag */
//static const char *TAG = "SNAPCAST";

/* FreeRTOS event group to signal when we are connected & ready to make a request */
//static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

static char buff[BUFF_LEN];
//static audio_element_handle_t snapcast_stream;
static char mac_address[18];


static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
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
			.ssid = CONFIG_ESP_WIFI_SSID,
			.password = CONFIG_ESP_WIFI_PASSWORD,
			.bssid_set = false
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

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
void find_mdns_service(const char * service_name, const char * proto)
{
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
void QuickSort(struct timeval *a, int left, int right) {
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

		QuickSort( a, left, i - 1 );
		QuickSort( a, j + 1, right );
	}
}


/**
 *
 */
// TODO: find better implementation. Could be a performance issue?
int8_t get_median( const struct timeval *tDiff, size_t n, struct timeval *result ) {
	struct timeval median = {0 , 0};
    int i, j;

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
	QuickSort(medianArray, 0, n);

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
// TODO: find a better way to do this! Especially the initialization/reset part
int8_t set_diff_to_server( struct timeval *tDiff, size_t n, uint8_t *index ) {
	int8_t ret = -1;
	struct timeval diff, now;
	struct timeval tmpDiffToServer;
	static struct timeval lastTimeSync = { 0, 0 };
	static uint8_t bufferFull = false;
	size_t len;
	int i;

	if (diffBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "set_diff_to_server: mutex handle == NULL");

		return -1;
	}

	// get current time
	if (gettimeofday(&now, NULL)) {
		ESP_LOGE(TAG, "set_diff_to_server: Failed to get time of day");

		return -1;
	}

    // clear diffBuffer if last update is older than a minute
	diff.tv_sec = now.tv_sec - lastTimeSync.tv_sec;
	diff.tv_usec = now.tv_usec - lastTimeSync.tv_usec;
    if ( diff.tv_sec > 60 ) {
    	ESP_LOGW(TAG, "Last time sync older than a minute. Clearing time buffer");

    	for (i=0; i<n; i++) {
    		tDiff[i].tv_sec = 0;
    		tDiff[i].tv_usec = 0;
    	}
//		memset(diffBuf, 0, sizeof(diffBuf));

    	*index = 0;
    	bufferFull = false;

    	if (xSemaphoreTake( diffBufSemaphoreHandle, 1 ) == pdFALSE) {
			return -1;
		}

		memset(&diffToServer, 0, sizeof(diffToServer));

		xSemaphoreGive( diffBufSemaphoreHandle );

		// store current time for next run
		lastTimeSync.tv_sec = now.tv_sec;
		lastTimeSync.tv_usec = now.tv_usec;

    	return -1;
    }

    if ((*index >= (n - 1)) && (bufferFull == false)) {
    	bufferFull = true;
    }

    // store current time for next run
    lastTimeSync.tv_sec = now.tv_sec;
    lastTimeSync.tv_usec = now.tv_usec;

    if (bufferFull == true) {
    	len = n;
    }
    else {
    	len = *index;
    }

    //ESP_LOGI(TAG, "set_diff_to_server: index: %d, bufferFull %d", *index, bufferFull);

    ret = get_median(tDiff, len, &tmpDiffToServer);
    if (ret < 0) {
    	ESP_LOGW(TAG, "set_diff_to_server: get median failed");
    }

    if (xSemaphoreTake( diffBufSemaphoreHandle, 1 ) == pdFALSE) {
    	ESP_LOGW(TAG, "set_diff_to_server: can't take semaphore");

		return -1;
	}

    diffToServer =  tmpDiffToServer;

	xSemaphoreGive( diffBufSemaphoreHandle );

	return ret;
}

/**
 *
 */
int8_t get_diff_to_server( struct timeval *tDiff ) {
	static struct timeval lastDiff = { 0, 0 };

	if (diffBufSemaphoreHandle == NULL) {
		return -1;
	}

	if (xSemaphoreTake( diffBufSemaphoreHandle, 0 ) == pdFALSE) {
		*tDiff = lastDiff;

		ESP_LOGW(TAG, "get_diff_to_server: can't take semaphore. Old diff retreived");

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
		ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

		return -1;
	}

//	ESP_LOGI(TAG, "now: %lldus", (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec);
//	ESP_LOGI(TAG, "diff: %lldus", (int64_t)diff.tv_sec * 1000000LL + (int64_t)diff.tv_usec);

	timeradd(&now, &diff, sNow);

//	ESP_LOGI(TAG, "serverNow: %lldus", (int64_t)sNow->tv_sec * 1000000LL + (int64_t)sNow->tv_usec);

	return 0;
}


static void snapcast_sync_task(void *pvParameters) {
	snapcast_sync_task_cfg_t *taskCfg = (snapcast_sync_task_cfg_t *)pvParameters;
	wire_chunk_message_t *wireChnk = NULL;
	struct timeval serverNow = {0, 0};
	int64_t age;
	BaseType_t ret;
	uint8_t initial_sync = 0;

	ESP_LOGI(TAG, "started sync task");

//	ESP_LOGI(TAG, "tvAgeCompare1: %fms", (float)tvAgeCompare1.tv_sec * 1000.0 + (float)tvAgeCompare1.tv_usec / 1000.0);
//	ESP_LOGI(TAG, "tvAgeCompare2: %fms", (float)tvAgeCompare2.tv_sec * 1000.0 + (float)tvAgeCompare2.tv_usec / 1000.0);
//	ESP_LOGI(TAG, "tvAgeCompare3: %fms", (float)tvAgeCompare3.tv_sec * 1000.0 + (float)tvAgeCompare3.tv_usec / 1000.0);
//	ESP_LOGI(TAG, "tvAgeCompare4: %fms", (float)tvAgeCompare4.tv_sec * 1000.0 + (float)tvAgeCompare4.tv_usec / 1000.0);

	while(1) {
		if (wireChnk == NULL) {
			ret = xQueueReceive( *(taskCfg->sync_queue_handle), &wireChnk, portMAX_DELAY );
		}
		else {
			ret = pdPASS;
		}

		if( ret == pdPASS )	{
			if (initial_sync > 5) {
				if (server_now(&serverNow) >= 0) {
					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
							((int64_t)wireChnk->timestamp.sec * 1000000LL + (int64_t)wireChnk->timestamp.usec) -
							(int64_t)taskCfg->buffer_us +
							(int64_t)taskCfg->outputBufferDacTime_us;

					//ESP_LOGI(TAG, "before age: %lldus", age);
					if (age < -(int64_t)taskCfg->buffer_us / 10LL) {	// if age gets younger than 1/10 of buffer then do a hard resync
						initial_sync = 0;

						ESP_LOGI(TAG, "need resync, skipping chnk. age: %lldus", age);

						wire_chunk_message_free(wireChnk);
						free(wireChnk);
						wireChnk = NULL;

						continue;
					}
					else if (age < -1099LL) {
						vTaskDelay((TickType_t)(-age / 1000LL));	// TODO: find better way to do a exact delay to age, skipping probably goes away afterwards
					}
					else if (age > 0) {
						ESP_LOGI(TAG, "skipping chunk, age: %lldus", age);

						wire_chunk_message_free(wireChnk);
						free(wireChnk);
						wireChnk = NULL;

						continue;
					}
				}

//				// just for testing, print age
//				if (server_now(&serverNow) >= 0) {
//					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
//							((int64_t)wireChnk->timestamp.sec * 1000000LL + (int64_t)wireChnk->timestamp.usec) -
//							(int64_t)taskCfg->buffer_us +
//							(int64_t)taskCfg->outputBufferDacTime_us;
//
//					ESP_LOGI(TAG, "after age: %lldus", age);
//				}

				raw_stream_write(*(taskCfg->p_raw_stream_reader), wireChnk->payload, wireChnk->size);

				wire_chunk_message_free(wireChnk);
				free(wireChnk);
				wireChnk = NULL;
			}
			else if (server_now(&serverNow) >= 0)
			{
				// calc age in µs
				age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
						((int64_t)wireChnk->timestamp.sec * 1000000LL + (int64_t)wireChnk->timestamp.usec) -
						(int64_t)taskCfg->buffer_us +
						(int64_t)taskCfg->outputBufferDacTime_us;

//				ESP_LOGI(TAG, "age: %lldus, %lldus, %lldus, %lldus, %lldus", age,
//																			 (int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec,
//																			 (int64_t)wireChnk->timestamp.sec * 1000000LL + (int64_t)wireChnk->timestamp.usec,
//																			 (int64_t)taskCfg->buffer_us,
//																			 (int64_t)taskCfg->outputBufferDacTime_us);

				if (age < -(int64_t)taskCfg->buffer_us) {
					// fast forward
					ESP_LOGI(TAG, "fast forward, age: %lldus", age);

					wire_chunk_message_free(wireChnk);
					free(wireChnk);
					wireChnk = NULL;
				}
				else if ((age >= -AGE_THRESHOLD ) && ( age <= 0 )) {
					initial_sync++;

					wire_chunk_message_free(wireChnk);
					free(wireChnk);
					wireChnk = NULL;
				}
				else if (age > 0 ) {
					ESP_LOGI(TAG, "too old, age: %lldus", age);

					initial_sync = 0;

					wire_chunk_message_free(wireChnk);
					free(wireChnk);
					wireChnk = NULL;
				}
			}
		}
	}
}


/**
 *
 */
static void http_get_task(void *pvParameters) {
	audio_element_handle_t *p_raw_stream_reader = (audio_element_handle_t *)pvParameters;
    struct sockaddr_in servaddr;
    char *start;
    int sockfd;
    char base_message_serialized[BASE_MESSAGE_SIZE];
    char *hello_message_serialized;
    int result, size, id_counter;
    struct timeval now, tv1, tv2, tv3, last_time_sync;
    time_message_t time_message;
    struct timeval tmpDiffToServer;
    uint8_t diffBufCnt = 0;
    QueueHandle_t wireChunkQueueHandle;
    const int64_t outputBufferDacTime_us = DAC_OUT_BUFFER_TIME_US;	// in ms
    TaskHandle_t syncTask = NULL;
    snapcast_sync_task_cfg_t snapcastTaskCfg;
    
    // create semaphore for time diff buffer to server
    diffBufSemaphoreHandle = xSemaphoreCreateMutex();

    last_time_sync.tv_sec = 0;
    last_time_sync.tv_usec = 0;

    id_counter = 0;

    // create snapcast receive buffer
    wireChunkQueueHandle = xQueueCreateStatic( WIRE_CHNK_QUEUE_LENGTH,
											   sizeof(wire_chunk_message_t *),
											   wireChunkQueueStorageArea,

											   &wireChunkQueue
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
        ESP_LOGI(TAG,"Found %08x", r->addr->addr.u_addr.ip4.addr);

        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = r->addr->addr.u_addr.ip4.addr;         // inet_addr("192.168.1.158");
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
        base_message_t base_message = {
            SNAPCAST_MESSAGE_HELLO,
            0x0,
            0x0,
            { now.tv_sec, now.tv_usec },
            { 0x0, 0x0 },
            0x0,
        };

        hello_message_t hello_message = {
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
            	result = recv(sockfd, &(buff[size]), BASE_MESSAGE_SIZE - size, MSG_DONTWAIT);
                if (result < 0) {
                    break;

                }
                size += result;
            }

            // TODO: what about other errno possibilities?
            if (errno == ENOTCONN) {
            	ESP_LOGI(TAG, "%s", strerror(errno));

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
				while (size < base_message.size) {
					result = read(sockfd, &(buff[size]), base_message.size - size);
					if (result < 0) {
						ESP_LOGI(TAG, "Failed to read from server: %d", result);

						break;
					}

					size += result;
				}

				if (result < 0) {
					break;
				}

				switch (base_message.type) {
					case SNAPCAST_MESSAGE_CODEC_HEADER:
						result = codec_header_message_deserialize(&codec_header_message, start, size);
						if (result) {
							ESP_LOGI(TAG, "Failed to read codec header: %d", result);
							return;
						}

						ESP_LOGI(TAG, "Received codec header message");

						size = codec_header_message.size;
						start = codec_header_message.payload;

						if (strcmp(codec_header_message.codec,"flac") == 0) {
							// TODO: reset queue and free all memory (wirechunks + payload) if new codec header is received while stream session is ongoing
							//		 something like the following will probably do the trick in a loop until queue is empty. get current queue size and empty it.

							//wire_chunk_message_t *wire_chunk_message
							//wire_chunk_message_free(wire_chunk_message);
							//free(wire_chunk_message);
							//xQueueReset(wireChunkQueueHandle);	// reset wire chunk queue

							raw_stream_write(*p_raw_stream_reader, codec_header_message.payload, size);
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

						codec_header_message_free(&codec_header_message);
						received_header = true;

					break;

					case SNAPCAST_MESSAGE_WIRE_CHUNK:
					{
						if (!received_header) {
							continue;
						}

						wire_chunk_message_t *wire_chunk_message = (wire_chunk_message_t *)malloc(sizeof(wire_chunk_message_t));
						if (wire_chunk_message == NULL) {
							ESP_LOGI(TAG, "Failed to allocate memory for wire chunk");

							break;
						}

						result = wire_chunk_message_deserialize(wire_chunk_message, start, size);
						if (result) {
							ESP_LOGI(TAG, "Failed to read wire chunk: %d\r\n", result);

							wire_chunk_message_free(wire_chunk_message);
							free(wire_chunk_message);
							break;
						}

						//ESP_LOGI(TAG, "wire chnk with size: %d, timestamp %d.%d", wire_chunk_message->size, wire_chunk_message->timestamp.sec, wire_chunk_message->timestamp.usec);

						if( xQueueSendToBack( wireChunkQueueHandle, (void *)&wire_chunk_message, ( TickType_t ) portMAX_DELAY) != pdPASS )
						{
							ESP_LOGI(TAG, "Failed to post the message");

							wire_chunk_message_free(wire_chunk_message);
							free(wire_chunk_message);
						}

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

						ESP_LOGI(TAG, "syncing clock to server");
						tv1.tv_sec = base_message.sent.sec;
						tv1.tv_usec = base_message.sent.usec;
						settimeofday(&tv1, NULL);

						// Volume setting using ADF HAL abstraction
						audio_hal_set_mute(board_handle->audio_hal, server_settings_message.muted);
						audio_hal_set_volume(board_handle->audio_hal, server_settings_message.volume);

						if (syncTask == NULL) {
							ESP_LOGI(TAG, "[ 8 ] Start snapcast_sync_task");

							snapcastTaskCfg.sync_queue_handle = &wireChunkQueueHandle;
							snapcastTaskCfg.p_raw_stream_reader = p_raw_stream_reader;
							snapcastTaskCfg.outputBufferDacTime_us = outputBufferDacTime_us;
							snapcastTaskCfg.buffer_us = (int64_t)buffer_ms * 1000LL;
							xTaskCreatePinnedToCore(&snapcast_sync_task, "snapcast_sync_task", 4*4096, &snapcastTaskCfg, SYNC_TASK_PRIORITY, &syncTask, SYNC_TASK_CORE_ID);
						}

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

					   diffBuf[diffBufCnt++] = tmpDiffToServer;
					   if (diffBufCnt >= (sizeof(diffBuf)/sizeof(struct timeval))) {
						   diffBufCnt = 0;
					   }

					   set_diff_to_server(diffBuf, sizeof(diffBuf) / sizeof(struct timeval), &diffBufCnt);

					break;
				}
            }


            // If it's been a second or longer since our last time message was
            // sent, do so now
            result = gettimeofday(&now, NULL);
            if (result) {
                ESP_LOGI(TAG, "Failed to gettimeofday\r\n");
                return;
            }

            // use time we got from before
            timersub(&now, &last_time_sync, &tv1);
            if (tv1.tv_sec >= 1) {
                last_time_sync.tv_sec = now.tv_sec;
                last_time_sync.tv_usec = now.tv_usec;

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

                //ESP_LOGI(TAG, "sent time sync message");
            }

            vTaskDelay( 1 / portTICK_PERIOD_MS );
        }

        if (syncTask != NULL) {
			vTaskDelete(syncTask);
			syncTask = NULL;
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
void sntp_cb(struct timeval *tv)
{   struct tm timeinfo = { 0 };
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
void app_main(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t raw_stream_reader, i2s_stream_writer, decoder;
    esp_err_t ret;
    uint8_t base_mac[6];

	ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


//    tcpip_adapter_init();
//    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
//    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
//    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
//    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
//    wifi_config_t sta_config = {
//        .sta = {
//            .ssid = CONFIG_ESP_WIFI_SSID,
//            .password = CONFIG_ESP_WIFI_PASSWORD,
//            .bssid_set = false
//        }
//    };
//    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
//    ESP_ERROR_CHECK( esp_wifi_start() );
//    ESP_ERROR_CHECK( esp_wifi_connect() );

    wifi_init_sta();

	// Get MAC address for WiFi station
	esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
	sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
	ESP_LOGI(TAG, "MAC Adress is: %s", mac_address);

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    //pipeline_cfg.rb_size = 1024;
    pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_NULL_CHECK(TAG, pipeline, return);

    ESP_LOGI(TAG, "[3.1] Create raw stream to read data from snapcast");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    //raw_cfg.out_rb_size = 1024;
    raw_stream_reader = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    //i2s_cfg.task_stack = I2S_STREAM_TASK_STACK * 2;
    i2s_cfg.i2s_config.sample_rate = 48000;
    //i2s_cfg.i2s_config.dma_buf_count = 8;
    //i2s_cfg.i2s_config.dma_buf_len = 480;
    //i2s_cfg.out_rb_size = 1024;
    i2s_cfg.task_core = I2S_TASK_CORE_ID;
    i2s_cfg.task_prio = I2S_TASK_PRIORITY;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.1] Create flac decoder to decode flac file and set custom read callback");
	flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
	//flac_cfg.out_rb_size = 1024;
	decoder = flac_decoder_init(&flac_cfg);

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, raw_stream_reader, "raw");
    audio_pipeline_register(pipeline, decoder, "decoder");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[3.5] Link it together [flash]-->raw-->decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"raw", "decoder", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 6 ] Listen for all pipeline events");

    // syncing to sntp
#if CONFIG_USE_SNTP == 1
	vTaskDelay(5000/portTICK_PERIOD_MS);
	ESP_LOGI(TAG, "[ 7 ] Syncing to sntp");
	set_time_from_sntp();
#else
	{
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
		ESP_LOGI(TAG, "[ 7 ] Current time is %s", buf);
	}
#endif

	ESP_LOGI(TAG, "[ 8 ] Start snapclient task");
    xTaskCreatePinnedToCore(&http_get_task, "http_get_task", 4*4096, &raw_stream_reader, HTTP_TASK_PRIORITY, NULL, HTTP_TASK_CORE_ID);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
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

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    //audio_pipeline_unregister(pipeline, spiffs_stream_reader);
    audio_pipeline_unregister(pipeline, raw_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, decoder);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(raw_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(decoder);
}



