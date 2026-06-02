/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FlexComm SPI master backend for the SK6812RGBW driver.
 *
 * Two implementations, selected at compile time via APP_LED_USE_DMA:
 *
 *   APP_LED_USE_DMA == 0  (default)
 *     SPI_MasterTransferBlocking(). Simple, works for short strips (<= 64
 *     LEDs). The calling task is blocked on the SPI peripheral for the
 *     full transfer duration (~2 ms / 100 LEDs at 6.4 MHz).
 *
 *   APP_LED_USE_DMA == 1
 *     fsl_spi_dma. The SPI peripheral is fed by a DMA channel; the
 *     caller blocks on a Zephyr semaphore that the DMA-complete ISR
 *     gives. The render loop in led_task.c can then keep producing 50 Hz
 *     animations on hundreds of LEDs without pegging the CPU.
 *
 * Higher layers (sk6812.c) already serialise refresh() with a mutex, so
 * this file is safe to call from any Zephyr thread.
 */

#include "sk6812_io_nxp_spi.h"

#include "fsl_common.h"
#include "fsl_spi.h"
#include "fsl_clock.h"

#if (APP_LED_USE_DMA != 0)
#include "fsl_dma.h"
#include "fsl_spi_dma.h"
#include <zephyr/kernel.h>
#endif

#if   (APP_LED_SPI_INSTANCE == 0u)
  #define APP_LED_SPI_BASE   SPI0
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(0u)
#elif (APP_LED_SPI_INSTANCE == 1u)
  #define APP_LED_SPI_BASE   SPI1
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(1u)
#elif (APP_LED_SPI_INSTANCE == 2u)
  #define APP_LED_SPI_BASE   SPI2
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(2u)
#elif (APP_LED_SPI_INSTANCE == 3u)
  #define APP_LED_SPI_BASE   SPI3
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(3u)
#elif (APP_LED_SPI_INSTANCE == 4u)
  #define APP_LED_SPI_BASE   SPI4
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(4u)
#elif (APP_LED_SPI_INSTANCE == 5u)
  #define APP_LED_SPI_BASE   SPI5
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(5u)
#elif (APP_LED_SPI_INSTANCE == 6u)
  #define APP_LED_SPI_BASE   SPI6
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(6u)
#elif (APP_LED_SPI_INSTANCE == 7u)
  #define APP_LED_SPI_BASE   SPI7
  #define APP_LED_SPI_CLK()  CLOCK_GetFlexCommClkFreq(7u)
#else
  #error "APP_LED_SPI_INSTANCE must be 0..7"
#endif

static volatile int s_inited;

#if (APP_LED_USE_DMA != 0)
static dma_handle_t      s_dma_tx_handle;
static spi_dma_handle_t  s_spi_dma_handle;
static struct k_sem      s_dma_done;

static void spi_dma_done_cb(SPI_Type *base, spi_dma_handle_t *h,
                            status_t status, void *userData)
{
    (void)base; (void)h; (void)status; (void)userData;
    k_sem_give(&s_dma_done);
}
#endif /* APP_LED_USE_DMA */

int sk6812_io_nxp_spi_init(void)
{
    if (s_inited) return 0;

    spi_master_config_t cfg;
    SPI_MasterGetDefaultConfig(&cfg);

    /* CPOL=0, CPHA=0 means the line idles low and the data bit is sampled
     * on the rising edge. Since the SK6812 only sees MOSI (its DIN), the
     * exact polarity / phase does not affect waveform shape, but we keep
     * CPOL=0 so the line is low between frames (avoids spurious bits when
     * SPI is enabled but no data is being clocked). */
    cfg.polarity     = kSPI_ClockPolarityActiveHigh;
    cfg.phase        = kSPI_ClockPhaseFirstEdge;
    cfg.direction    = kSPI_MsbFirst;
    cfg.dataWidth    = kSPI_Data8Bits;
    cfg.baudRate_Bps = APP_LED_SPI_BAUDRATE_HZ;
    cfg.enableMaster = true;

    /* SSEL not used by SK6812 but the SDK config struct needs a value. */
    cfg.sselNum      = kSPI_Ssel0;
    cfg.sselPol      = kSPI_SpolActiveAllLow;

    if (SPI_MasterInit(APP_LED_SPI_BASE, &cfg, APP_LED_SPI_CLK()) != kStatus_Success) {
        return -1;
    }

#if (APP_LED_USE_DMA != 0)
    k_sem_init(&s_dma_done, 0, 1);

    DMA_Init(APP_LED_SPI_DMA_CONTROLLER);
    DMA_EnableChannel(APP_LED_SPI_DMA_CONTROLLER, APP_LED_SPI_DMA_TX_CH);
    DMA_CreateHandle(&s_dma_tx_handle, APP_LED_SPI_DMA_CONTROLLER,
                     APP_LED_SPI_DMA_TX_CH);

    /* tx-only chain: rx handle is NULL. */
    if (SPI_MasterTransferCreateHandleDMA(APP_LED_SPI_BASE,
                                          &s_spi_dma_handle,
                                          spi_dma_done_cb, NULL,
                                          &s_dma_tx_handle, NULL)
        != kStatus_Success) {
        return -3;
    }
#endif

    s_inited = 1;
    return 0;
}

int sk6812_io_nxp_spi_send_blocking(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0u)            return -1;
    if (!s_inited && sk6812_io_nxp_spi_init() != 0) return -2;

    spi_transfer_t xfer;
    xfer.txData      = (uint8_t *)buf;          /* SDK type is not const  */
    xfer.rxData      = NULL;
    xfer.dataSize    = len;
    xfer.configFlags = kSPI_FrameAssert;

#if (APP_LED_USE_DMA != 0)
    /* Drain any stale semaphore (e.g. from a cancelled previous transfer). */
    k_sem_reset(&s_dma_done);

    if (SPI_MasterTransferDMA(APP_LED_SPI_BASE, &s_spi_dma_handle, &xfer)
        != kStatus_Success) {
        return -3;
    }
    if (k_sem_take(&s_dma_done, K_MSEC(APP_LED_SPI_DMA_TIMEOUT_MS)) != 0) {
        SPI_MasterTransferAbortDMA(APP_LED_SPI_BASE, &s_spi_dma_handle);
        return -4;
    }
    return 0;
#else
    return (SPI_MasterTransferBlocking(APP_LED_SPI_BASE, &xfer) == kStatus_Success)
           ? 0 : -3;
#endif
}
