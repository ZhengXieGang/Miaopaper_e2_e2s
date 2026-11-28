#pragma once
/* ESP-IDF SPI 主机驱动桩头（供本机 EPD 驱动协议回归测试使用）。 */
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_err.h"

typedef int spi_host_device_t;

#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3
#define SPI_DEVICE_3WIRE (1 << 2)
#define SPI_DEVICE_HALFDUPLEX (1 << 4)

typedef struct spi_device_t *spi_device_handle_t;

typedef struct spi_transaction_t {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t length;
    size_t rxlength;
    void *user;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;

typedef void (*transaction_cb_t)(spi_transaction_t *trans);

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    uint32_t flags;
    int clock_speed_hz;
    uint8_t mode;
    int spics_io_num;
    int queue_size;
    transaction_cb_t pre_cb;
} spi_device_interface_config_t;

esp_err_t spi_bus_initialize(spi_host_device_t host_id,
                             const spi_bus_config_t *bus_config,
                             int dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);
esp_err_t spi_bus_add_device(spi_host_device_t host_id,
                             const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
                                      spi_transaction_t *trans_desc);
