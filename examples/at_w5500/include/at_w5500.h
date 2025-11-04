#pragma once

#include "esp_at_core.h"
#include "esp_at.h"

//! \brief Initialize the W5500 chip & attach it to netif
esp_err_t w5500_init(void);