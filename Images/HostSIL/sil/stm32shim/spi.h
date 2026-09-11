/* spi.h — SIL CubeMX-style header: SPI peripheral handle externs. */
#ifndef SIL_SPI_H
#define SIL_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern SPI_HandleTypeDef hspi2;
extern SPI_HandleTypeDef hspi4;

void MX_SPI2_Init(void);
void MX_SPI4_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_SPI_H */
