// libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "driver/spi_master.h"
#include <esp_event.h>
#include <esp_system.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <esp_wifi.h>
#include <esp_log.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include <esp_http_server.h>
#include "ssd1306.h"
#include "font8x8_basic.h"
#include "dht11.h"
#include "MLX90614_API.h"
#include "MLX90614_SMBus_Driver.h"

// define
#define tag "SSD1306"
#define SSID "192.168.4.1" //WIFI NAME
#define PASS "12345678" // PASSWORD
#define MLX_ADDRESS 0x5A	//I2C ADDRESS OF MLX SENSOR
//DECLARE 2 I2C PINS FOR MLX SENSOR
#define MLX_SDA 21 // SDA PIN FOR MLX SENSOR
#define MLX_SCL 22 // SCL PIN FOR MLX SENSOR
#define DHT11_PIN 26

// user-defined types
struct async_resp_arg { //PARAMETERS TO BE PASSED INTO URI HANDLER
	httpd_handle_t hd; //HANDLER TO HTTP SERVER
	int fd;           //SOCKET
};

// function prototypes
void oled_task(void *pvParameter);
void read_sensor_task();
void wifi_task();
static void wifi_event_handler(void *event_handler_arg,
		esp_event_base_t event_base, int32_t event_id, void *event_data);
static httpd_handle_t web_server_task(void);
static esp_err_t uri__handler(httpd_req_t *req);
static void response_request(void *arg);

// constants
static const char *TAG = "server";
static const char *SOCKET = "SOCKET: ";
static const char *WIFI = "WIFI: ";

// variables
static esp_adc_cal_characteristics_t adc1_chars;
float MLX_temp = 0; // Temperature read from MLX
int humi = 0;// Humidity variable
int temp = 0;	// Converted temperature variable
int soil = 0;// Soil moisture variable
char humi_text[12];// Char array for storing humidity in text to display
char temp_text[12];// Char array for storing temperature in text to display
char soil_text[20]; // Char array for storing soil humidity in text to display
char noti_text[] = "Connect for more"; // Text displayed on OLED
char ip_text[] = "192.168.4.1"; // Local host IP
TaskHandle_t xHandle_oled = NULL; // OLED Task handler



///////////////////////////////////////////
// APPLICATION
///////////////////////////////////////////


void app_main(void) {
	nvs_flash_init();
	// Initialize softAP
	wifi_task();
	vTaskDelay(2000 / portTICK_RATE_MS);
	// Start web server
	web_server_task();
	// Create OLED task
	xTaskCreatePinnedToCore(&oled_task, "oled", 4096, NULL, 5, &xHandle_oled, 1);
	// CREATE reading senser task
	xTaskCreatePinnedToCore(&read_sensor_task, "sensor", 4096, NULL, 5, NULL, 1);
}



///////////////////////////////////////////
// TASK FOR DISPLAYING OLED
///////////////////////////////////////////

void oled_task(void *pvParameter) {
	// Initialize OLED
	SSD1306_t dev;
	spi_master_init(&dev, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO,
	CONFIG_DC_GPIO, CONFIG_RESET_GPIO);
	ssd1306_init(&dev, 128, 64);
	ssd1306_clear_screen(&dev, false);
	ssd1306_contrast(&dev, 0xff);

	while (1) {
		// Wait for notification
		xTaskGenericNotifyWait(0, 0x8000, 0x8000, NULL, portMAX_DELAY);
		// OLED display
		ssd1306_display_text(&dev, 0, humi_text, sizeof(humi_text), false);
		ssd1306_display_text(&dev, 1, temp_text, sizeof(temp_text), false);
		ssd1306_display_text(&dev, 2, "Soil moisture:", 15, false);
		ssd1306_display_text(&dev, 3, soil_text, sizeof(soil_text), false);
		ssd1306_display_text(&dev, 5, noti_text, sizeof(noti_text), false);
		ssd1306_display_text(&dev, 6, ip_text, sizeof(ip_text), false);

	}
}



///////////////////////////////////////////
// TASK FOR READING SENSOR
///////////////////////////////////////////

