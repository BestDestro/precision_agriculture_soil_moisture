// ======================= INCLUDES (LIBRERÍAS) =======================

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "esp_http_client.h"
#include "cJSON.h"

#include "si5351.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "driver/uart.h"

#include "esp_modbus_common.h"
#include "esp_modbus_master.h"


// ======================= LOG TAG =======================

static const char *TAG = "ESP32";


// ======================= CONFIG WIFI / SERVIDOR =======================

#define WIFI_SSID   "**********"
#define WIFI_PASS   "**********"
#define SERVER_IP   "**********"

#define SERVER_PORT 8080
#define SERVER_PATH "/data"
#define SERVER_URL  "http://" SERVER_IP ":8080" SERVER_PATH


static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0


// ======================= WIFI EVENT HANDLER =======================

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi iniciado, conectando...");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi desconectado, reintentando...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


// ======================= WIFI INIT =======================

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}


// ======================= HARDWARE PINS =======================

#define I2C_SDA 21
#define I2C_SCL 22

#define ADC_GPIO 34
#define ADC_CH   ADC_CHANNEL_6

#define MODBUS_UART_NUM  UART_NUM_2
#define MODBUS_RX_PIN    16
#define MODBUS_TX_PIN    17
#define MODBUS_RTS_PIN   4
#define MODBUS_BAUDRATE  9600
#define MODBUS_SLAVE     1


// ======================= SWEEP CONFIG =======================

#define NUM_BARRIDOS 10
#define MEDIDAS 100

#define SETTLE_MS 500
#define ADC_DELAY_BASE_MS    20
#define ADC_DELAY_JITTER_MS  20

#define NUM_FRECUENCIAS 9

static const uint64_t frecuencias[NUM_FRECUENCIAS] = {
    128000000ULL,  // 128 MHz
    64000000ULL,   // 64 MHz
    32000000ULL,   // 32 MHz
    16000000ULL,   // 16 MHz
    8000000ULL,    // 8 MHz
    4000000ULL,    // 4 MHz
    2000000ULL,    // 2 MHz
    1000000ULL,    // 1 MHz
    500000ULL      // 0.5 MHz
};


// ======================= ADC =======================

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_ok = false;

typedef struct {
    double media_v;
    double var_v;
    double std_v;
    double media_raw;
    double var_raw;
    double std_raw;
    int min_raw;
    int max_raw;
} adc_stats_t;

static esp_err_t adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CH, &chan_cfg));

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 0
    };

    esp_err_t err = adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo activar calibracion ADC: %s", esp_err_to_name(err));
        adc_cali_ok = false;
        return err;
    }

    adc_cali_ok = true;
    ESP_LOGI(TAG, "Calibracion ADC activa");
    return ESP_OK;
}

static inline uint32_t random_delay_ms(uint32_t base_ms, uint32_t jitter_ms)
{
    if (jitter_ms == 0) {
        return base_ms;
    }
    return base_ms + (esp_random() % (jitter_ms + 1));
}

static esp_err_t raw_to_calibrated_voltage(int raw, double *voltage_out)
{
    if (!adc_cali_ok || adc_cali_handle == NULL || voltage_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int mv = 0;
    esp_err_t err = adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv);
    if (err != ESP_OK) {
        return err;
    }

    *voltage_out = (double)mv / 1000.0;
    return ESP_OK;
}

