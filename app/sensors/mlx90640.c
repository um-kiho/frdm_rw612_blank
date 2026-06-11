/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MLX90640 driver. Calibration / temperature math adapted from the Melexis
 * MLX90640 API (Apache-2.0), wired to the shared app/i2c_bus front-end.
 */

#include "mlx90640.h"
#include "i2c_bus.h"

#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mlx90640, LOG_LEVEL_INF);

#define SCALEALPHA          0.000001f

/* Words read per I2C transaction (keeps the shared-bus mutex hold bounded;
 * the MLX90640 RAM/EEPROM address is re-sent for each chunk). */
#ifndef MLX_READ_CHUNK_WORDS
#define MLX_READ_CHUNK_WORDS   256u
#endif

/* 2^n as float, valid for any (possibly large) integer exponent. */
static inline float pow2f(int n)
{
    return ldexpf(1.0f, n);
}

/* ------------------------------------------------------------------------- *
 * I2C transport (16-bit address, 16-bit big-endian words)
 * ------------------------------------------------------------------------- */
static int mlx_read(mlx90640_t *dev, uint16_t start_addr,
                    uint16_t *out, size_t n_words)
{
    uint8_t rxb[MLX_READ_CHUNK_WORDS * 2u];
    size_t done = 0;

    while (done < n_words) {
        size_t chunk = n_words - done;
        if (chunk > MLX_READ_CHUNK_WORDS) chunk = MLX_READ_CHUNK_WORDS;

        uint16_t addr = (uint16_t)(start_addr + done);
        uint8_t  tx[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFFu) };

        if (i2c_bus_write_read(dev->addr, tx, 2u, rxb, chunk * 2u) != 0) {
            return MLX90640_ERR_IO;
        }
        for (size_t i = 0; i < chunk; ++i) {
            out[done + i] = (uint16_t)((rxb[i * 2] << 8) | rxb[i * 2 + 1]);
        }
        done += chunk;
    }
    return MLX90640_OK;
}

static int mlx_write(mlx90640_t *dev, uint16_t addr, uint16_t value)
{
    uint8_t tx[4] = {
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFFu),
        (uint8_t)(value >> 8), (uint8_t)(value & 0xFFu),
    };
    if (i2c_bus_write(dev->addr, tx, 4u) != 0) {
        return MLX90640_ERR_IO;
    }
    /* Read back and verify (the MLX90640 config registers are in EEPROM-like
     * cells; a mismatch usually means a bus error). */
    uint16_t check = 0;
    if (mlx_read(dev, addr, &check, 1u) != MLX90640_OK) {
        return MLX90640_ERR_IO;
    }
    if (check != value) {
        LOG_WRN("write 0x%04X=0x%04X verify got 0x%04X", addr, value, check);
        return MLX90640_ERR_VERIFY;
    }
    return MLX90640_OK;
}

/* ------------------------------------------------------------------------- *
 * Configuration (control register 1 @ 0x800D)
 * ------------------------------------------------------------------------- */
int mlx90640_set_refresh_rate(mlx90640_t *dev, uint8_t rr)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;
    uint16_t ctrl = 0;
    int rc = mlx_read(dev, MLX90640_REG_CONTROL1, &ctrl, 1u);
    if (rc != MLX90640_OK) return rc;
    uint16_t value = (uint16_t)((ctrl & 0xFC7Fu) | ((uint16_t)(rr & 0x07u) << 7));
    return mlx_write(dev, MLX90640_REG_CONTROL1, value);
}

int mlx90640_set_resolution(mlx90640_t *dev, uint8_t res)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;
    uint16_t ctrl = 0;
    int rc = mlx_read(dev, MLX90640_REG_CONTROL1, &ctrl, 1u);
    if (rc != MLX90640_OK) return rc;
    uint16_t value = (uint16_t)((ctrl & 0xF3FFu) | ((uint16_t)(res & 0x03u) << 10));
    return mlx_write(dev, MLX90640_REG_CONTROL1, value);
}