void read_sensor_task(void *pvParameter) {
	// Initialize DHT
	DHT11_init(DHT11_PIN);	//DATA PIN

	// Use ADC to read soil moisture
	esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);

	adc1_config_width(ADC_WIDTH_BIT_10);                    // 10Bit - ADC resolution
	adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11); // Channel 3 is PIN 39

	// Use infrared thermometer
	MLX90614_SMBusInit(MLX_SDA, MLX_SCL, 50000); //SCL Frequency: 50KHz, Frequency must be between 10KHz - 100KHz

	TickType_t xLastWakeTime;
	while (1) {
		// Get temperature value from MLX
		MLX90614_GetTa(MLX_ADDRESS, &MLX_temp);
		temp = MLX_temp;
		// Get humidity value from DHT11
		humi = DHT11_read().humidity;
		// Convert value to text
		sprintf(humi_text, "Humi: %d %%", humi);
		sprintf(temp_text, "Temp: %d 'C", temp);

		// Read soil humidity value via ADC
		int adc_value = adc1_get_raw(ADC1_CHANNEL_3);
		soil = -0.255 * adc_value + 216.327;
		if (soil > 100)
			soil = 100;
		if (soil < 0)
			soil = 0;
		sprintf(soil_text, "      %d %%", soil);

		// Notify OLED
		xTaskGenericNotify(xHandle_oled, 0, 0x8000, eSetBits, NULL);
		// Timing for reading sensors
		vTaskDelayUntil(&xLastWakeTime, 2000 / portTICK_RATE_MS);
	}
}



///////////////////////////////////////////
// TASKS FOR STARTING WIFI ACCESS POINT
///////////////////////////////////////////

// Wifi event handler
static void wifi_event_handler(void *event_handler_arg,
		esp_event_base_t event_base, int32_t event_id, void *event_data) {
	switch (event_id) {
	case WIFI_EVENT_AP_STACONNECTED:
		ESP_LOGI(WIFI, "STA conenected to AP");
		break;
	case WIFI_EVENT_AP_STADISCONNECTED:
		ESP_LOGI(WIFI, "STA disconnected from AP");
		break;
	default:
		break;
	}
}


void wifi_task() {
	// Initialize NVS
	nvs_flash_init();

	// create an LwIP core task and initialize LwIP-related work.
	esp_netif_init();

	// create a system Event task and initialize an application event's callback function
	esp_event_loop_create_default();

	// create default network interface instance binding AP with TCP/IP stack.
	esp_netif_create_default_wifi_ap();

	// create wifi driver task and initialize the driver with the default configuration
	wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&wifi_initiation);

	// register event handler for wifi events
	esp_event_handler_register(WIFI, ESP_EVENT_ANY_ID, wifi_event_handler,
	NULL);
	wifi_config_t wifi_configuration = { 
		.ap = { 
			.ssid = SSID, 
			.ssid_len = strlen(SSID), 
			.password = PASS,
			.authmode = WIFI_AUTH_WPA2_PSK,
			.max_connection = 4, 
		} 
	};
	esp_wifi_set_mode(WIFI_MODE_AP);
	esp_wifi_set_config(WIFI_IF_AP, &wifi_configuration);
	esp_wifi_start();
}


///////////////////////////////////////////
// TASK FOR STARTING AND OPERATING WEB SERVER
///////////////////////////////////////////


//SENDING HTTP RESPONSE FOR uri__handler
static void response_request(void *arg) {
	char http_string[250];
	char data_string[400];

	// create HTTP message
	sprintf(data_string,
			"<!DOCTYPE html><html>\n<head>\n<style>\nh1 {text-align: center;}\ntemp {text-align: center;}\nhumid {text-align: center;}\nmois {text-align: center;}\n</style>\n</head>\n<body>\n<h1>Tree-monitoring application</h1>\n<center><p>Temperature: %d</p></center>\n<center><p>Humidity: %d</p></center>\n<center><p>Soil Moisture: %d %% </p></center> \n</body>\n</html>",
			temp, humi, soil);
	sprintf(http_string, "HTTP/1.1 200 OK \r\nContent-Length: %d\r\n\r\n",
			strlen(data_string));

	// send message
	struct async_resp_arg *resp_arg = (struct async_resp_arg*) arg;
	httpd_handle_t hd = resp_arg->hd;
	int fd = resp_arg->fd;
	httpd_socket_send(hd, fd, http_string, strlen(http_string), 0);
	httpd_socket_send(hd, fd, data_string, strlen(data_string), 0);

	ESP_LOGI(SOCKET, "Executing queued work fd : %d", fd);
	free(arg);
}


// URI "/" HANDLER
static esp_err_t uri__handler(httpd_req_t *req) {
	struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
	resp_arg->hd = req->handle;
	resp_arg->fd = httpd_req_to_sockfd(req);
	ESP_LOGI(SOCKET, "Queuing work fd : %d", resp_arg->fd);
	httpd_queue_work(req->handle, response_request, resp_arg);
	return ESP_OK;
}

// URI "/"
static const httpd_uri_t get_ = { .uri = "/", .method = HTTP_GET, .handler =
		uri__handler, .user_ctx = NULL };


// Start web server
static httpd_handle_t web_server_task(void) {
	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.core_id = 0;
	// START HTTPD SERVER
	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if (httpd_start(&server, &config) == ESP_OK) {
		// REGISTERING HANDLER
		ESP_LOGI(TAG, "Registering URI handlers");
		httpd_register_uri_handler(server, &get_);
		return server;
	}

	ESP_LOGI(TAG, "Error starting server!");
	return NULL;
}

