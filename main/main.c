#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

static const char *TAG = "Dimmer";

#define clk_pin     GPIO_NUM_2
#define dt_pin      GPIO_NUM_1
#define zerocross   GPIO_NUM_7
#define alpha       GPIO_NUM_11
#define ESP_INTR_FLAG_DEFAULT 0

// ── Control de modo ──────────────────────────────────────────────
// false = Encoder manda | true = Web manda
volatile bool web_mode = false;

volatile int selected_index = 0;
int delay_len = 37;

static const uint32_t delays[37] = {
    40,   231,  463,  694,  926,
    1157, 1389, 1620, 1852, 2083,
    2315, 2546, 2778, 3009, 3241,
    3472, 3704, 3935, 4167, 4398,
    4630, 4861, 5093, 5324, 5556,
    5787, 6019, 6250, 6481, 6713,
    6944, 7176, 7407, 7639, 7870, 8000, 8220
};

static gptimer_handle_t timer;
static httpd_handle_t server = NULL;

// ── Forward declarations ─────────────────────────────────────────
static void WiFi_STA_Initialization(void);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
esp_err_t FileSystemInit(void);
static httpd_handle_t start_webserver(void);

// ── ISR: Cruce por Cero ──────────────────────────────────────────
static void IRAM_ATTR gpio_isr_handler(void *arg) {
    if (selected_index==0){
    gpio_set_level(alpha, 1);
        
    }
    else if (selected_index == 36){
        gpio_set_level(alpha, 0);
    }
    else{

    
    gptimer_alarm_config_t alarma = {
        .alarm_count = delays[selected_index]-60,
        .flags.auto_reload_on_alarm = false
    };
    gptimer_set_alarm_action(timer, &alarma);
    gptimer_set_raw_count(timer, 0);
    gptimer_start(timer);
}
}

// ── ISR: Timer → disparo TRIAC ───────────────────────────────────
static bool IRAM_ATTR alpha_callback(gptimer_handle_t tmr,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_data) {
    gptimer_stop(tmr);
    gpio_set_level(alpha, 1);
    esp_rom_delay_us(40);
    gpio_set_level(alpha, 0);
    return false;
}

// ── GPIO ─────────────────────────────────────────────────────────
void setup_gpios(void) {
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << zerocross) | (1ULL << clk_pin) | (1ULL << dt_pin),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE
    };
    gpio_config(&io_conf);

    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << alpha);
    gpio_config(&io_conf);
    gpio_set_level(alpha, 0);
}

// ── Timer ────────────────────────────────────────────────────────
void setup_timer(void) {
    gptimer_config_t timer_cfg = {
        .clk_src      = GPTIMER_CLK_SRC_DEFAULT,
        .direction    = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000
    };
    gptimer_new_timer(&timer_cfg, &timer);

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = delays[0],
        .flags.auto_reload_on_alarm = false
    };
    gptimer_set_alarm_action(timer, &alarm_cfg);

    gptimer_event_callbacks_t cbs = { .on_alarm = alpha_callback };
    gptimer_register_event_callbacks(timer, &cbs, NULL);
    gptimer_enable(timer);
}

// ── ISR del cruce por cero ───────────────────────────────────────
void setup_isr(void) {
    gpio_set_direction(zerocross, GPIO_MODE_INPUT);
    gpio_set_intr_type(zerocross, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(zerocross, gpio_isr_handler, NULL);
}

// ── Tarea Encoder ────────────────────────────────────────────────
void Selector_Task(void *pvParameters) {
    int last_clk = gpio_get_level(clk_pin);
    while (1) {
        int current_clk = gpio_get_level(clk_pin);
        if (current_clk != last_clk && current_clk == 1) {
            // Solo actúa si estamos en modo encoder
            if (!web_mode) {
                int current_dt = gpio_get_level(dt_pin);
                if (current_dt != current_clk) {
                    if (selected_index < delay_len - 1) selected_index++;
                } else {
                    if (selected_index > 0) selected_index--;
                }
            }
            // Si web_mode == true, simplemente ignoramos el giro
        }
        last_clk = current_clk;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── HTTP Handlers ────────────────────────────────────────────────

// GET /set-angle?val=0..36
static esp_err_t set_angle_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");    
    httpd_resp_set_type(req, "text/plain");                     
    if (!web_mode) {
        // Rechazar si no estamos en modo web
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Modo encoder activo. Activa modo web primero.");
        return ESP_OK;
    }

    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val_str[8];
        if (httpd_query_key_value(buf, "val", val_str, sizeof(val_str)) == ESP_OK) {
            int val = atoi(val_str);
            if (val >= 0 && val <= 36) {
                selected_index = val;
                ESP_LOGI(TAG, "[WEB] Ángulo → index=%d | delay=%lu us", val, delays[val]);
                httpd_resp_sendstr(req, "OK");
                return ESP_OK;
            }
        }
    }
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "Valor inválido. Rango: 0–36");
    return ESP_OK;
}