int mlx90640_set_chess_mode(mlx90640_t *dev)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;
    uint16_t ctrl = 0;
    int rc = mlx_read(dev, MLX90640_REG_CONTROL1, &ctrl, 1u);
    if (rc != MLX90640_OK) return rc;
    uint16_t value = (uint16_t)(ctrl | 0x1000u);  /* bit12 = chess pattern */
    return mlx_write(dev, MLX90640_REG_CONTROL1, value);
}

/* ------------------------------------------------------------------------- *
 * Acquisition
 * ------------------------------------------------------------------------- */
int mlx90640_dump_ee(mlx90640_t *dev)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;
    return mlx_read(dev, MLX90640_EE_BASE, dev->eeData, MLX90640_EE_WORDS);
}

int mlx90640_get_frame(mlx90640_t *dev)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;

    uint16_t *frame = dev->frame;
    uint16_t status = 0;
    uint16_t ctrl1  = 0;
    uint16_t ready  = 0;
    int rc;
    int spins = 0;

    /* Wait for "new data in RAM" (status bit3). */
    while (ready == 0) {
        rc = mlx_read(dev, MLX90640_REG_STATUS, &status, 1u);
        if (rc != MLX90640_OK) return rc;
        ready = (uint16_t)(status & 0x0008u);
        if (ready == 0) {
            if (++spins > 2000) return MLX90640_ERR_TIMEOUT;
            k_sleep(K_MSEC(1));
        }
    }

    /* Clear data-ready (and keep RAM-overwrite enabled), then read the frame. */
    rc = mlx_write(dev, MLX90640_REG_STATUS, 0x0030u);
    if (rc != MLX90640_OK) return rc;

    rc = mlx_read(dev, MLX90640_RAM_BASE, frame, 832u);
    if (rc != MLX90640_OK) return rc;

    rc = mlx_read(dev, MLX90640_REG_CONTROL1, &ctrl1, 1u);
    if (rc != MLX90640_OK) return rc;

    frame[832] = ctrl1;
    frame[833] = (uint16_t)(status & 0x0001u);   /* sub-page number */
    return (int)frame[833];
}

int mlx90640_get_subpage(const mlx90640_t *dev)
{
    return (int)dev->frame[833];
}

/* ------------------------------------------------------------------------- *
 * Vdd / Ta
 * ------------------------------------------------------------------------- */
float mlx90640_get_vdd(mlx90640_t *dev)
{
    float vdd = (float)dev->frame[810];
    if (vdd > 32767.0f) vdd -= 65536.0f;

    int resolutionRAM = (dev->frame[832] & 0x0C00) >> 10;
    float resolutionCorrection = pow2f(dev->resolutionEE) / pow2f(resolutionRAM);
    vdd = (resolutionCorrection * vdd - (float)dev->vdd25) / (float)dev->kVdd + 3.3f;
    return vdd;
}

float mlx90640_get_ta(mlx90640_t *dev)
{
    float vdd = mlx90640_get_vdd(dev);

    float ptat = (float)dev->frame[800];
    if (ptat > 32767.0f) ptat -= 65536.0f;

    float ptatArt = (float)dev->frame[768];
    if (ptatArt > 32767.0f) ptatArt -= 65536.0f;
    ptatArt = (ptat / (ptat * dev->alphaPTAT + ptatArt)) * pow2f(18);

    float ta = (ptatArt / (1.0f + dev->KvPTAT * (vdd - 3.3f)) - (float)dev->vPTAT25);
    ta = ta / dev->KtPTAT + 25.0f;
    return ta;
}

/* ------------------------------------------------------------------------- *
 * EEPROM parameter extraction (Melexis API, adapted)
 * ------------------------------------------------------------------------- */
static void extract_vdd(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int16_t kVdd  = (ee[51] & 0xFF00) >> 8;
    if (kVdd > 127) kVdd -= 256;
    kVdd *= 32;
    int16_t vdd25 = ee[51] & 0x00FF;
    vdd25 = (int16_t)(((vdd25 - 256) << 5) - 8192);
    m->kVdd  = kVdd;
    m->vdd25 = vdd25;
}

