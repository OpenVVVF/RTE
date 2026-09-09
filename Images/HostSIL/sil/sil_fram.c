/*
 * sil_fram.c — SIL replacement for
 * Src/Inverter/Drivers/Storage/cy15b102q_driver.c.
 *
 * In-memory CY15B102Q (2-Mbit FRAM) model: a zero-initialised 256 KiB image
 * with optional file persistence (sil_fram_attach path from the scenario).
 * No SPI/GPIO traffic is simulated beyond keeping the chip-select line
 * bookkeeping visible to the GPIO model.
 */
#include "cy15b102q_driver.h"

#include <stdio.h>
#include <string.h>

static uint8_t g_image[CY15B102Q_SIZE];
static char    g_path[512];
static int     g_dirty = 0;

/* Hook called by host_sil main() before the firmware boots.  With path ==
 * NULL the image starts zeroed (fresh FRAM) and is never persisted. */
void sil_fram_attach(const char* path) {
    if (path == NULL || path[0] == '\0') return;
    snprintf(g_path, sizeof(g_path), "%s", path);
    FILE* f = fopen(g_path, "rb");
    if (f != NULL) {
        const size_t got = fread(g_image, 1, sizeof(g_image), f);
        (void)got;   /* short read = smaller/former image; rest stays 0 */
        fclose(f);
    }
    /* Missing file => first-boot zeroed image; it is created on detach. */
}

/* Flush back to the backing file if attached. */
void sil_fram_detach_save(void) {
    if (g_path[0] == '\0' || !g_dirty) return;
    FILE* f = fopen(g_path, "wb");
    if (f != NULL) {
        (void)fwrite(g_image, 1, sizeof(g_image), f);
        fclose(f);
    }
}

static void select_chip(CY15B102Q_HandleTypeDef* dev) {
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_RESET);
}

static void deselect_chip(CY15B102Q_HandleTypeDef* dev) {
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_SET);
}

HAL_StatusTypeDef CY15B102Q_Init(CY15B102Q_HandleTypeDef* dev) {
    if (dev == NULL) return HAL_ERROR;
    HAL_GPIO_WritePin(dev->cs_port,   dev->cs_pin,   GPIO_PIN_SET);
    HAL_GPIO_WritePin(dev->wp_port,   dev->wp_pin,   GPIO_PIN_SET);
    HAL_GPIO_WritePin(dev->hold_port, dev->hold_pin, GPIO_PIN_SET);
    HAL_Delay(1);
    /* ID check always passes in SIL (no JEDEC mismatch possible). */
    return HAL_OK;
}

uint8_t CY15B102Q_ReadStatus(CY15B102Q_HandleTypeDef* dev) {
    (void)dev;
    return 0;   /* WIP=0 (always ready) */
}

void CY15B102Q_WriteEnable(CY15B102Q_HandleTypeDef* dev)  { (void)dev; }
void CY15B102Q_WriteDisable(CY15B102Q_HandleTypeDef* dev) { (void)dev; }

void CY15B102Q_Read(CY15B102Q_HandleTypeDef* dev, uint32_t addr,
                    uint8_t* buf, uint32_t len) {
    select_chip(dev);
    for (uint32_t i = 0; i < len; ++i) {
        buf[i] = g_image[(addr + i) & CY15B102Q_ADDR_MASK];
    }
    deselect_chip(dev);
}

void CY15B102Q_Write(CY15B102Q_HandleTypeDef* dev, uint32_t addr,
                     const uint8_t* buf, uint32_t len) {
    select_chip(dev);
    for (uint32_t i = 0; i < len; ++i) {
        g_image[(addr + i) & CY15B102Q_ADDR_MASK] = buf[i];
    }
    g_dirty = 1;
    deselect_chip(dev);
}

uint64_t CY15B102Q_ReadID(CY15B102Q_HandleTypeDef* dev) {
    (void)dev;
    return 0x047F5E03ULL;   /* plausible Cypress JEDEC-style ID */
}

void CY15B102Q_Sleep(CY15B102Q_HandleTypeDef* dev) { (void)dev; }
void CY15B102Q_Wake(CY15B102Q_HandleTypeDef* dev)  { (void)dev; }

__attribute__((weak)) void CY15B102Q_FaultCallback(CY15B102Q_FaultCode code) {
    (void)code;
}

uint32_t CY15B102Q_GetErrorCount(void) { return 0; }
void     CY15B102Q_ClearErrorCount(void) {}