// GET /set-mode?mode=web  o  /set-mode?mode=encoder
static esp_err_t set_mode_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Content-Type", "text/plain");   // ← agrega
    httpd_resp_set_type(req, "text/plain");                     
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char mode_str[16];
        if (httpd_query_key_value(buf, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
            if (strcmp(mode_str, "web") == 0) {
                web_mode = true;
                ESP_LOGI(TAG, "Modo cambiado → WEB");
                httpd_resp_sendstr(req, "web");
                return ESP_OK;
            } else if (strcmp(mode_str, "encoder") == 0) {
                web_mode = false;
                ESP_LOGI(TAG, "Modo cambiado → ENCODER");
                httpd_resp_sendstr(req, "encoder");
                return ESP_OK;
            }
        }
    }
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "Modo inválido. Usa: web | encoder");
    return ESP_OK;
}

// GET /get-mode  → responde "web" o "encoder" (para que la UI sincronice al cargar)
static esp_err_t get_mode_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");  // ← agrega
    httpd_resp_set_type(req, "text/plain");                     
    httpd_resp_sendstr(req, web_mode ? "web" : "encoder");
    return ESP_OK;
}

// GET /  → sirve el HTML desde SPIFFS
static esp_err_t root_handler(httpd_req_t *req) {
    FILE *f = fopen("/Datos/Dimmer.html", "r");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "index.html no encontrado en SPIFFS");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // Fin
    return ESP_OK;
}

// ── Arrancar servidor HTTP ────────────────────────────────────────
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error al iniciar servidor HTTP");
        return NULL;
    }

    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler
    };
    httpd_uri_t uri_angle = {
        .uri = "/set-angle", .method = HTTP_GET, .handler = set_angle_handler
    };
    httpd_uri_t uri_set_mode = {
        .uri = "/set-mode", .method = HTTP_GET, .handler = set_mode_handler
    };
    httpd_uri_t uri_get_mode = {
        .uri = "/get-mode", .method = HTTP_GET, .handler = get_mode_handler
    };

    httpd_register_uri_handler(srv, &uri_root);
    httpd_register_uri_handler(srv, &uri_angle);
    httpd_register_uri_handler(srv, &uri_set_mode);
    httpd_register_uri_handler(srv, &uri_get_mode);

    ESP_LOGI(TAG, "Servidor HTTP iniciado.");
    return srv;
}

// ── SPIFFS ───────────────────────────────────────────────────────
esp_err_t FileSystemInit(void) {
    ESP_LOGI(TAG, "Montando SPIFFS...");
    esp_vfs_spiffs_conf_t DiskConf = {
        .base_path = "/Datos",
        .partition_label = "Datos",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t result = esp_vfs_spiffs_register(&DiskConf);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Error al montar SPIFFS: %s", esp_err_to_name(result));
        return result;
    }
    size_t total, used;
    if (esp_spiffs_info(DiskConf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%d, usado=%d, libre=%d", total, used, total - used);
    }
    return ESP_OK;
}

// ── WiFi ─────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Desconectado, reintentando...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        // Arrancar servidor solo cuando tenemos IP
        if (server == NULL) {
            server = start_webserver();
        }
    }
}

static void WiFi_STA_Initialization(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = "POCOleo",
            .password = "chochocrusher"
        }
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// ── app_main ─────────────────────────────────────────────────────
void app_main(void) {
    nvs_flash_init();
    setup_gpios();
    setup_timer();
    setup_isr();
    FileSystemInit();
    WiFi_STA_Initialization();

    xTaskCreate(Selector_Task, "Encoder", 2048, NULL, 5, NULL);

    int last_printed_index = -1;
    while (1) {
        int current_index = selected_index;
        if (current_index != last_printed_index) {
            ESP_LOGI(TAG, "[%s] Índice=%d | Ángulo=%d° | Delay=%lu us",
                     web_mode ? "WEB" : "ENC",
                     current_index,
                     current_index * 5,
                     delays[current_index]);
            last_printed_index = current_index;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}