static void extract_ptat(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    float KvPTAT = (ee[50] & 0xFC00) >> 10;
    if (KvPTAT > 31) KvPTAT -= 64;
    KvPTAT /= 4096.0f;
    float KtPTAT = ee[50] & 0x03FF;
    if (KtPTAT > 511) KtPTAT -= 1024;
    KtPTAT /= 8.0f;
    m->vPTAT25  = ee[49];
    m->alphaPTAT = (ee[16] & 0xF000) / pow2f(14) + 8.0f;
    m->KvPTAT = KvPTAT;
    m->KtPTAT = KtPTAT;
}

static void extract_gain(mlx90640_t *m)
{
    int16_t gainEE = (int16_t)m->eeData[48];
    m->gainEE = gainEE;
}

static void extract_tgc(mlx90640_t *m)
{
    float tgc = m->eeData[60] & 0x00FF;
    if (tgc > 127) tgc -= 256;
    m->tgc = tgc / 32.0f;
}

static void extract_resolution(mlx90640_t *m)
{
    m->resolutionEE = (m->eeData[56] & 0x3000) >> 12;
}

static void extract_ksta(mlx90640_t *m)
{
    float KsTa = (m->eeData[60] & 0xFF00) >> 8;
    if (KsTa > 127) KsTa -= 256;
    m->KsTa = KsTa / 8192.0f;
}

static void extract_ksto(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int step = ((ee[63] & 0x3000) >> 12) * 10;
    m->ct[0] = -40;
    m->ct[1] = 0;
    m->ct[2] = (ee[63] & 0x00F0) >> 4;
    m->ct[3] = (ee[63] & 0x0F00) >> 8;
    m->ct[2] = (int16_t)(m->ct[2] * step);
    m->ct[3] = (int16_t)(m->ct[2] + m->ct[3] * step);
    m->ct[4] = 400;

    int KsToScale = (ee[63] & 0x000F) + 8;
    KsToScale = 1 << KsToScale;

    m->ksTo[0] = ee[61] & 0x00FF;
    m->ksTo[1] = (ee[61] & 0xFF00) >> 8;
    m->ksTo[2] = ee[62] & 0x00FF;
    m->ksTo[3] = (ee[62] & 0xFF00) >> 8;
    for (int i = 0; i < 4; i++) {
        if (m->ksTo[i] > 127) m->ksTo[i] -= 256;
        m->ksTo[i] = m->ksTo[i] / (float)KsToScale;
    }
    m->ksTo[4] = -0.0002f;
}

static void extract_cp(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    uint8_t alphaScale = ((ee[32] & 0xF000) >> 12) + 27;

    int16_t offsetSP0 = (ee[58] & 0x03FF);
    if (offsetSP0 > 511) offsetSP0 -= 1024;
    int16_t offsetSP1 = (ee[58] & 0xFC00) >> 10;
    if (offsetSP1 > 31) offsetSP1 -= 64;
    offsetSP1 = (int16_t)(offsetSP1 + offsetSP0);

    float alphaSP0 = (ee[57] & 0x03FF);
    if (alphaSP0 > 511) alphaSP0 -= 1024;
    alphaSP0 = alphaSP0 / pow2f(alphaScale);

    float alphaSP1 = (ee[57] & 0xFC00) >> 10;
    if (alphaSP1 > 31) alphaSP1 -= 64;
    alphaSP1 = (1.0f + alphaSP1 / 128.0f) * alphaSP0;

    float cpKta = (ee[59] & 0x00FF);
    if (cpKta > 127) cpKta -= 256;
    uint8_t ktaScale1 = ((ee[56] & 0x00F0) >> 4) + 8;
    m->cpKta = cpKta / pow2f(ktaScale1);

    float cpKv = (ee[59] & 0xFF00) >> 8;
    if (cpKv > 127) cpKv -= 256;
    uint8_t kvScale = (ee[56] & 0x0F00) >> 8;
    m->cpKv = cpKv / pow2f(kvScale);

    m->cpAlpha[0]  = alphaSP0;
    m->cpAlpha[1]  = alphaSP1;
    m->cpOffset[0] = offsetSP0;
    m->cpOffset[1] = offsetSP1;
}

