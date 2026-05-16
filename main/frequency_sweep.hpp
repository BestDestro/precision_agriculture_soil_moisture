#pragma once

#include <array>
#include "esp_err.h"

// Inicializa SI5351 + ADC
esp_err_t frequency_sweep_init();

// Barre [128, 64, 32, 16, 8, 4, 2, 1, 0.5] MHz y devuelve:
// - tensiones medias en voltios
// - desviaciones estándar en voltios
// - medias raw del ADC
esp_err_t frequency_sweep_read_vector(std::array<float, 9>& out_v,
                                      std::array<float, 9>& out_std_v,
                                      std::array<int, 9>& out_raw);