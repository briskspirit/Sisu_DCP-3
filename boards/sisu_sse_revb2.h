/*
 * Sisu SSE Rev B2 RP2354B board - Nokia 3210 NSE-8 replacement PCB.
 *
 * MCU: RP2354B (RP2350B die + 2 MB internal stacked flash, 48 GPIO), 12 MHz crystal.
 * USB is exposed only on the battery-bay service-pad jig; stdio is over USB.
 * Application pin map: include/hal/board.h (matches the schematic, wiring §20).
 *
 * This board definition exists so the Pico SDK does NOT apply the stock "pico2"
 * reserved-pin semantics, which are wrong for this hardware:
 *   - There is no onboard LED (GP25 is I2C SCL).
 *   - GP22 drives vibra and GP23 drives the LCD backlight.
 *   - Service VBUS is sensed directly on GP28; PMIC PS/SYNC is driven on GP33.
 *   - Battery measurement is provided by the LTC2959; GP26/GP27 are unconnected.
 *   - The shared active-low interrupt from TCA8418/RV-8803/LTC2959 is on GP42.
 *   - The part has 2 MB internal flash, not the 4 MB external flash pico2 assumes.
 *
 * Copyright (c) 2026 briskspirit
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_SISU_SSE_REVB2_H
#define _BOARDS_SISU_SSE_REVB2_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define SISU_SSE_REVB2

// --- RP2350 VARIANT (RP2354B == RP2350B package: 48 GPIO) ---
#define PICO_RP2350A 0

// --- UART --- (modem UART0 on GP0/GP1; firmware stdio is over USB)
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// No onboard LED on this board (GP25 is I2C SCL). PICO_DEFAULT_LED_PIN intentionally undefined.

// --- I2C --- (TCA8418 0x34, NAU88C22 0x1A, RV-8803 0x32, LTC2959 0x63)
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 24
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 25
#endif

// --- SPI --- (PCD8544 LCD, write-only: no MISO/RX line)
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 18
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 19
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 17
#endif

// --- FLASH --- (RP2354B: 2 MB internal stacked flash)
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

// NOTE: deliberately NO Pico-board PICO_SMPS_MODE_PIN / PICO_VBUS_PIN /
// PICO_VSYS_PIN declarations here. The application owns its direct GP28 VBUS
// input and GP33 PMIC mode output; battery data comes from the LTC2959.

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
