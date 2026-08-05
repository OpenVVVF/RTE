// FpgaSpiDriver.h — MCU SPI2 Master Driver for Tang Nano 20K FPGA Co-Processor
#ifndef FPGA_SPI_DRIVER_H
#define FPGA_SPI_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register Addresses matching Fpga/TangNano20k/docs/register_map.md
#define FPGA_REG_MAGIC        0x00
#define FPGA_REG_VERSION      0x01
#define FPGA_REG_STATUS       0x02
#define FPGA_REG_FAULT        0x03
#define FPGA_REG_CTRL         0x04
#define FPGA_REG_FREQ_HZ      0x05
#define FPGA_REG_DEADTIME_NS  0x06
#define FPGA_REG_DUTY_U       0x07
#define FPGA_REG_DUTY_V       0x08
#define FPGA_REG_DUTY_W       0x09
#define FPGA_REG_SCRATCH      0x0A

#define FPGA_EXPECTED_MAGIC   0x544E  // ASCII "TN"

typedef struct {
    uint16_t magic;
    uint16_t version;
    uint16_t status;
    uint16_t fault;
    bool     is_initialized;
} FpgaDriverState;

// API Prototypes
void     FpgaSpi_Init(FpgaDriverState* driver);
bool     FpgaSpi_ReadRegister(uint8_t addr, uint16_t* value);
bool     FpgaSpi_WriteRegister(uint8_t addr, uint16_t value);
bool     FpgaSpi_SetDuties(uint16_t duty_u, uint16_t duty_v, uint16_t duty_w);
bool     FpgaSpi_EnablePwm(bool enable);

#ifdef __cplusplus
}
#endif

#endif // FPGA_SPI_DRIVER_H
