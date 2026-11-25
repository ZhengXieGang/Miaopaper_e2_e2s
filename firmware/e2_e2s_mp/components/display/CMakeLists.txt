# display — logical frame, refresh policy, GC (DESIGN.md §4/§17.1)
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES common
                       PRIV_REQUIRES e2s_epd esp_driver_gpio esp_timer esp_pm)
