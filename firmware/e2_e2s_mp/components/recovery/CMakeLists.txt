# recovery — 恢复原厂启动槽(otadata 唯一写入点)
idf_component_register(SRCS "recovery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update bootloader_support efuse esp_app_format esp_hw_support spi_flash)
