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

//#include "driver/timer.h"

#include <sys/time.h>



uint64_t wirechnkCnt = 0;
uint64_t pcmchnkCnt = 0;


#define CONFIG_USE_SNTP		0

#define DAC_OUT_BUFFER_TIME_US		0//3 * 1526LL		//TODO: not sure about this... // @48kHz, 16bit samples, 2 channels and a DMA buffer length of 300 Byte and 3 buffers. 300 Byte / (48000 * 2 Byte * 2 channels)

static const char *TAG = "SNAPCLIENT";

static int sntp_synced = 0;

char *codecString = NULL;

// configMAX_PRIORITIES - 1

// TODO: what are the best values here?
#define SYNC_TASK_PRIORITY		6
#define SYNC_TASK_CORE_ID  		1

#define HTTP_TASK_PRIORITY		6
#define HTTP_TASK_CORE_ID  		0

#define I2S_TASK_PRIORITY  		0
#define I2S_TASK_CORE_ID  		1

#define FLAC_DECODER_PRIORITY	6
#define FLAC_DECODER_CORE_ID	1

#define AGE_THRESHOLD	50LL	// in µs


QueueHandle_t timestampQueueHandle;
#define TIMESTAMP_QUEUE_LENGTH 	5		// TODO: what's the minimum value needed here, although probably not that important because we create queue using xQueueCreate()
static StaticQueue_t timestampQueue;
uint8_t timestampQueueStorageArea[ TIMESTAMP_QUEUE_LENGTH * sizeof(tv_t) ];

QueueHandle_t pcmChunkQueueHandle;
#define PCM_CHNK_QUEUE_LENGTH 150	// TODO: one chunk is hardcoded to 20ms, change it to be dynamically adjustable. 1s buffer = 50
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

static struct timeval diffToServer = {0, 0};	// median diff to server in µs
static struct timeval diffBuf[50] = {0};	// collected diff's to server
//static struct timeval *medianArray = NULL;	// temp median calculation data is stored at this location
static struct timeval medianArray[50] = {0};	// temp median calculation data is stored at this location

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
int8_t set_diff_to_server( struct timeval *tDiff, size_t len) {
	int8_t ret = -1;
	struct timeval tmpDiffToServer;

	if (diffBufSemaphoreHandle == NULL) {
		ESP_LOGE(TAG, "set_diff_to_server: mutex handle == NULL");

		return -1;
	}

    ret = get_median(tDiff, len, &tmpDiffToServer);
    if (ret < 0) {
    	ESP_LOGW(TAG, "set_diff_to_server: get median failed");
    }

    //ESP_LOGI(TAG, "set_diff_to_server: median is %ld.%06ld", tmpDiffToServer.tv_sec, tmpDiffToServer.tv_usec);

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
		ESP_LOGE(TAG, "get_diff_to_server: diffBufSemaphoreHandle == NULL");

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
		//ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

		return -1;
	}

	timeradd(&now, &diff, sNow);

//	ESP_LOGI(TAG, "now: %lldus", (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec);
//	ESP_LOGI(TAG, "diff: %lldus", (int64_t)diff.tv_sec * 1000000LL + (int64_t)diff.tv_usec);
//	ESP_LOGI(TAG, "serverNow: %lldus", (int64_t)sNow->tv_sec * 1000000LL + (int64_t)sNow->tv_usec);

	return 0;
}


