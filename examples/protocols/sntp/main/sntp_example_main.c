/* LwIP SNTP example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_deep_sleep.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "apps/sntp/sntp.h"

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD
//Leds to flash
#define led1 32
#define led2 16
#define TimingPin 17

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static const char *TAG = "example";

/* Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;
/* Variable holding which led was last on.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
static RTC_DATA_ATTR int led = led1;  // initial state


static void obtain_time(void);
static void initialize_sntp(void);
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);

uint64_t rtc_time_get(void);

void app_main()
{
	esp_log_level_set("*", ESP_LOG_WARN);      // set all components to WARN level
	gpio_pad_select_gpio(TimingPin);
	/* Set the GPIO as a push/pull output */
	gpio_set_direction(TimingPin, GPIO_MODE_OUTPUT);
		/* Turn on (output high)*/
	gpio_set_level(TimingPin, 1);
    ++boot_count;

    struct timeval tv_now;
    gettimeofday (&tv_now, NULL);
    printf("gettimeofday: %ld.%06ld\n", tv_now.tv_sec, tv_now.tv_usec); fflush(stdout);
    ESP_LOGW(TAG, "Boot count: %d", boot_count);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGW(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }
    char strftime_buf[64];

    /* Set timezone to Eastern Standard Time and print local time
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGW(TAG, "The current date/time in New York is: %s", strftime_buf);*/

    // Set timezone to Canberra Standard Time
//    setenv("TZ", "CST-8", 1);
    setenv("TZ", "AEST-10", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGW(TAG, "The current date/time in Canberra is: %s", strftime_buf);
    //Flash led
    gpio_pad_select_gpio(led1);
	gpio_pad_select_gpio(led2);
	/* Set the GPIO as a push/pull output */
	gpio_set_direction(led1, GPIO_MODE_OUTPUT);
	gpio_set_direction(led2, GPIO_MODE_OUTPUT);
	/* Blink on (output high)*/
	gpio_set_level(led, 1);
	ESP_LOGW(TAG, "Led value at flash start: %d", led);
	vTaskDelay(1000 / portTICK_PERIOD_MS);
	/* Blink off (output low) */
	gpio_set_level(led, 0);
    led ^= led1^led2;           // kept in RTC memory
	//End Led flash
    const int deep_sleep_sec = 1;
    ESP_LOGW(TAG, "Entering deep sleep for %d seconds", deep_sleep_sec);
    gpio_set_level(TimingPin, 0);
    esp_deep_sleep(1000000LL * deep_sleep_sec);
}

static void obtain_time(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    initialise_wifi();
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGW(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    ESP_ERROR_CHECK( esp_wifi_stop() );
}

static void initialize_sntp(void)
{
    ESP_LOGW(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGW(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