static esp_err_t medirVoltaje(adc_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    double suma_raw = 0.0;
    double suma2_raw = 0.0;
    double suma_v = 0.0;
    double suma2_v = 0.0;

    int min_raw = 4095;
    int max_raw = 0;

    for (int i = 0; i < MEDIDAS; i++) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CH, &raw));

        double voltage_v = 0.0;
        esp_err_t err = raw_to_calibrated_voltage(raw, &voltage_v);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc_cali_raw_to_voltage fallo: %s", esp_err_to_name(err));
            return err;
        }

        if (raw < min_raw) min_raw = raw;
        if (raw > max_raw) max_raw = raw;

        suma_raw += (double)raw;
        suma2_raw += ((double)raw * (double)raw);

        suma_v += voltage_v;
        suma2_v += (voltage_v * voltage_v);

        uint32_t delay_ms = random_delay_ms(ADC_DELAY_BASE_MS, ADC_DELAY_JITTER_MS);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    const double media_raw = suma_raw / (double)MEDIDAS;
    double var_raw = (suma2_raw / (double)MEDIDAS) - (media_raw * media_raw);
    if (var_raw < 0.0) var_raw = 0.0;

    const double std_raw = sqrt(var_raw);

    const double media_v = suma_v / (double)MEDIDAS;
    double var_v = (suma2_v / (double)MEDIDAS) - (media_v * media_v);
    if (var_v < 0.0) var_v = 0.0;

    const double std_v = sqrt(var_v);

    adc_stats_t stats = {
        .media_v = media_v,
        .var_v = var_v,
        .std_v = std_v,
        .media_raw = media_raw,
        .var_raw = var_raw,
        .std_raw = std_raw,
        .min_raw = min_raw,
        .max_raw = max_raw
    };

    *out = stats;
    return ESP_OK;
}


// ======================= MODBUS =======================

enum {
    CID_HUMIDITY = 0,
    CID_TEMPERATURE = 1,
};

static const mb_parameter_descriptor_t mb_params[] = {
    {
        .cid = CID_HUMIDITY,
        .param_key = "humidity",
        .param_units = "%",
        .mb_slave_addr = MODBUS_SLAVE,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start = 0x0000,
        .mb_size = 1,
        .param_offset = 0,
        .param_type = PARAM_TYPE_U16,
        .param_size = 2,
        .param_opts = {{0}},
        .access = PAR_PERMS_READ
    },
    {
        .cid = CID_TEMPERATURE,
        .param_key = "temperature",
        .param_units = "C",
        .mb_slave_addr = MODBUS_SLAVE,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start = 0x0001,
        .mb_size = 1,
        .param_offset = 0,
        .param_type = PARAM_TYPE_U16,
        .param_size = 2,
        .param_opts = {{0}},
        .access = PAR_PERMS_READ
    }
};

static void *mb_handler = NULL;

static float last_hum = 0.0f;
static float last_temp = 0.0f;


static esp_err_t modbus_master_init(void)
{
    esp_log_level_set("MB_PORT_COMMON", ESP_LOG_DEBUG);
    esp_log_level_set("MB_CONTROLLER_MASTER", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(mbc_master_init(MB_PORT_SERIAL_MASTER, &mb_handler));

    mb_communication_info_t comm = {0};
    comm.port = MODBUS_UART_NUM;
    comm.mode = MB_MODE_RTU;
    comm.baudrate = MODBUS_BAUDRATE;
    comm.parity = MB_PARITY_NONE;

    ESP_ERROR_CHECK(mbc_master_setup((void *)&comm));
    ESP_ERROR_CHECK(mbc_master_set_descriptor(
        mb_params, sizeof(mb_params) / sizeof(mb_params[0])));
    ESP_ERROR_CHECK(mbc_master_start());

    uart_config_t uart_cfg = {
        .baud_rate = MODBUS_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_NUM,
                                 MODBUS_TX_PIN, MODBUS_RX_PIN,
                                 MODBUS_RTS_PIN, UART_PIN_NO_CHANGE));

    esp_err_t e = uart_set_mode(MODBUS_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_mode fallo: %s", esp_err_to_name(e));
        return e;
    }

    uart_flush_input(MODBUS_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Modbus master listo (UART2 RS485 half-duplex)");
    return ESP_OK;
}


static esp_err_t medirTempHumedad(void)
{
    esp_err_t err;
    uint8_t type = 0;

    uint16_t hum_raw = 0;
    err = mbc_master_get_parameter(CID_HUMIDITY, (char*)"humidity", (uint8_t*)&hum_raw, &type);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(50));
            err = mbc_master_get_parameter(CID_HUMIDITY, (char*)"humidity", (uint8_t*)&hum_raw, &type);
        }
        if (err != ESP_OK) return err;
    }

    uint16_t temp_raw = 0;
    err = mbc_master_get_parameter(CID_TEMPERATURE, (char*)"temperature", (uint8_t*)&temp_raw, &type);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(50));
            err = mbc_master_get_parameter(CID_TEMPERATURE, (char*)"temperature", (uint8_t*)&temp_raw, &type);
        }
        if (err != ESP_OK) return err;
    }

    last_hum  = ((int16_t)hum_raw) / 10.0f;
    last_temp = ((int16_t)temp_raw) / 10.0f;

    return ESP_OK;
}