static void snapcast_sync_task(void *pvParameters) {
	snapcast_sync_task_cfg_t *taskCfg = (snapcast_sync_task_cfg_t *)pvParameters;
	wire_chunk_message_t *chnk = NULL;
	struct timeval serverNow = {0, 0};
	int64_t age;
	BaseType_t ret;
	uint8_t initial_sync = 0;

	ESP_LOGI(TAG, "started sync task");

	while(1) {
		if (chnk == NULL) {
			ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(3000) );
		}
		else {
			ret = pdPASS;
		}

		if( ret == pdPASS )	{
			if (initial_sync > 5) {		// hard sync was successful?
				if (server_now(&serverNow) >= 0) {
					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
							((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
							(int64_t)taskCfg->buffer_us +
							(int64_t)taskCfg->outputBufferDacTime_us;

//					ESP_LOGI(TAG, "before age: %lldus", age);
					if (age < -(int64_t)taskCfg->buffer_us / 10LL) {	// if age gets younger than 1/10 of buffer then do a hard resync
						initial_sync = 0;

						ESP_LOGW(TAG, "need resync, skipping chnk. age: %lldus", age);

						free(chnk->payload);
						free(chnk);
						chnk = NULL;

						continue;
					}
					else if (age < -1099LL) {
						//ESP_LOGI(TAG, "age: %lldus", age);

						vTaskDelay( pdMS_TO_TICKS(-age / 1000LL) );	//((TickType_t)(-age / 1000LL));	// TODO: find better way to do a exact delay to age, skipping probably goes away afterwards
					}
					else if (age > 0) {
						ESP_LOGW(TAG, "skipping chunk, age: %lldus", age);

						free(chnk->payload);
						free(chnk);
						chnk = NULL;

						continue;
					}
				}

				// just for testing, print age
//				if (server_now(&serverNow) >= 0) {
//					age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
//							((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
//							(int64_t)taskCfg->buffer_us +
//							(int64_t)taskCfg->outputBufferDacTime_us;
//
//					ESP_LOGI(TAG, "after age: %lldus", age);
//				}

				int bytesWritten = 0;
				while ( bytesWritten < chnk->size ) {
					bytesWritten += raw_stream_write(*(taskCfg->p_raw_stream_writer), chnk->payload, chnk->size);
					if (bytesWritten < chnk->size) {
						ESP_LOGE(TAG, "i2s raw writer ring buf full");
						vTaskDelay(100);
					}
				}

				free(chnk->payload);
				free(chnk);
				chnk = NULL;
			}
			else if (server_now(&serverNow) >= 0) {		// hard syncing
				// calc age in µs
				age =   ((int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec) -
						((int64_t)chnk->timestamp.sec * 1000000LL + (int64_t)chnk->timestamp.usec) -
						(int64_t)taskCfg->buffer_us +
						(int64_t)taskCfg->outputBufferDacTime_us;

//				ESP_LOGI(TAG, "age: %lldus, %lldus, %lldus, %lldus, %lldus", age,
//																			 (int64_t)serverNow.tv_sec * 1000000LL + (int64_t)serverNow.tv_usec,
//																			 (int64_t)wireChnk->timestamp.sec * 1000000LL + (int64_t)wireChnk->timestamp.usec,
//																			 (int64_t)taskCfg->buffer_us,
//																			 (int64_t)taskCfg->outputBufferDacTime_us);

				if (age < -(int64_t)taskCfg->buffer_us) {
					// fast forward
					ESP_LOGW(TAG, "fast forward, age: %lldus", age);

					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}
				else if ((age >= -AGE_THRESHOLD ) && ( age <= 0 )) {
					initial_sync++;

					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}
				else if (age > 0 ) {
					ESP_LOGW(TAG, "too old, age: %lldus", age);

					initial_sync = 0;

					free(chnk->payload);
					free(chnk);
					chnk = NULL;
				}
			}
			else {
				vTaskDelay( pdMS_TO_TICKS(10) );
			}
		}
		else {
			ESP_LOGE(TAG, "Couldn't get PCM chunk");
		}
	}
}



/**
 *
 */
static void http_get_task(void *pvParameters) {
	http_task_cfg_t *httpTaskCfg = (http_task_cfg_t *)pvParameters;
	audio_element_handle_t *p_raw_stream_writer;
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
    const int64_t outputBufferDacTime_us = DAC_OUT_BUFFER_TIME_US;	// in ms
    TaskHandle_t syncTask = NULL;
    snapcast_sync_task_cfg_t snapcastTaskCfg;
	struct timeval lastTimeSync = { 0, 0 };
	uint8_t bufferFull = false;
    
    p_raw_stream_writer = httpTaskCfg->p_raw_stream_writer_to_decoder;

    // create semaphore for time diff buffer to server
    diffBufSemaphoreHandle = xSemaphoreCreateMutex();

    last_time_sync.tv_sec = 0;
    last_time_sync.tv_usec = 0;

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

							raw_stream_write(*p_raw_stream_writer, codec_header_message.payload, size);

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

						// store chunk's timestamp, decoder callback will need it later
						tv_t timestamp;
						timestamp = wire_chunk_message.timestamp;

						int bytesWritten = 0;

						wirechnkCnt++;
						//ESP_LOGI(TAG, "wirechnkCnt: %lld", wirechnkCnt);

						while ( bytesWritten < wire_chunk_message.size ) {
							bytesWritten += raw_stream_write(*p_raw_stream_writer, wire_chunk_message.payload, wire_chunk_message.size);
							if (bytesWritten < wire_chunk_message.size) {
								ESP_LOGE(TAG, "wirechnk decode ring buf full");
								vTaskDelay(100);
							}
						}

						if (xQueueSendToBack( timestampQueueHandle, &timestamp, pdMS_TO_TICKS(3000)) == pdTRUE) {
							// write encoded data to decoder pipeline, callback will trigger when it's finished
							// also we do a check if all data was written successfully
//							int bytesWritten = 0;
//
//							wirechnkCnt++;
//							//ESP_LOGI(TAG, "wirechnkCnt: %lld", wirechnkCnt);
//
//							while ( bytesWritten < wire_chunk_message.size ) {
//								bytesWritten += raw_stream_write(*p_raw_stream_writer, wire_chunk_message.payload, wire_chunk_message.size);
//								if (bytesWritten < wire_chunk_message.size) {
//									ESP_LOGE(TAG, "wirechnk decode ring buf full");
//									vTaskDelay(100);
//								}
//							}
						}
						else {
							ESP_LOGW(TAG, "timestamp queue full, dropping data ...");
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

						ESP_LOGI(TAG, "syncing clock to server");
						tv1.tv_sec = base_message.sent.sec;
						tv1.tv_usec = base_message.sent.usec;
						settimeofday(&tv1, NULL);

						// Volume setting using ADF HAL abstraction
						audio_hal_set_mute(board_handle->audio_hal, server_settings_message.muted);
						audio_hal_set_volume(board_handle->audio_hal, server_settings_message.volume);

						if (syncTask == NULL) {
							ESP_LOGI(TAG, "Start snapcast_sync_task");

							snapcastTaskCfg.p_raw_stream_writer = httpTaskCfg->p_raw_stream_writer_to_i2s;
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

//						ESP_LOGI(TAG, "Current latency: %ld.%06ld", tmpDiffToServer.tv_sec, tmpDiffToServer.tv_usec);

						// following code is storing / initializing / resetting diff to server algorithm
						// we collect a number of latencies. Basded on these we can get the median of server now
						{
							struct timeval diff;
							// clear diffBuffer if last update is older than a minute
							timersub(&now, &lastTimeSync, &diff);
							if ( diff.tv_sec > 60 ) {
								ESP_LOGW(TAG, "Last time sync older than a minute. Clearing time buffer");

								memset(diffBuf, 0, sizeof(diffBuf));
								diffBufCnt = 0;
								bufferFull = false;
							}

							// store current time for next run
							lastTimeSync.tv_sec = now.tv_sec;
							lastTimeSync.tv_usec = now.tv_usec;

						   diffBuf[diffBufCnt++] = tmpDiffToServer;
						   if (diffBufCnt >= (sizeof(diffBuf)/sizeof(struct timeval))) {
							   bufferFull = true;

							   diffBufCnt = 0;
						   }

						   size_t bufLen;
						   if (bufferFull == true) {
							   bufLen = sizeof(diffBuf)/sizeof(struct timeval);
						   }
						   else {
							   bufLen = diffBufCnt;
						   }

						   set_diff_to_server(diffBuf, bufLen);
						}

					break;
				}
            }


            // TODO: create a dedicated task for this which is started upon connect and deleted upon disconnect
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


int flac_decoder_write_cb(audio_element_handle_t el, char *buffer, int len, TickType_t ticks_to_wait, void *context) {
	wire_chunk_message_t *pcm_chunk_message;
	tv_t timestamp;
	int ret = len;

	//ESP_LOGI(TAG, "flac_decoder_write_cb: got buffer with length %d", len);

//	audio_element_info_t music_info = {0};
//	audio_element_getinfo(el, &music_info);
//	ESP_LOGI(TAG, "[ cb ] Receive music info from %s decoder, sample_rates=%d, bits=%d, ch=%d, total_bytes: %lld, byte_pos:%lld",
//			 codecString, music_info.sample_rates, music_info.bits, music_info.channels, music_info.total_bytes, music_info.byte_pos);


	if (xQueueReceive( timestampQueueHandle, &timestamp, 0 ) == pdPASS) {
		pcm_chunk_message = (wire_chunk_message_t *)malloc(sizeof(wire_chunk_message_t));
		if (pcm_chunk_message == NULL) {
			ESP_LOGE(TAG, "flac_decoder_write_cb: Failed to allocate memory for pcm chunk message");

			return AEL_IO_FAIL;
		}

		pcm_chunk_message->payload = (char *)malloc(sizeof(char) * len);
		if (pcm_chunk_message->payload == NULL) {
			ESP_LOGE(TAG, "flac_decoder_write_cb: Failed to allocate memory for pcm chunk payload");

			return AEL_IO_FAIL;
		}

		pcm_chunk_message->size = len;
		pcm_chunk_message->timestamp = timestamp;
		memcpy(pcm_chunk_message->payload, buffer, len);

		ret = len;
		if( xQueueSendToBack( pcmChunkQueueHandle, &pcm_chunk_message, pdMS_TO_TICKS(5)) != pdPASS ) {
			ESP_LOGE(TAG, "flac_decoder_write_cb: Failed to post the message");

			free(pcm_chunk_message->payload);
			free(pcm_chunk_message);

			ret = AEL_IO_FAIL;
		}
		else {
			pcmchnkCnt++;
			//if ((wirechnkCnt - pcmchnkCnt) > 3)
			{
				//ESP_LOGW(TAG, "diff: %lld", wirechnkCnt - pcmchnkCnt);
			}
		}
	}
	else {
		ESP_LOGW(TAG, "flac_decoder_write_cb: failed to get timestamp for it");

		ret = AEL_IO_FAIL;
	}

	return ret;
}

/**
 *
 */
void app_main(void) {
    audio_pipeline_handle_t flacDecodePipeline;
    audio_element_handle_t raw_stream_writer_to_decoder, decoder;
    audio_pipeline_handle_t playbackPipeline;
    audio_element_handle_t raw_stream_writer_to_i2s, i2s_stream_writer;
    esp_err_t ret;
    uint8_t base_mac[6];
    http_task_cfg_t httpTaskCfg = {NULL, NULL};

	ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

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
    flac_dec_pipeline_cfg.rb_size = 16 * 4096;	// TODO: how much is really needed?
    AUDIO_NULL_CHECK(TAG, flacDecodePipeline, return);

    ESP_LOGI(TAG, "Create raw stream to write data from snapserver to decoder");
    raw_stream_cfg_t raw_1_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_1_cfg.type = AUDIO_STREAM_WRITER;
    raw_1_cfg.out_rb_size = 16 * 4096;	// TODO: how much is really needed?
    raw_stream_writer_to_decoder = raw_stream_init(&raw_1_cfg);

    ESP_LOGI(TAG, "Create flac decoder to decode flac file and set custom write callback");
	flac_decoder_cfg_t flac_cfg = DEFAULT_FLAC_DECODER_CONFIG();
	flac_cfg.task_prio = FLAC_DECODER_PRIORITY;
	flac_cfg.task_core = FLAC_DECODER_CORE_ID;
	flac_cfg.out_rb_size = 16 * 4096;	// TODO: how much is really needed?
	decoder = flac_decoder_init(&flac_cfg);
	audio_element_set_write_cb(decoder, flac_decoder_write_cb, NULL);

    ESP_LOGI(TAG, "Register all elements to audio pipeline");
    audio_pipeline_register(flacDecodePipeline, raw_stream_writer_to_decoder, "raw_1");
    audio_pipeline_register(flacDecodePipeline, decoder, "decoder");

    ESP_LOGI(TAG, "Link it together [snapclient]-->raw_1-->decoder");
	const char *link_tag[2] = {"raw_1", "decoder"};
    audio_pipeline_link(flacDecodePipeline, &link_tag[0], 2);

    ESP_LOGI(TAG, "Create audio pipeline for playback");
	audio_pipeline_cfg_t playback_pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	playback_pipeline_cfg.rb_size = 16 * 4096;	// TODO: how much is really needed?
	playbackPipeline = audio_pipeline_init(&playback_pipeline_cfg);
	AUDIO_NULL_CHECK(TAG, playbackPipeline, return);

    ESP_LOGI(TAG, "Create raw stream to write data from decoder to i2s");
    raw_stream_cfg_t raw_2_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_2_cfg.type = AUDIO_STREAM_WRITER;
    raw_2_cfg.out_rb_size = 16 * 4096;	// TODO: how much is really needed?
    raw_stream_writer_to_i2s = raw_stream_init(&raw_2_cfg);

    ESP_LOGI(TAG, "Create i2s stream to write data to codec chip");
	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
	//i2s_cfg.task_stack = I2S_STREAM_TASK_STACK * 2;
	i2s_cfg.i2s_config.sample_rate = 48000;
	i2s_cfg.i2s_config.dma_buf_count = 4;
	i2s_cfg.i2s_config.dma_buf_len = 410;
	i2s_cfg.out_rb_size = 4 * 410;
	i2s_cfg.task_core = I2S_TASK_CORE_ID;
	//i2s_cfg.task_prio = I2S_TASK_PRIORITY;
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

	httpTaskCfg.p_raw_stream_writer_to_decoder = &raw_stream_writer_to_decoder;
	httpTaskCfg.p_raw_stream_writer_to_i2s = &raw_stream_writer_to_i2s;
    xTaskCreatePinnedToCore(&http_get_task, "http_get_task", 4*4096, &httpTaskCfg, HTTP_TASK_PRIORITY, NULL, HTTP_TASK_CORE_ID);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret;

        // listen to events
        ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(decoder, &music_info);

            if (codecString != NULL) {
				ESP_LOGI(TAG, "[ * ] Receive music info from %s decoder, sample_rates=%d, bits=%d, ch=%d, total_bytes: %lld, byte_pos:%lld",
									 codecString, music_info.sample_rates, music_info.bits, music_info.channels, music_info.total_bytes, music_info.byte_pos);
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

    audio_pipeline_deinit(playbackPipeline);
    audio_element_deinit(raw_stream_writer_to_i2s);
    audio_element_deinit(i2s_stream_writer);

    // TODO: clean up all created tasks and delete them
}



