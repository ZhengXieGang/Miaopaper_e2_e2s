# main — boot wiring, app state machine, MP UI and the Miaopaper-compatible
# storage/BLE layers (PORT_SPEC §10.1/§10.5).
#
# Every source is listed explicitly: a missing file must fail at configure or
# link time instead of silently dropping a module (docs/research/platform.md
# §2.2 P0-1 was exactly that: mp_store.c/mp_font.c never got compiled and the
# link failed with "undefined reference to 'app_main'").
idf_component_register(
    SRCS "main.c"
         "mp_app.c"
         "mp_ui.c"
         "mp_hints.c"
         "mp_page.c"
         "mp_ble.c"
         "mp_store.c"
         "mp_font.c"
         "mp_backlight.c"
         "mp_ota.c"
         "mp_toc.c"
    INCLUDE_DIRS "."
    # Public: the mp_* headers are included across the modules and by main.c,
    # and these components' headers appear in main's public surface
    # (reader_cfg.h, display.h/gfx.h, text.h, storage/input/power headers,
    # recovery.h for mp_app.c).
    REQUIRES common text display storage input power recovery
    # Private, implementation only:
    #   bt    — NimBLE host headers, used inside mp_ble.c (mp_ble.h is pure C,
    #           PORT_SPEC §7.1: never REQUIRES the reader "ble" component);
    #   fatfs — esp_vfs_fat_info("/sdcard") for mp_store_space();
    #   vfs   — VFS mount path used by mp_store.c (PORT_SPEC §6.5).
    PRIV_REQUIRES bt fatfs vfs esp_driver_i2c esp_driver_gpio esp_hw_support esp_pm esp_phy
                  app_update spi_flash bootloader_support esp_app_format mbedtls
                  esp_rom esp_system efuse esp_hal_wdt)
