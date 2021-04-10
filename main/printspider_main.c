#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "lwip/apps/netbiosns.h"
#include "esp_http_server.h"
#include "multipartparser.h"
#include "printspider_i2s.h"
#include "printspider_buffer_filler.h"
#include "printspider_genwaveform.h"

/* The examples use WiFi configuration that you can set via project configuration menu
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define WIFI_SSID "mywifissid"
*/
#define WIFI_SSID      "linux-core-rulez"
#define WIFI_PASSWORD  "linux-core12354"
#define WIFI_MAXIMUM_RETRY  20

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "PritSpider";

static int s_retry_num = 0;

static bool image_color = true;
static int image_width = 0;
static int image_height = 0;
static uint8_t* image = NULL;
static size_t image_len = 0;

#define MDNS_HOSTNAME "printspider"
#define MDNS_INSTANCE_NAME "PrintSpider esp32 board"

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
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

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
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
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void start_mdns_service()
{
    //initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) {
        ESP_LOGE(TAG, "MDNS Init failed: %d\n", err);
        return;
    }

    //set hostname
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    //set default instance
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE_NAME));

    //set http service
    ESP_ERROR_CHECK(mdns_service_add("HTTP service", "_http", "_tcp", 80, NULL, 0));
}

static char* read_header_value(httpd_req_t *req, char* header_key, size_t *buf_len) {
    char *buf;
    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    *buf_len = httpd_req_get_hdr_value_len(req, header_key) + 1;
    if (*buf_len > 1) {
        buf = malloc(*buf_len);
        /* Copy null terminated value string into buffer */
        ESP_ERROR_CHECK(httpd_req_get_hdr_value_str(req, header_key, buf, *buf_len));
        ESP_LOGI(TAG, "Found header => %s: %s", header_key, buf);
        return buf;
    }
    return NULL;
}

