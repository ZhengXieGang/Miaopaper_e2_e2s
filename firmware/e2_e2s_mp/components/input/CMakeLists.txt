# input — 5-key polling, debounce, queue policy (DESIGN.md §7)
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "include"
                       REQUIRES common
                       PRIV_REQUIRES esp_driver_gpio esp_timer esp_hw_support)
