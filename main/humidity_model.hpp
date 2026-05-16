#pragma once

#include <array>
#include "esp_err.h"

esp_err_t humidity_model_init();
esp_err_t humidity_model_predict(const std::array<float, 9>& input_v,
                                 const std::array<float, 9>& input_std_v,
                                 float* humidity_out);