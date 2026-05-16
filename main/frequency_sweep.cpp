#include "frequency_sweep.hpp"

#include <inttypes.h>
#include <array>
#include <cmath>
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

extern "C" {
#include "si5351.h"
}

static const char* TAG = "FREQ_SWEEP";

static constexpr gpio_num_t I2C_SDA = GPIO_NUM_21;
static constexpr gpio_num_t I2C_SCL = GPIO_NUM_22;
static constexpr adc_channel_t ADC_CH = ADC_CHANNEL_6; // GPIO34

static constexpr int SETTLE_MS = 1500;

static constexpr int ADC_SAMPLES_TOTAL = 100;
static constexpr int ADC_GAP_FIXED_MS = 50;
static constexpr int ADC_GAP_RANDOM_MIN_MS = 1;
static constexpr int ADC_GAP_RANDOM_MAX_MS = 20;

// Frecuencias
static constexpr int kFrequencyCount = 9;

static const uint64_t kFrequenciesHz[kFrequencyCount] = {
    128000000ULL,   // 128 MHz
    64000000ULL,    // 64 MHz
    32000000ULL,    // 32 MHz
    16000000ULL,    // 16 MHz
    8000000ULL,     // 8 MHz
    4000000ULL,     // 4 MHz
    2000000ULL,     // 2 MHz
    1000000ULL,     // 1 MHz
    500000ULL       // 500 kHz
};

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;
static bool s_cali_ok = false;
static si5351_t s_dev = {};

static constexpr bool SHOW_RAW_LOG = true;
static constexpr bool SHOW_VOLTAGE_LOG = true;
static constexpr bool SHOW_STD_LOG = true;

struct SampleResult {
    int raw_avg;
    float voltage_v;
    float voltage_std_v;
};

static int random_gap_ms() {
    const uint32_t span = ADC_GAP_RANDOM_MAX_MS - ADC_GAP_RANDOM_MIN_MS + 1;
    return ADC_GAP_RANDOM_MIN_MS + (esp_random() % span);
}


static float raw_to_voltage_float(int raw) {
    if (!s_cali_ok) {
        ESP_LOGE(TAG, "Calibracion ADC no disponible");
        return 0.0f;
    }

    if (raw < 0) {
        raw = 0;
    } else if (raw > 4095) {
        raw = 4095;
    }

    int mv = 0;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_cali, raw, &mv));
    return static_cast<float>(mv) / 1000.0f;
}

static SampleResult read_sample() {
    std::array<float, ADC_SAMPLES_TOTAL> voltage_samples{};
    int64_t raw_sum = 0;

    for (int i = 0; i < ADC_SAMPLES_TOTAL; ++i) {
        int raw = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, ADC_CH, &raw));
        raw_sum += raw;
        voltage_samples[i] = raw_to_voltage_float(raw);

        if (i < ADC_SAMPLES_TOTAL - 1) {
            const int delay_ms = ADC_GAP_FIXED_MS + random_gap_ms();
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    const float raw_avg_f = static_cast<float>(raw_sum) / static_cast<float>(ADC_SAMPLES_TOTAL);
    const int raw_avg_i = static_cast<int>(raw_avg_f);

    float voltage_sum = 0.0f;
    for (float sample_v : voltage_samples) {
        voltage_sum += sample_v;
    }
    const float voltage_avg = voltage_sum / static_cast<float>(ADC_SAMPLES_TOTAL);

    float variance = 0.0f;
    for (float sample_v : voltage_samples) {
        const float diff = sample_v - voltage_avg;
        variance += diff * diff;
    }
    variance /= static_cast<float>(ADC_SAMPLES_TOTAL); // std poblacional

    SampleResult result;
    result.raw_avg = raw_avg_i;
    result.voltage_v = voltage_avg;
    result.voltage_std_v = std::sqrt(variance);
    return result;
}

esp_err_t frequency_sweep_init() {
    const int8_t ok = si5351_i2c_init(&s_dev, I2C_NUM_0, I2C_SDA, I2C_SCL, 100000);
    if (ok != 0) {
        ESP_LOGE(TAG, "si5351_i2c_init fallo (%d)", ok);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(si5351_init(&s_dev, SI5351_CRYSTAL_LOAD_8PF, 0, 0));
    si5351_output_enable(&s_dev, SI5351_CLK2, true);

    adc_oneshot_unit_init_cfg_t init_cfg{};
    init_cfg.unit_id = ADC_UNIT_1;
    init_cfg.clk_src = ADC_RTC_CLK_SRC_DEFAULT;
    init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg{};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CH, &chan_cfg));

    adc_cali_line_fitting_config_t cali_cfg{};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    cali_cfg.default_vref = 0;

    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali_ok = false;
        ESP_LOGE(TAG, "No se pudo activar calibracion ADC");
        return ESP_FAIL;
    }

    s_cali_ok = true;
    ESP_LOGI(TAG, "Calibracion ADC activa");

    return ESP_OK;
}

esp_err_t frequency_sweep_read_vector(std::array<float, kFrequencyCount>& out_v,
                                      std::array<float, kFrequencyCount>& out_std_v,
                                      std::array<int, kFrequencyCount>& out_raw) {
    for (int i = 0; i < kFrequencyCount; ++i) {
        const bool freq_ok = si5351_set_freq(&s_dev, kFrequenciesHz[i], SI5351_CLK2);
        if (!freq_ok) {
            ESP_LOGE(TAG, "set_freq fallo para %" PRIu64 " Hz", kFrequenciesHz[i]);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Frecuencia %.4f MHz, estabilizando %d ms...",
                 static_cast<double>(kFrequenciesHz[i]) / 1e6,
                 SETTLE_MS);

        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

        SampleResult sample = read_sample();

        out_v[i] = sample.voltage_v;
        out_std_v[i] = sample.voltage_std_v;
        out_raw[i] = sample.raw_avg;

        if (SHOW_RAW_LOG && SHOW_VOLTAGE_LOG && SHOW_STD_LOG) {
            ESP_LOGI(TAG, "  -> %.3f MHz = raw:%d | %.4f V | std:%.5f V",
                     static_cast<double>(kFrequenciesHz[i]) / 1e6,
                     sample.raw_avg,
                     sample.voltage_v,
                     sample.voltage_std_v);
        } else if (SHOW_RAW_LOG && SHOW_VOLTAGE_LOG) {
            ESP_LOGI(TAG, "  -> %.3f MHz = raw:%d | %.4f V",
                     static_cast<double>(kFrequenciesHz[i]) / 1e6,
                     sample.raw_avg,
                     sample.voltage_v);
        } else if (SHOW_RAW_LOG) {
            ESP_LOGI(TAG, "  -> %.3f MHz = raw:%d",
                     static_cast<double>(kFrequenciesHz[i]) / 1e6,
                     sample.raw_avg);
        } else if (SHOW_VOLTAGE_LOG && SHOW_STD_LOG) {
            ESP_LOGI(TAG, "  -> %.3f MHz = %.4f V | std:%.5f V",
                     static_cast<double>(kFrequenciesHz[i]) / 1e6,
                     sample.voltage_v,
                     sample.voltage_std_v);
        } else if (SHOW_VOLTAGE_LOG) {
            ESP_LOGI(TAG, "  -> %.3f MHz = %.4f V",
                     static_cast<double>(kFrequenciesHz[i]) / 1e6,
                     sample.voltage_v);
        }
    }

    return ESP_OK;
}