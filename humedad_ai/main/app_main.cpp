#include <array>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "frequency_sweep.hpp"
#include "humidity_model.hpp"
#include "humidity_sensor.hpp"

static const char* TAG = "APP";

extern "C" void app_main(void) {
    ESP_ERROR_CHECK(frequency_sweep_init());
    ESP_ERROR_CHECK(humidity_model_init());
    ESP_ERROR_CHECK(humidity_sensor_init());

    while (true) {
        std::array<float, 9> input_v{};
        std::array<float, 9> input_std_v{};
        std::array<int, 9> input_raw{};
        float predicted_humidity = 0.0f;
        float sensor_humidity = 0.0f;
        float sensor_temperature = 0.0f;

        if (frequency_sweep_read_vector(input_v, input_std_v, input_raw) == ESP_OK) {
            ESP_LOGI(TAG,
                     "Vector V = [%.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f] V",
                     input_v[0], input_v[1], input_v[2], input_v[3], input_v[4],
                     input_v[5], input_v[6], input_v[7], input_v[8]);

            ESP_LOGI(TAG,
                     "Vector STD = [%.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f] V",
                     input_std_v[0], input_std_v[1], input_std_v[2], input_std_v[3], input_std_v[4],
                     input_std_v[5], input_std_v[6], input_std_v[7], input_std_v[8]);

            ESP_LOGI(TAG,
                     "Vector RAW = [%d, %d, %d, %d, %d, %d, %d, %d, %d]",
                     input_raw[0], input_raw[1], input_raw[2], input_raw[3], input_raw[4],
                     input_raw[5], input_raw[6], input_raw[7], input_raw[8]);

            if (humidity_model_predict(input_v, input_std_v, &predicted_humidity) == ESP_OK) {
                ESP_LOGI(TAG, "Humedad predicha = %.2f %%", predicted_humidity);

                if (humidity_sensor_read(&sensor_humidity, &sensor_temperature) == ESP_OK) {
                    const float diff_abs = std::fabs(predicted_humidity - sensor_humidity);
                    ESP_LOGI(TAG,
                             "Sensor -> humedad = %.2f %% | temperatura = %.2f C",
                             sensor_humidity,
                             sensor_temperature);
                    ESP_LOGI(TAG,
                             "Comparacion -> predicha = %.2f %% | sensor = %.2f %% | error_abs = %.2f puntos",
                             predicted_humidity,
                             sensor_humidity,
                             diff_abs);
                } else {
                    ESP_LOGW(TAG, "No se pudo leer la humedad del sensor para comparar");
                }
            } else {
                ESP_LOGE(TAG, "Fallo en inferencia");
            }
        } else {
            ESP_LOGE(TAG, "Fallo en barrido de frecuencias");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
