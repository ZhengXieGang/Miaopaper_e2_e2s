#pragma once
/* ESP32-S3 ROM CPU 复位桩头。 */
#include <stdint.h>

void esp_rom_software_reset_cpu(int cpu_no);