static void extract_alpha(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int accRow[24];
    int accColumn[32];
    static float alphaTemp[MLX90640_PIXELS];
    int p;

    uint8_t accRemScale    = ee[32] & 0x000F;
    uint8_t accColumnScale = (ee[32] & 0x00F0) >> 4;
    uint8_t accRowScale    = (ee[32] & 0x0F00) >> 8;
    uint8_t alphaScale     = ((ee[32] & 0xF000) >> 12) + 30;
    int     alphaRef       = ee[33];

    for (int i = 0; i < 6; i++) {
        p = i * 4;
        accRow[p + 0] = (ee[34 + i] & 0x000F);
        accRow[p + 1] = (ee[34 + i] & 0x00F0) >> 4;
        accRow[p + 2] = (ee[34 + i] & 0x0F00) >> 8;
        accRow[p + 3] = (ee[34 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 24; i++) {
        if (accRow[i] > 7) accRow[i] -= 16;
    }
    for (int i = 0; i < 8; i++) {
        p = i * 4;
        accColumn[p + 0] = (ee[40 + i] & 0x000F);
        accColumn[p + 1] = (ee[40 + i] & 0x00F0) >> 4;
        accColumn[p + 2] = (ee[40 + i] & 0x0F00) >> 8;
        accColumn[p + 3] = (ee[40 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 32; i++) {
        if (accColumn[i] > 7) accColumn[i] -= 16;
    }

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            alphaTemp[p] = (ee[64 + p] & 0x03F0) >> 4;
            if (alphaTemp[p] > 31) alphaTemp[p] -= 64;
            alphaTemp[p] = alphaTemp[p] * (1 << accRemScale);
            alphaTemp[p] = (alphaRef + (accRow[i] << accRowScale) +
                            (accColumn[j] << accColumnScale) + alphaTemp[p]);
            alphaTemp[p] = alphaTemp[p] / pow2f(alphaScale);
            alphaTemp[p] = alphaTemp[p] - m->tgc * (m->cpAlpha[0] + m->cpAlpha[1]) / 2.0f;
            alphaTemp[p] = SCALEALPHA / alphaTemp[p];
        }
    }

    float temp = alphaTemp[0];
    for (int i = 1; i < MLX90640_PIXELS; i++) {
        if (alphaTemp[i] > temp) temp = alphaTemp[i];
    }
    alphaScale = 0;
    while (temp < 32768.0f) {
        temp *= 2.0f;
        alphaScale++;
    }
    for (int i = 0; i < MLX90640_PIXELS; i++) {
        temp = alphaTemp[i] * pow2f(alphaScale);
        m->alpha[i] = (uint16_t)(temp + 0.5f);
    }
    m->alphaScale = alphaScale;
}

static void extract_offset(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int occRow[24];
    int occColumn[32];
    int p;

    uint8_t occRemScale    = (ee[16] & 0x000F);
    uint8_t occColumnScale = (ee[16] & 0x00F0) >> 4;
    uint8_t occRowScale    = (ee[16] & 0x0F00) >> 8;
    int16_t offsetRef      = (int16_t)ee[17];

    for (int i = 0; i < 6; i++) {
        p = i * 4;
        occRow[p + 0] = (ee[18 + i] & 0x000F);
        occRow[p + 1] = (ee[18 + i] & 0x00F0) >> 4;
        occRow[p + 2] = (ee[18 + i] & 0x0F00) >> 8;
        occRow[p + 3] = (ee[18 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 24; i++) {
        if (occRow[i] > 7) occRow[i] -= 16;
    }
    for (int i = 0; i < 8; i++) {
        p = i * 4;
        occColumn[p + 0] = (ee[24 + i] & 0x000F);
        occColumn[p + 1] = (ee[24 + i] & 0x00F0) >> 4;
        occColumn[p + 2] = (ee[24 + i] & 0x0F00) >> 8;
        occColumn[p + 3] = (ee[24 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 32; i++) {
        if (occColumn[i] > 7) occColumn[i] -= 16;
    }

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            int off = (ee[64 + p] & 0xFC00) >> 10;
            if (off > 31) off -= 64;
            off = off * (1 << occRemScale);
            off = (offsetRef + (occRow[i] << occRowScale) +
                   (occColumn[j] << occColumnScale) + off);
            m->offset[p] = (int16_t)off;
        }
    }
}

static void extract_kta_pixel(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int8_t KtaRC[4];
    static float ktaTemp[MLX90640_PIXELS];
    int p;

    int8_t v;
    v = (ee[54] & 0xFF00) >> 8; if (v > 127) v -= 256; KtaRC[0] = v;   /* RoCo */
    v = (ee[54] & 0x00FF);      if (v > 127) v -= 256; KtaRC[2] = v;   /* ReCo */
    v = (ee[55] & 0xFF00) >> 8; if (v > 127) v -= 256; KtaRC[1] = v;   /* RoCe */
    v = (ee[55] & 0x00FF);      if (v > 127) v -= 256; KtaRC[3] = v;   /* ReCe */

    uint8_t ktaScale1 = ((ee[56] & 0x00F0) >> 4) + 8;
    uint8_t ktaScale2 = (ee[56] & 0x000F);

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            int split = 2 * (p / 32 - (p / 64) * 2) + p % 2;
            float kt = (ee[64 + p] & 0x000E) >> 1;
            if (kt > 3) kt -= 8;
            kt = kt * (1 << ktaScale2);
            kt = KtaRC[split] + kt;
            ktaTemp[p] = kt / pow2f(ktaScale1);
        }
    }

    float temp = fabsf(ktaTemp[0]);
    for (int i = 1; i < MLX90640_PIXELS; i++) {
        if (fabsf(ktaTemp[i]) > temp) temp = fabsf(ktaTemp[i]);
    }
    ktaScale1 = 0;
    while (temp < 64.0f) { temp *= 2.0f; ktaScale1++; }

    for (int i = 0; i < MLX90640_PIXELS; i++) {
        float t = ktaTemp[i] * pow2f(ktaScale1);
        m->kta[i] = (int8_t)(t < 0 ? (t - 0.5f) : (t + 0.5f));
    }
    m->ktaScale = ktaScale1;
}

static void extract_kv_pixel(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    int8_t KvT[4];
    static float kvTemp[MLX90640_PIXELS];
    int p;

    int8_t v;
    v = (ee[52] & 0xF000) >> 12; if (v > 7) v -= 16; KvT[0] = v;   /* RoCo */
    v = (ee[52] & 0x0F00) >> 8;  if (v > 7) v -= 16; KvT[2] = v;   /* ReCo */
    v = (ee[52] & 0x00F0) >> 4;  if (v > 7) v -= 16; KvT[1] = v;   /* RoCe */
    v = (ee[52] & 0x000F);       if (v > 7) v -= 16; KvT[3] = v;   /* ReCe */

    uint8_t kvScale = (ee[56] & 0x0F00) >> 8;

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            int split = 2 * (p / 32 - (p / 64) * 2) + p % 2;
            kvTemp[p] = (float)KvT[split] / pow2f(kvScale);
        }
    }

    float temp = fabsf(kvTemp[0]);
    for (int i = 1; i < MLX90640_PIXELS; i++) {
        if (fabsf(kvTemp[i]) > temp) temp = fabsf(kvTemp[i]);
    }
    kvScale = 0;
    while (temp < 64.0f) { temp *= 2.0f; kvScale++; }

    for (int i = 0; i < MLX90640_PIXELS; i++) {
        float t = kvTemp[i] * pow2f(kvScale);
        m->kv[i] = (int8_t)(t < 0 ? (t - 0.5f) : (t + 0.5f));
    }
    m->kvScale = kvScale;
}

