#include "humidity_model.hpp"

#include "esp_log.h"
#include "model_data.h"
#include "normalization.hpp"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char* TAG = "HUM_MODEL";

namespace {
const tflite::Model* model = nullptr;
static tflite::MicroMutableOpResolver<1> resolver;

constexpr size_t kTensorArenaSize = 24 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];

tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input_tensor = nullptr;
TfLiteTensor* output_tensor = nullptr;
bool initialized = false;
bool resolver_ready = false;

constexpr int kTfliteSchemaVersion = 3;
constexpr int kFrequencyCount = 9;

float normalize_x(float x, int idx) {
    return (x - kXMean[idx]) / kXStd[idx];
}

float denormalize_y(float y) {
    return y * kYStd + kYMean;
}
}

esp_err_t humidity_model_init() {
    model = tflite::GetModel(g_humedad_model);

    if (model->version() != kTfliteSchemaVersion) {
        MicroPrintf("Model schema version %d != supported %d",
                    model->version(), kTfliteSchemaVersion);
        return ESP_FAIL;
    }

    if (!resolver_ready) {
        if (resolver.AddFullyConnected() != kTfLiteOk) {
            ESP_LOGE(TAG, "No se pudo registrar FullyConnected");
            return ESP_FAIL;
        }
        resolver_ready = true;
    }

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);

    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors fallo; sube kTensorArenaSize");
        return ESP_FAIL;
    }

    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);

    if (!input_tensor || !output_tensor) {
        ESP_LOGE(TAG, "No se pudieron obtener tensores");
        return ESP_FAIL;
    }

    if (input_tensor->type != kTfLiteFloat32 || output_tensor->type != kTfLiteFloat32) {
        ESP_LOGE(TAG, "Este ejemplo espera modelo float32");
        return ESP_FAIL;
    }

    if (input_tensor->dims == nullptr || input_tensor->dims->size < 2 ||
        input_tensor->dims->data[1] != kInputSize) {
        ESP_LOGE(TAG, "Entrada inesperada del modelo: se esperaban %d features", kInputSize);
        return ESP_FAIL;
    }

    initialized = true;
    ESP_LOGI(TAG, "Modelo inicializado (%d features)", kInputSize);
    return ESP_OK;
}

esp_err_t humidity_model_predict(const std::array<float, 9>& input_v,
                                 const std::array<float, 9>& input_std_v,
                                 float* humidity_out) {
    if (!initialized || !humidity_out) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < kFrequencyCount; ++i) {
        input_tensor->data.f[i] = normalize_x(input_v[i], i);
        input_tensor->data.f[kFrequencyCount + i] =
            normalize_x(input_std_v[i], kFrequencyCount + i);
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke fallo");
        return ESP_FAIL;
    }

    float y_norm = output_tensor->data.f[0];
    *humidity_out = denormalize_y(y_norm);
    return ESP_OK;
}