// ======================= HTTP SEND (POST JSON) =======================

static esp_err_t send_json_payload(const char *json_str)
{
    const int len = (int)strlen(json_str);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init fallo");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_open fallo: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, json_str, len);
    if (written != len) {
        ESP_LOGE(TAG, "http_write incompleto: %d/%d", written, len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char resp[128];
    int rlen = esp_http_client_read_response(client, resp, sizeof(resp) - 1);
    if (rlen > 0) {
        resp[rlen] = '\0';
        ESP_LOGI(TAG, "Respuesta server: %s", resp);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "JSON enviado OK (HTTP %d)", status);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return ESP_FAIL;
    }
}


// ======================= MAIN =======================

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init();
    ESP_LOGI(TAG, "Esperando conexión WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi conectado");

    si5351_t dev = {0};

    int8_t ok = si5351_i2c_init(&dev, I2C_NUM_0, I2C_SDA, I2C_SCL, 100000);
    if (ok != 0) {
        ESP_LOGE(TAG, "si5351_i2c_init fallo (%d)", ok);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_err_t err = si5351_init(&dev, SI5351_CRYSTAL_LOAD_8PF, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "si5351_init fallo: %s", esp_err_to_name(err));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    si5351_output_enable(&dev, SI5351_CLK2, true);
    ESP_LOGI(TAG, "Si5351 OK (CLK2 habilitado)");

    ESP_ERROR_CHECK(adc_init());
    ESP_ERROR_CHECK(modbus_master_init());

    double *voltajes = (double *)calloc(NUM_BARRIDOS * NUM_FRECUENCIAS, sizeof(double));
    double *desvios  = (double *)calloc(NUM_BARRIDOS * NUM_FRECUENCIAS, sizeof(double));
    float *temps     = (float *)calloc(NUM_BARRIDOS, sizeof(float));
    float *hums      = (float *)calloc(NUM_BARRIDOS, sizeof(float));

    if (!voltajes || !desvios || !temps || !hums) {
        ESP_LOGE(TAG, "No hay memoria para buffers (heap).");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (int b = 0; b < NUM_BARRIDOS; b++) {
        ESP_LOGI(TAG, "Barrido %d/%d", b + 1, NUM_BARRIDOS);

        for (int i = 0; i < NUM_FRECUENCIAS; i++) {
            bool freq_ok = si5351_set_freq(&dev, frecuencias[i], SI5351_CLK2);
            if (!freq_ok) ESP_LOGW(TAG, "set_freq fallo para %" PRIu64 " Hz", frecuencias[i]);

            if (frecuencias[i] >= 1000000ULL) {
                ESP_LOGI(TAG, "  -> %.3f MHz estabilizando (%d ms)...",
                         (double)frecuencias[i] / 1e6, SETTLE_MS);
            } else {
                ESP_LOGI(TAG, "  -> %.3f kHz estabilizando (%d ms)...",
                         (double)frecuencias[i] / 1e3, SETTLE_MS);
            }

            vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

            adc_stats_t s = {0};
            ESP_ERROR_CHECK(medirVoltaje(&s));

            voltajes[b * NUM_FRECUENCIAS + i] = s.media_v;
            desvios[b * NUM_FRECUENCIAS + i]  = s.std_v;

            if (frecuencias[i] >= 1000000ULL) {
                ESP_LOGI(TAG,
                        "  %.3f MHz -> media=%.6f V | var=%.9f | std=%.6f V | raw_media=%.2f | raw_var=%.2f | raw_std=%.2f | min=%d max=%d",
                        (double)frecuencias[i] / 1e6,
                        s.media_v,
                        s.var_v,
                        s.std_v,
                        s.media_raw,
                        s.var_raw,
                        s.std_raw,
                        s.min_raw,
                        s.max_raw);
            } else {
                ESP_LOGI(TAG,
                        "  %.3f kHz -> media=%.6f V | var=%.9f | std=%.6f V | raw_media=%.2f | raw_var=%.2f | raw_std=%.2f | min=%d max=%d",
                        (double)frecuencias[i] / 1e3,
                        s.media_v,
                        s.var_v,
                        s.std_v,
                        s.media_raw,
                        s.var_raw,
                        s.std_raw,
                        s.min_raw,
                        s.max_raw);
            }
        }

        if (medirTempHumedad() == ESP_OK) {
            temps[b] = last_temp;
            hums[b]  = last_hum;
            ESP_LOGI(TAG, "  Temp=%.1f C | Hum=%.1f %%",
                     (double)last_temp, (double)last_hum);
        } else {
            ESP_LOGW(TAG, "Modbus lectura fallo en barrido %d", b + 1);
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }

    si5351_output_enable(&dev, SI5351_CLK2, false);
    ESP_LOGI(TAG, "Barridos terminados. Generando JSON final...");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dispositivo", "ESP32");
    cJSON_AddNumberToObject(root, "barridos_totales", NUM_BARRIDOS);
    cJSON_AddNumberToObject(root, "muestras_por_frecuencia", MEDIDAS);

    cJSON *barridos_arr = cJSON_AddArrayToObject(root, "barridos");

    for (int b = 0; b < NUM_BARRIDOS; b++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", b + 1);

        cJSON_AddNumberToObject(obj, "128mhz",   voltajes[b * NUM_FRECUENCIAS + 0]);
        cJSON_AddNumberToObject(obj, "64mhz",    voltajes[b * NUM_FRECUENCIAS + 1]);
        cJSON_AddNumberToObject(obj, "32mhz",    voltajes[b * NUM_FRECUENCIAS + 2]);
        cJSON_AddNumberToObject(obj, "16mhz",    voltajes[b * NUM_FRECUENCIAS + 3]);
        cJSON_AddNumberToObject(obj, "8mhz",     voltajes[b * NUM_FRECUENCIAS + 4]);
        cJSON_AddNumberToObject(obj, "4mhz",     voltajes[b * NUM_FRECUENCIAS + 5]);
        cJSON_AddNumberToObject(obj, "2mhz",     voltajes[b * NUM_FRECUENCIAS + 6]);
        cJSON_AddNumberToObject(obj, "1mhz",     voltajes[b * NUM_FRECUENCIAS + 7]);
        cJSON_AddNumberToObject(obj, "500khz",   voltajes[b * NUM_FRECUENCIAS + 8]);

        cJSON_AddNumberToObject(obj, "std_128mhz",  desvios[b * NUM_FRECUENCIAS + 0]);
        cJSON_AddNumberToObject(obj, "std_64mhz",   desvios[b * NUM_FRECUENCIAS + 1]);
        cJSON_AddNumberToObject(obj, "std_32mhz",   desvios[b * NUM_FRECUENCIAS + 2]);
        cJSON_AddNumberToObject(obj, "std_16mhz",   desvios[b * NUM_FRECUENCIAS + 3]);
        cJSON_AddNumberToObject(obj, "std_8mhz",    desvios[b * NUM_FRECUENCIAS + 4]);
        cJSON_AddNumberToObject(obj, "std_4mhz",    desvios[b * NUM_FRECUENCIAS + 5]);
        cJSON_AddNumberToObject(obj, "std_2mhz",    desvios[b * NUM_FRECUENCIAS + 6]);
        cJSON_AddNumberToObject(obj, "std_1mhz",    desvios[b * NUM_FRECUENCIAS + 7]);
        cJSON_AddNumberToObject(obj, "std_500khz",  desvios[b * NUM_FRECUENCIAS + 8]);

        cJSON_AddNumberToObject(obj, "temp", temps[b]);
        cJSON_AddNumberToObject(obj, "humedad", hums[b]);

        cJSON_AddItemToArray(barridos_arr, obj);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Enviando JSON final a %s ...", SERVER_URL);
    send_json_payload(json_str);

    free(json_str);
    free(voltajes);
    free(desvios);
    free(temps);
    free(hums);

    ESP_LOGI(TAG, "Listo.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}