static void extract_cilc(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    uint8_t calibrationModeEE = (ee[10] & 0x0800) >> 4;
    calibrationModeEE = calibrationModeEE ^ 0x80;

    float c0 = (ee[53] & 0x003F);
    if (c0 > 31) c0 -= 64;
    c0 = c0 / 16.0f;
    float c1 = (ee[53] & 0x07C0) >> 6;
    if (c1 > 15) c1 -= 32;
    c1 = c1 / 2.0f;
    float c2 = (ee[53] & 0xF800) >> 11;
    if (c2 > 15) c2 -= 32;
    c2 = c2 / 8.0f;

    m->calibrationModeEE = calibrationModeEE;
    m->ilChessC[0] = c0;
    m->ilChessC[1] = c1;
    m->ilChessC[2] = c2;
}

static void extract_deviating(mlx90640_t *m)
{
    uint16_t *ee = m->eeData;
    uint16_t brokenCnt = 0, outlierCnt = 0;

    for (int i = 0; i < 5; i++) {
        m->brokenPixels[i]  = 0xFFFF;
        m->outlierPixels[i] = 0xFFFF;
    }
    for (uint16_t pix = 0; pix < MLX90640_PIXELS &&
                           brokenCnt < 5 && outlierCnt < 5; pix++) {
        if (ee[pix + 64] == 0) {
            m->brokenPixels[brokenCnt++] = pix;
        } else if ((ee[pix + 64] & 0x0001) != 0) {
            m->outlierPixels[outlierCnt++] = pix;
        }
    }
    if (brokenCnt > 0)  LOG_WRN("%u broken pixel(s)", (unsigned)brokenCnt);
    if (outlierCnt > 0) LOG_WRN("%u outlier pixel(s)", (unsigned)outlierCnt);
}

