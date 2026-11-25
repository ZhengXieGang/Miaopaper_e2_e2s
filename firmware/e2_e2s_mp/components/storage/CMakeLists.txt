# storage — SD/FAT, library, progress, bookmarks, upload staging (DESIGN §6)
# IDF 6.1.1: esp_vfs_fat.h is in fatfs, sdmmc_cmd.h in sdmmc,
# driver/sdmmc_host.h in esp_driver_sdmmc.
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "include"
                       REQUIRES common
                       PRIV_REQUIRES fatfs esp_driver_sdmmc sdmmc esp_driver_gpio vfs esp_pm)