/* An HTTP GET handler */
static esp_err_t main_get_handler(httpd_req_t *req)
{
    const char* main_html_str = "<form action='upload' method='post' enctype='multipart/form-data'>Select image to upload:<br><input type='file' name='fileToUpload' id='fileToUpload'><br>Width:<br><input type='text' name='width' id='width'><br>Height:<br><input type='text' name='height' id='height'><br><input type='hidden' name='color' value='1'><input type='checkbox' checked='on' onclick='this.previousSibling.value=1-this.previousSibling.value'><br><input type='submit' value='Upload Image' name='submit'></form><br><form action='print' method='get'><input type='submit' value='Print' name='submit'></form>";
    httpd_resp_send(req, main_html_str, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

static const httpd_uri_t main_uri = {
    .uri       = "/main",
    .method    = HTTP_GET,
    .handler   = main_get_handler,
};

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE       (2000*1024) // 2000 KB
#define MAX_FILE_SIZE_STR   "2000KB"
/* Scratch buffer size */
#define MULTIPART_BUF_SIZE  1024
#define SCRATCH_BUF_SIZE  2048

typedef struct miltipart_data
{
    char *field;

    char *name;
    char *filename;

    char *data_buf;
    size_t data_buf_len;
} miltipart_data;

static int upload_on_body_begin(multipartparser* p) {
    ESP_LOGI(TAG, "Upload body begin.");
    return 0;
}

static int upload_on_part_begin(multipartparser* p) {
    ESP_LOGI(TAG, "Upload part begin.");    
    return 0;
}

static int upload_on_header_field(multipartparser* p, const char *at, size_t length) {
    miltipart_data* data = p->data;
    if (length > 0) {
        data->field = (char*)malloc(length + sizeof(char));
        memcpy(data->field, at, length);
        data->field[length] = '\0';
        ESP_LOGI(TAG, "Upload header field: %s", data->field);
    }
    return 0;
}

char* get_val_by_key(const char *key, const char *at, size_t length) {
    char *key_start = (char*)malloc(strlen(key) + 2 * sizeof(char));
    strcpy(key_start, key);
    strcat(key_start, "=\"");
    const char *key_end = "\"";

    char *target = NULL;
    char *start;
    char *end;

    if ((start = strstr(at, key_start))) {
        start += strlen(key_start);
        if ((end = strstr(start, key_end))) {
            target = (char*)malloc(end - start + 1);
            memcpy(target, start, end - start);
            target[end - start] = '\0';
        }
    }
    if (target) {
        ESP_LOGI(TAG, "get_val_by_key target: %s", target);
    }
    free(key_start);
    return target;
}

static int upload_on_header_value(multipartparser* p, const char *at, size_t length) {
    miltipart_data* data = (miltipart_data*) p->data;
    if (strcmp(data->field, "Content-Disposition") == 0) {
        ESP_LOGI(TAG, "Header is Content-Disposition");    
        data->name = get_val_by_key("name", at, length);
        ESP_LOGI(TAG, "Upload header name: %s", data->name);
        if (strcmp(data->name, "fileToUpload") == 0) {
            data->filename = get_val_by_key("filename", at, length);
            ESP_LOGI(TAG, "Upload header filename: %s", data->filename);
        }
    }
    free(data->field);
    return 0;
}

static int upload_on_headers_complete(multipartparser* p) {
    ESP_LOGI(TAG, "Upload headers complete.");
    miltipart_data* data = (miltipart_data*) p->data;
    if (strcmp(data->name, "fileToUpload") == 0) {
        ESP_LOGI(TAG, "Freeing image...");
        free(image);
        image = NULL;
        image_len = 0;
    }
    return 0;
}

static int upload_on_data(multipartparser* p, const char *at, size_t length) {
    miltipart_data* data = (miltipart_data*) p->data;
    if (strcmp(data->name, "fileToUpload") == 0) {
        ESP_LOGI(TAG, "Image data processed...");
        if (!image && length > 0) {
            ESP_LOGI(TAG, "Malloc image with length %d", length);
            image = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
            memcpy(image, at, length);
            image_len = length;
        } else if (length > 0) {
            ESP_LOGI(TAG, "Realloc image with additional length %d and final length %d", length, (image_len + length));
            image = (uint8_t*)heap_caps_realloc(image, image_len + length, MALLOC_CAP_SPIRAM);
            memcpy(image + image_len, at, length);
            image_len = image_len + length;
        }
    } else if (strcmp(data->name, "width") == 0 || strcmp(data->name, "height") == 0 || strcmp(data->name, "color") == 0) {
        ESP_LOGI(TAG, "%s data processed...", data->name);
        if (!&(data->data_buf) && length > 0) {
            ESP_LOGI(TAG, "Malloc %s with length %d", data->name, length);
            data->data_buf = (char*)malloc(length);
            memcpy(data->data_buf, at, length);
            data->data_buf_len = length;
        } else if (length > 0) {
            ESP_LOGI(TAG, "Realloc %s with additional length %d and final length %d", data->name, length, data->data_buf_len + length);
            data->data_buf = (char*)realloc(data->data_buf, data->data_buf_len + length);
            memcpy(data->data_buf + data->data_buf_len, at, length);
            data->data_buf_len = data->data_buf_len + length;
        }
    }
    return 0;
}

static int upload_on_part_end(multipartparser* p) {
    ESP_LOGI(TAG, "Upload part end.");
    miltipart_data* data = (miltipart_data*) p->data;
    ESP_LOGI(TAG, "Data buffer lenght: %d", data->data_buf_len);
    if (strcmp(data->name, "width") == 0 || strcmp(data->name, "height") == 0 || strcmp(data->name, "color") == 0) {   
        if (data->data_buf_len > 0) {
            data->data_buf_len = data->data_buf_len + sizeof(char);
            data->data_buf = (char*)realloc(data->data_buf, data->data_buf_len);
            data->data_buf[data->data_buf_len - 1] = '\0';
            ESP_LOGI(TAG, "Data buffer: %s", data->data_buf);
            if (strcmp(data->name, "width") == 0) {
                image_width = atoi(data->data_buf);
                ESP_LOGI(TAG, "Image width set to: %d", image_width);
            }
            if (strcmp(data->name, "height") == 0) {
                image_height = atoi(data->data_buf);
                ESP_LOGI(TAG, "Image height set to: %d", image_height);
            }
            if (strcmp(data->name, "color") == 0) {
                image_color = (strcmp(data->data_buf, "1") == 0);
                ESP_LOGI(TAG, "Image color set to: %d", image_color);
            }
            free(data->data_buf);
            data->data_buf = NULL;
            data->data_buf_len = 0;
        } else {
            image_width = 0;
            image_height = 0;
        }
    }
    free(data->name);
    data->name = NULL;
    free(data->filename);
    data->filename = NULL;
    return 0;
}

static int upload_on_body_end(multipartparser* p) {
    ESP_LOGI(TAG, "Upload body end.");
    return 0;
}

static multipartparser_callbacks callbacks;

static esp_err_t init_upload_callbacks() {
    multipartparser_callbacks_init(&callbacks); // It only sets all callbacks to NULL.
    callbacks.on_body_begin = &upload_on_body_begin;
    callbacks.on_part_begin = &upload_on_part_begin;
    callbacks.on_header_field = &upload_on_header_field;
    callbacks.on_header_value = &upload_on_header_value;
    callbacks.on_headers_complete = &upload_on_headers_complete;
    callbacks.on_data = &upload_on_data;
    callbacks.on_part_end = &upload_on_part_end;
    callbacks.on_body_end = &upload_on_body_end;

    return ESP_OK;
}

esp_err_t parse_boundary(char *at, int *bound_len, char **bound_str) {
    // start offset
    size_t length = sizeof(at);
    size_t offset = sizeof("multipart/form-data;") - 1;
    while (at[offset] == ' ')
    {
        offset++;
    }
    offset += sizeof("boundary=") - 1;

    int boundary_len = length - offset;
    char *boundary_str = (char *) at + offset;

    // find ';'
    char *tmp = (char*) memchr(boundary_str, ';', boundary_len);
    if (tmp)
    {
        boundary_len = tmp - boundary_str;
    }
    if (boundary_len <= 0)
    {
        ESP_LOGE(TAG, "Invalid multipart/form-data body.");
        return ESP_FAIL;
    }
    // trim '"'
    if (boundary_len >= 2 && boundary_str[0] == '"' && *(boundary_str + boundary_len - 1) == '"')
    {
        boundary_str++;
        boundary_len -= 2;
    }
    *bound_len = boundary_len;
    *bound_str = boundary_str;
    return ESP_OK;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    /* Skip leading "/upload" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "File size must be less than "
                            MAX_FILE_SIZE_STR "!");
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
        return ESP_FAIL;
    }

    size_t content_type_len = 0;
    char *content_type = read_header_value(req, "Content-Type", &content_type_len);
    if (content_type_len <= 1 || (strncmp(content_type, "multipart/form-data;", strlen("multipart/form-data;")) != 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Should be multipart/form-data request.");
        return ESP_FAIL;
    }
    int boundary_len = 0;
    char *boundary_str;
    parse_boundary(content_type, &boundary_len, &boundary_str);
    ESP_LOGI(TAG, "Boundary: %s", boundary_str);

    multipartparser* parser = malloc(sizeof(multipartparser));
    multipartparser_init(parser, boundary_str);

    miltipart_data* data = malloc(sizeof(miltipart_data));
    data->field = NULL;
    data->name = NULL;
    data->filename = NULL;
    data->data_buf = NULL;
    data->data_buf_len = 0;

    parser->data = data;

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char* buf = malloc(SCRATCH_BUF_SIZE);
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUF_SIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        size_t nparsed;
        nparsed = multipartparser_execute(parser, &callbacks, buf, received);
        ESP_LOGI(TAG, "nparsed: %d", nparsed);
        if (nparsed != received) {
            ESP_LOGE(TAG, "Multipart parsing error. Parsed: %d. Should be: %d.", nparsed, received);
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }
    free(buf);
    free(data);
    free(parser);

    /* Close file upon upload completion */
    ESP_LOGI(TAG, "File reception complete");

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/main");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

static const httpd_uri_t upload_uri = {
    .uri       = "/upload",
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
};

//GPIO numbers for the lines that are connected (via level converters) to the printer cartridge.
#define PIN_NUM_CART_D2 12
#define PIN_NUM_CART_D1 27
#define PIN_NUM_CART_D3 13
#define PIN_NUM_CART_CSYNC 14
#define PIN_NUM_CART_S2 32
#define PIN_NUM_CART_S4 2
#define PIN_NUM_CART_S1 4
#define PIN_NUM_CART_S5 5
#define PIN_NUM_CART_DCLK 18
#define PIN_NUM_CART_S3 19
#define PIN_NUM_CART_F3 15
#define PIN_NUM_CART_F5 21

//Queue for nozzle data
QueueHandle_t nozdata_queue;

#define WAVEFORM_DMALEN 1500

//Selecting printsider waveform.
static esp_err_t select_waveform() {
    if (image_color) {
        ESP_LOGI(TAG, "Setting waveform PRINTSPIDER_WAVEFORM_COLOR_B");
    	printspider_select_waveform(PRINTSPIDER_WAVEFORM_COLOR_B);
    } else {
        ESP_LOGI(TAG, "Setting waveform PRINTSPIDER_WAVEFORM_BLACK_B");
        printspider_select_waveform(PRINTSPIDER_WAVEFORM_BLACK_B);
    }
    return ESP_OK;
}

static esp_err_t init_printing() {

    //Create nozzle data queue
	nozdata_queue=xQueueCreate(1, PRINTSPIDER_NOZDATA_SZ);

    //Initialize I2S parallel device. Use the function to generate waveform data from nozzle data as the callback
	//function.
	i2s_parallel_config_t i2scfg={
		.gpio_bus={
			PIN_NUM_CART_D1, //0
			PIN_NUM_CART_D2, //1
			PIN_NUM_CART_D3, //2
			PIN_NUM_CART_CSYNC, //3
			PIN_NUM_CART_S2, //4
			PIN_NUM_CART_S4, //5
			PIN_NUM_CART_S1, //6
			PIN_NUM_CART_S5, //7
			PIN_NUM_CART_DCLK, //8
			PIN_NUM_CART_S3, //9
			PIN_NUM_CART_F3, //10
			PIN_NUM_CART_F5, //11
			-1, -1, -1, -1 //12-15 - unused
		},
		.bits=I2S_PARALLEL_BITS_16,
		.clkspeed_hz=1000000, //1MHz //3333333, //3.3MHz
		.bufsz=WAVEFORM_DMALEN*sizeof(uint16_t),
		.refill_cb=printspider_buffer_filler_fn,
		.refill_cb_arg=nozdata_queue
	};

    ESP_LOGI(TAG, "Setting up parallel I2S bus at I2S%d", 1);
    i2s_parallel_setup(&I2S1, &i2scfg);
	i2s_parallel_start(&I2S1);

    select_waveform();

    return ESP_OK;
}

//Returns a pixel from the selected channel of the selected coordinate of an embedded image.
//Returns white when out of bounds in the Y direction; image wraps around in the X direction.
//Color=0 returns red, color=1 returns green, color=3 returns the blue pixel data (0-255).
//Note that this function wraps X around, so the text repeats.
uint8_t image_get_pixel(int x, int y, int color) {
	x=x%image_width;
	if (x<0) x+=image_width;
	if (y<0 || y>=image_height) return 0xFF;
	//Nozzles are bottom-first, picture is top-first. Compensate.
	y=image_height-y-1;
    // ESP_LOGI(TAG, "image_get_pixel image%p", image);
    
	const uint8_t *p=&image[(y*image_width+x)*3];

	return p[color];
}

void send_image_row_color(int pos) {
	uint8_t nozdata[PRINTSPIDER_NOZDATA_SZ];
	memset(nozdata, 0, PRINTSPIDER_NOZDATA_SZ);
	for (int c=0; c<3; c++) {
		for (int y=0; y<84; y++) {
			uint8_t v=image_get_pixel(pos-c*PRINTSPIDER_CMY_ROW_OFFSET, y*2, c);
			//Note the v returned is 0 for black, 255 for the color. We need to invert that here as we're printing on
			//white.
			v=255-v;
			//Random-dither. The chance of the nozzle firing is equal to (v/256).
			if (v>(rand()&255)) {
				//Note: The actual nozzles for the color cart start around y=14
				printspider_fire_nozzle_color(nozdata, y+14, c);
			}
		}
	}
	//Send nozzle data to queue so ISR can pick up on it.
	xQueueSend(nozdata_queue, nozdata, portMAX_DELAY);
}

void send_image_row_black(int pos) {
	uint8_t nozdata[PRINTSPIDER_NOZDATA_SZ];
	memset(nozdata, 0, PRINTSPIDER_NOZDATA_SZ);
	for (int row=0; row<2; row++) {
		for (int y=0; y<168; y++) {
			//We take anything but white in any color channel of the image to mean we want black there.
			if (image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 0)!=0xff ||
				image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 1)!=0xff ||
				image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 2)!=0xff) {
				//Random-dither 50%, as firing all nozzles is a bit hard on the power supply.
				if (rand()&1) {
					printspider_fire_nozzle_black(nozdata, y, row);
				}
			}
		}
	}
	//Send nozzle data to queue so ISR can pick up on it.
	xQueueSend(nozdata_queue, nozdata, portMAX_DELAY);
}