int mlx90640_extract_parameters(mlx90640_t *dev)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;

    /* The 0xA5A5 marker in EEPROM word 10 (bit pattern) is checked loosely;
     * order matters: CP must precede Alpha. */
    extract_vdd(dev);
    extract_ptat(dev);
    extract_gain(dev);
    extract_tgc(dev);
    extract_resolution(dev);
    extract_ksta(dev);
    extract_ksto(dev);
    extract_cp(dev);
    extract_alpha(dev);
    extract_offset(dev);
    extract_kta_pixel(dev);
    extract_kv_pixel(dev);
    extract_cilc(dev);
    extract_deviating(dev);
    return MLX90640_OK;
}

/* ------------------------------------------------------------------------- *
 * Object temperature (Melexis CalculateTo, adapted)
 * ------------------------------------------------------------------------- */
void mlx90640_calculate_to(mlx90640_t *dev, float emissivity, float tr)
{
    uint16_t *frame = dev->frame;
    uint16_t subPage = frame[833];

    float vdd = mlx90640_get_vdd(dev);
    float ta  = mlx90640_get_ta(dev);

    float ta4 = (ta + 273.15f); ta4 = ta4 * ta4; ta4 = ta4 * ta4;
    float tr4 = (tr + 273.15f); tr4 = tr4 * tr4; tr4 = tr4 * tr4;
    float taTr = tr4 - (tr4 - ta4) / emissivity;

    float ktaScale   = pow2f(dev->ktaScale);
    float kvScale    = pow2f(dev->kvScale);
    float alphaScale = pow2f(dev->alphaScale);

    float alphaCorrR[4];
    alphaCorrR[0] = 1.0f / (1.0f + dev->ksTo[0] * 40.0f);
    alphaCorrR[1] = 1.0f;
    alphaCorrR[2] = (1.0f + dev->ksTo[1] * dev->ct[2]);
    alphaCorrR[3] = alphaCorrR[2] * (1.0f + dev->ksTo[2] * (dev->ct[3] - dev->ct[2]));

    /* Gain */
    float gain = (float)frame[778];
    if (gain > 32767.0f) gain -= 65536.0f;
    gain = dev->gainEE / gain;

    uint8_t mode = (frame[832] & 0x1000) >> 5;

    /* Compensation pixels */
    float irDataCP[2];
    irDataCP[0] = (float)frame[776];
    irDataCP[1] = (float)frame[808];
    for (int i = 0; i < 2; i++) {
        if (irDataCP[i] > 32767.0f) irDataCP[i] -= 65536.0f;
        irDataCP[i] *= gain;
    }
    irDataCP[0] = irDataCP[0] - dev->cpOffset[0] *
        (1.0f + dev->cpKta * (ta - 25.0f)) * (1.0f + dev->cpKv * (vdd - 3.3f));
    if (mode == dev->calibrationModeEE) {
        irDataCP[1] = irDataCP[1] - dev->cpOffset[1] *
            (1.0f + dev->cpKta * (ta - 25.0f)) * (1.0f + dev->cpKv * (vdd - 3.3f));
    } else {
        irDataCP[1] = irDataCP[1] - (dev->cpOffset[1] + dev->ilChessC[0]) *
            (1.0f + dev->cpKta * (ta - 25.0f)) * (1.0f + dev->cpKv * (vdd - 3.3f));
    }

    for (int pix = 0; pix < MLX90640_PIXELS; pix++) {
        int ilPattern = pix / 32 - (pix / 64) * 2;
        int chessPattern = ilPattern ^ (pix - (pix / 2) * 2);
        int conversionPattern = ((pix + 2) / 4 - (pix + 3) / 4 +
                                 (pix + 1) / 4 - pix / 4) * (1 - 2 * ilPattern);
        int pattern = (mode == 0) ? ilPattern : chessPattern;

        if (pattern != frame[833]) {
            continue;   /* pixel belongs to the other sub-page */
        }

        float irData = (float)frame[pix];
        if (irData > 32767.0f) irData -= 65536.0f;
        irData *= gain;

        float kta = dev->kta[pix] / ktaScale;
        float kv  = dev->kv[pix]  / kvScale;
        irData = irData - dev->offset[pix] *
            (1.0f + kta * (ta - 25.0f)) * (1.0f + kv * (vdd - 3.3f));

        if (mode != dev->calibrationModeEE) {
            irData = irData + dev->ilChessC[2] * (2 * ilPattern - 1) -
                     dev->ilChessC[1] * conversionPattern;
        }

        irData = irData - dev->tgc * irDataCP[subPage];
        irData = irData / emissivity;

        float alphaComp = SCALEALPHA * alphaScale / dev->alpha[pix];
        alphaComp = alphaComp * (1.0f + dev->KsTa * (ta - 25.0f));

        float Sx = alphaComp * alphaComp * alphaComp * (irData + alphaComp * taTr);
        Sx = sqrtf(sqrtf(Sx)) * dev->ksTo[1];

        float To = sqrtf(sqrtf(irData /
            (alphaComp * (1.0f - dev->ksTo[1] * 273.15f) + Sx) + taTr)) - 273.15f;

        int range;
        if (To < dev->ct[1])      range = 0;
        else if (To < dev->ct[2]) range = 1;
        else if (To < dev->ct[3]) range = 2;
        else                      range = 3;

        To = sqrtf(sqrtf(irData /
            (alphaComp * alphaCorrR[range] *
             (1.0f + dev->ksTo[range] * (To - dev->ct[range]))) + taTr)) - 273.15f;

        dev->To[pix] = To;
    }
}

