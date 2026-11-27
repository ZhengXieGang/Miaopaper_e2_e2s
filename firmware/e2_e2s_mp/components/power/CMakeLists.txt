# power — battery ADC and low-battery policy (DESIGN.md §10)
# IDF 6.1.1 removed esp_adc_cal; use esp_adc (oneshot + calibration).
idf_component_register(SRCS "power_battery.c" "power_filter.c" "power_policy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common
                       PRIV_REQUIRES esp_adc esp_driver_gpio esp_timer esp_pm esp_hw_support)
