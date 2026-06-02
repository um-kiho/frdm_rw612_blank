/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * NXP FlexComm SPI master backend for sk6812.c.
 *
 * Defaults (override via CMake or here):
 *   - SPI instance     : FLEXCOMM4 (FC0/FC2/FC3 already used by debug
 *                        console, I2C2, radar UART)
 *   - SPI baudrate     : 6.4 MHz  (1 SK6812 bit per SPI byte; see sk6812.h)
 *   - SPI mode         : CPOL=0, CPHA=0, MSB first, 8-bit
 *
 * The strip's DIN pin connects to the SPI peripheral's MOSI. SCK / MISO /
 * CS pins are unused by SK6812 but the FlexComm block still drives SCK
 * during the transfer - either route it to a test point or leave the pad
 * unmuxed (no risk; idle low).
 *
 * NOTE: this implementation uses the SPI peripheral in blocking mode for
 * simplicity. For longer strips (> ~64 LEDs) or animation-heavy scenes,
 * switch to SPI_MasterTransferDMA from fsl_spi_dma.h.
 */

#ifndef APP_SK6812_IO_NXP_SPI_H
#define APP_SK6812_IO_NXP_SPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FlexComm instance: SPI4 by default. Must match the pin mux. */
#ifndef APP_LED_SPI_INSTANCE
#define APP_LED_SPI_INSTANCE     4u
#endif

/* SPI clock: 6.4 MHz +- a few ppm. 6.4 MHz is exact at 1 SPI byte / SK6812
 * bit. Acceptable range tested in practice: 6.25-6.66 MHz. */
#ifndef APP_LED_SPI_BAUDRATE_HZ
#define APP_LED_SPI_BAUDRATE_HZ  6400000u
#endif

/* ----------------------------------------------------------------------- *
 * Optional DMA backend.
 *
 * Define APP_LED_USE_DMA=1 (from CMake or in a build-config header) to
 * switch the implementation from blocking transfers to fsl_spi_dma. The
 * public API stays the same; sk6812_io_nxp_spi_send_blocking() returns
 * once the DMA chain completes (it waits on a binary semaphore that the
 * SPI/DMA callback gives from ISR context).
 *
 * The DMA channel ID is SoC-specific. Check the RW612 reference manual
 * "DMA request muxing" table for the FLEXCOMMx_TX trigger ID and edit
 * APP_LED_SPI_DMA_TX_CH below to match. NXP's "spi/dma_b2b_transfer"
 * example for frdmrw612 is a known-good starting point.
 * ----------------------------------------------------------------------- */
#ifndef APP_LED_USE_DMA
#define APP_LED_USE_DMA          1
#endif

#ifndef APP_LED_SPI_DMA_CONTROLLER
#define APP_LED_SPI_DMA_CONTROLLER  DMA0
#endif

#ifndef APP_LED_SPI_DMA_TX_CH
#define APP_LED_SPI_DMA_TX_CH       4u       /* TODO: confirm for FLEXCOMM4 */
#endif

#ifndef APP_LED_SPI_DMA_TIMEOUT_MS
#define APP_LED_SPI_DMA_TIMEOUT_MS  200u
#endif

/* One-shot initialisation: clocks, peripheral, NULL transfers. */
int sk6812_io_nxp_spi_init(void);

/* Blocking send. Caller owns buf for the duration. Returns 0 on success. */
int sk6812_io_nxp_spi_send_blocking(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_SK6812_IO_NXP_SPI_H */