/* ------------------------------------------------------------------------- *
 * Init
 * ------------------------------------------------------------------------- */
int mlx90640_init(mlx90640_t *dev, uint8_t addr_7b, uint8_t refresh_rate)
{
    if (dev == NULL) return MLX90640_ERR_PARAM;
    if (refresh_rate > MLX90640_RR_64HZ) return MLX90640_ERR_PARAM;

    memset(dev, 0, sizeof(*dev));
    dev->addr = addr_7b;

    if (i2c_bus_init() != 0) return MLX90640_ERR_IO;

    if (i2c_bus_probe(addr_7b) != 0) {
        LOG_ERR("no device @0x%02X", (unsigned)addr_7b);
        return MLX90640_ERR_IO;
    }

    int rc = mlx90640_set_refresh_rate(dev, refresh_rate);
    if (rc != MLX90640_OK) {
        LOG_ERR("set refresh rate failed (%d)", rc);
        return rc;
    }
    /* Chess pattern is the recommended/default readout mode. */
    rc = mlx90640_set_chess_mode(dev);
    if (rc != MLX90640_OK) return rc;

    rc = mlx90640_dump_ee(dev);
    if (rc != MLX90640_OK) {
        LOG_ERR("EEPROM dump failed (%d)", rc);
        return rc;
    }
    rc = mlx90640_extract_parameters(dev);
    if (rc != MLX90640_OK) return rc;

    LOG_INF("init @0x%02X OK (rr=%u, %dx%d)",
            (unsigned)addr_7b, (unsigned)refresh_rate,
            MLX90640_COLS, MLX90640_ROWS);
    return MLX90640_OK;
}
