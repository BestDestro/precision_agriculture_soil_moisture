#pragma once

#include "esp_err.h"

esp_err_t humidity_sensor_init();
esp_err_t humidity_sensor_read(float* humidity_out, float* temperature_out = nullptr);
