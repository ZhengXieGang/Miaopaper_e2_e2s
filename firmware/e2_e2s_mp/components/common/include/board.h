#pragma once
/* board.h — frozen board pin map (DESIGN.md §17.2, facts from INVENTORY §1/§3)
 *
 * Every value below is copied from the byte-exact dump / disassembly evidence
 * listed in docs/E2S_READER_INVENTORY.md. Do not "fix" a pin here without a
 * new measurement; the wrong ones (GPIO46 as EPD DC, GPIO11 as ADC) are
 * documented landmines.
 *
 * KEY_* roles are inferred: the inventory says no physical map exists
 * (INVENTORY §4.2/§8.6), so the assignment is GPIO-ascending only.
 */

/* EPD: CS=40, DC=39, RST=48, BUSY/type=47, MOSI=41, SCLK=42.
 * SPI3_HOST (host id 2), 3-wire half-duplex, mode 0, 10 MHz, manual CS.
 * Evidence: INVENTORY §3.1; badapple/main/epd.c:405-414. */
#define BOARD_EPD_PIN_CS 40
#define BOARD_EPD_PIN_DC 39
#define BOARD_EPD_PIN_RST 48
#define BOARD_EPD_PIN_BUSY 47
#define BOARD_EPD_PIN_MOSI 41
#define BOARD_EPD_PIN_SCLK 42
#define BOARD_EPD_SPI_HOST 2            /* SPI3_HOST */

/* SD: SDMMC 4-bit, slot 1, internal pull-ups.
 * Evidence: INVENTORY §3.4; GPIO34 low pulse 50 ms before mount. */
#define BOARD_SD_PIN_CLK 11
#define BOARD_SD_PIN_CMD 12
#define BOARD_SD_PIN_D0 13
#define BOARD_SD_PIN_D1 14
#define BOARD_SD_PIN_D2 9
#define BOARD_SD_PIN_D3 10
#define BOARD_SD_WIDTH 4
#define BOARD_SD_PWR_GPIO 34
#define BOARD_SD_PWR_LOW_MS 50

/* Battery ADC: adc_unit_t has ADC_UNIT_1 == 0. Writing 1 selects ADC_UNIT_2
 * and ADC2_CHANNEL_0 == GPIO11, which is the SD CLK (DESIGN §2, §10.1).
 * Evidence: esp_hal_ana_conv/include/hal/adc_types.h:21-24;
 * soc/esp32s3/include/soc/adc_channel.h:10,40. */
#define BOARD_ADC_UNIT 0                /* ADC_UNIT_1 */
#define BOARD_ADC_CHANNEL 0             /* ADC1_CH0 = GPIO1 */
#define BOARD_ADC_GPIO 1
#define BOARD_VBAT_EN_GPIO 45           /* high only during the sample window */
#define BOARD_CHARGE_DET_GPIO 5         /* input, low = "true"; semantics unconfirmed */

/* Keys: GPIO 2/4/6/7/8, active low.
 * 2026-11-25 物理映射（左->右/上->下：左/右/胶囊菜单/○确认/×取消）：
 *   - 结合原厂 1.3.739 反汇编 KeyInit(@0x4201DE5C) 与 UI 事件回调：
 *     GPIO2 = PREV (左 ◀), GPIO6 = NEXT (右 ▶),
 *     GPIO4 = FUNC (胶囊菜单键 ▭), GPIO8 = OK (确认 ○), GPIO7 = MENU (取消/返回 ×)。 */
#define BOARD_KEY_PREV_GPIO 2
#define BOARD_KEY_NEXT_GPIO 6
#define BOARD_KEY_FUNC_GPIO 4
#define BOARD_KEY_MENU_GPIO 7
#define BOARD_KEY_OK_GPIO 8
#define BOARD_KEY_COUNT 5
