#include "humidity_sensor.hpp"

extern "C" {
#include "driver/uart.h"
#include "esp_modbus_common.h"
#include "esp_modbus_master.h"
}

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "HUM_SENSOR";

static constexpr uart_port_t MODBUS_UART_NUM = UART_NUM_2;
static constexpr int MODBUS_RX_PIN = 16;
static constexpr int MODBUS_TX_PIN = 17;
static constexpr int MODBUS_RTS_PIN = 4;
static constexpr int MODBUS_BAUDRATE = 9600;
static constexpr uint8_t MODBUS_SLAVE_ADDR = 1;

namespace {

enum {
    CID_HUMIDITY = 0,
    CID_TEMPERATURE = 1,
};

const mb_parameter_descriptor_t kMbParams[] = {
    {
        .cid = CID_HUMIDITY,
        .param_key = "humidity",
        .param_units = "%",
        .mb_slave_addr = MODBUS_SLAVE_ADDR,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start = 0x0000,
        .mb_size = 1,
        .param_offset = 0,
        .param_type = PARAM_TYPE_U16,
        .param_size = PARAM_SIZE_U16,
        .param_opts = {{0}},
        .access = PAR_PERMS_READ,
    },
    {
        .cid = CID_TEMPERATURE,
        .param_key = "temperature",
        .param_units = "C",
        .mb_slave_addr = MODBUS_SLAVE_ADDR,
        .mb_param_type = MB_PARAM_HOLDING,
        .mb_reg_start = 0x0001,
        .mb_size = 1,
        .param_offset = 0,
        .param_type = PARAM_TYPE_U16,
        .param_size = PARAM_SIZE_U16,
        .param_opts = {{0}},
        .access = PAR_PERMS_READ,
    },
};

void* s_mb_handler = nullptr;
bool s_initialized = false;

esp_err_t read_register_with_retry(uint16_t cid,
                                   const char* key,
                                   uint16_t* value_out,
                                   uint8_t* type_out) {
    esp_err_t err = mbc_master_get_parameter(cid,
                                             const_cast<char*>(key),
                                             reinterpret_cast<uint8_t*>(value_out),
                                             type_out);
    if (err == ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(50));
        err = mbc_master_get_parameter(cid,
                                       const_cast<char*>(key),
                                       reinterpret_cast<uint8_t*>(value_out),
                                       type_out);
    }
    return err;
}

} 

esp_err_t humidity_sensor_init() {
    if (s_initialized) {
        return ESP_OK;
    }

    esp_log_level_set("MB_PORT_COMMON", ESP_LOG_WARN);
    esp_log_level_set("MB_CONTROLLER_MASTER", ESP_LOG_WARN);

    ESP_RETURN_ON_ERROR(mbc_master_init(MB_PORT_SERIAL_MASTER, &s_mb_handler),
                        TAG,
                        "mbc_master_init fallo");

    mb_communication_info_t comm = {};
    comm.port = MODBUS_UART_NUM;
    comm.mode = MB_MODE_RTU;
    comm.baudrate = MODBUS_BAUDRATE;
    comm.parity = MB_PARITY_NONE;

    ESP_RETURN_ON_ERROR(mbc_master_setup(&comm), TAG, "mbc_master_setup fallo");
    ESP_RETURN_ON_ERROR(
        mbc_master_set_descriptor(kMbParams, sizeof(kMbParams) / sizeof(kMbParams[0])),
        TAG,
        "mbc_master_set_descriptor fallo");
    ESP_RETURN_ON_ERROR(mbc_master_start(), TAG, "mbc_master_start fallo");

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = MODBUS_BAUDRATE;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_param_config(MODBUS_UART_NUM, &uart_cfg),
                        TAG,
                        "uart_param_config fallo");
    ESP_RETURN_ON_ERROR(uart_set_pin(MODBUS_UART_NUM,
                                     MODBUS_TX_PIN,
                                     MODBUS_RX_PIN,
                                     MODBUS_RTS_PIN,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin fallo");
    ESP_RETURN_ON_ERROR(uart_set_mode(MODBUS_UART_NUM, UART_MODE_RS485_HALF_DUPLEX),
                        TAG,
                        "uart_set_mode fallo");

    uart_flush_input(MODBUS_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_initialized = true;
    ESP_LOGI(TAG, "Sensor humedad listo por Modbus RTU (UART2, RS485)");
    return ESP_OK;
}

esp_err_t humidity_sensor_read(float* humidity_out, float* temperature_out) {
    if (!s_initialized || humidity_out == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t type = 0;
    uint16_t hum_raw = 0;
    ESP_RETURN_ON_ERROR(read_register_with_retry(CID_HUMIDITY, "humidity", &hum_raw, &type),
                        TAG,
                        "No se pudo leer humedad");

    *humidity_out = static_cast<float>(static_cast<int16_t>(hum_raw)) / 10.0f;

    if (temperature_out != nullptr) {
        uint16_t temp_raw = 0;
        ESP_RETURN_ON_ERROR(read_register_with_retry(CID_TEMPERATURE,
                                                     "temperature",
                                                     &temp_raw,
                                                     &type),
                            TAG,
                            "No se pudo leer temperatura");
        *temperature_out = static_cast<float>(static_cast<int16_t>(temp_raw)) / 10.0f;
    }

    return ESP_OK;
}
