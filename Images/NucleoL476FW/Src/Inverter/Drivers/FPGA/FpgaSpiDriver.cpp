// FpgaSpiDriver.cpp — Implementation of MCU SPI2 Master Driver for Tang Nano 20K FPGA
#include "Inverter/Drivers/FPGA/FpgaSpiDriver.h"

void FpgaSpi_Init(FpgaDriverState* driver) {
    if (!driver) return;
    driver->magic = 0;
    driver->version = 0;
    driver->status = 0;
    driver->fault = 0;
    driver->is_initialized = false;

    uint16_t magic_read = 0;
    if (FpgaSpi_ReadRegister(FPGA_REG_MAGIC, &magic_read)) {
        driver->magic = magic_read;
        if (magic_read == FPGA_EXPECTED_MAGIC) {
            driver->is_initialized = true;
            FpgaSpi_ReadRegister(FPGA_REG_VERSION, &driver->version);
            FpgaSpi_ReadRegister(FPGA_REG_STATUS, &driver->status);
        }
    }
}

bool FpgaSpi_ReadRegister(uint8_t addr, uint16_t* value) {
    if (!value) return false;
    // 24-bit frame: [1, addr[6:0]], [dummy_hi], [dummy_lo]
    // In software simulation / hardware stub: return simulated register values
    uint8_t cmd = 0x80 | (addr & 0x7F);
    (void)cmd;

    switch (addr) {
        case FPGA_REG_MAGIC:   *value = FPGA_EXPECTED_MAGIC; return true;
        case FPGA_REG_VERSION: *value = 0x0001; return true;
        case FPGA_REG_STATUS:  *value = 0x0001; return true; // PWM Enabled
        case FPGA_REG_FAULT:   *value = 0x0000; return true;
        default:               *value = 0x0000; return true;
    }
}

bool FpgaSpi_WriteRegister(uint8_t addr, uint16_t value) {
    // 24-bit frame: [0, addr[6:0]], [data_hi], [data_lo]
    uint8_t cmd = 0x00 | (addr & 0x7F);
    (void)cmd;
    (void)value;
    return true;
}

bool FpgaSpi_SetDuties(uint16_t duty_u, uint16_t duty_v, uint16_t duty_w) {
    bool ok = true;
    ok &= FpgaSpi_WriteRegister(FPGA_REG_DUTY_U, duty_u);
    ok &= FpgaSpi_WriteRegister(FPGA_REG_DUTY_V, duty_v);
    ok &= FpgaSpi_WriteRegister(FPGA_REG_DUTY_W, duty_w);
    return ok;
}

bool FpgaSpi_EnablePwm(bool enable) {
    uint16_t ctrl_val = enable ? 0x0001 : 0x0000;
    return FpgaSpi_WriteRegister(FPGA_REG_CTRL, ctrl_val);
}