/* An HTTP GET handler */
static esp_err_t print_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Printing image of length: %d, width: %d, height: %d", image_len, image_width, image_height);

    const char* print_html_str = "Printing...<br><br><a href='/main'>Back to main page</a>";
    httpd_resp_send(req, print_html_str, HTTPD_RESP_USE_STRLEN);

    int pos = 0;
    while (pos < image_width * 2) {
        if (image_color) {
            send_image_row_color(pos/2);
        } else {
            send_image_row_black(pos/2);
        }
        pos++;
    }
    ESP_LOGI(TAG, "Final pos: %d", pos);

    return ESP_OK;
}

static const httpd_uri_t print_uri = {
    .uri       = "/print",
    .method    = HTTP_GET,
    .handler   = print_get_handler,
};

void start_webserver() {
    httpd_handle_t server = NULL;
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &conf));
    
    ESP_LOGI(TAG, "Registering URI: %s", main_uri.uri);
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &main_uri));

    ESP_LOGI(TAG, "Registering URI: %s", upload_uri.uri);
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &upload_uri));
    ESP_LOGI(TAG, "init_upload_callbacks");
    ESP_ERROR_CHECK(init_upload_callbacks());

    ESP_LOGI(TAG, "Registering URI: %s", print_uri.uri);
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &print_uri));
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "wifi_init_sta");
    wifi_init_sta();

    ESP_LOGI(TAG, "start_mdns_service");
    start_mdns_service();

    ESP_LOGI(TAG, "netbiosns_init");
    netbiosns_init();
    netbiosns_set_name(MDNS_HOSTNAME);

    ESP_LOGI(TAG, "start_webserver");
    start_webserver();

    ESP_LOGI(TAG, "init_printing");
    init_printing();
}
