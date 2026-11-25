# e2s_epd — SSD1680/UC8151 panel driver, copied from firmware/badapple/main.
# IDF 6.1.1 split the old "driver" aggregate: SPI and GPIO live in
# esp_driver_spi / esp_driver_gpio (see e2s_re/components/epd/CMakeLists.txt).
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_timer)
