#ifndef SAFETY_FAULT_WIRE_H
#define SAFETY_FAULT_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed ASCII line sent G474 -> H7 on the existing bridged USART3 link:
 * !SF1 + 8 hex fault bits + 1 hex state + 4 hex generation +
 * 1 hex clear result + 2 hex CRC-8 + newline. It is never sent to USB. */
enum { SAFETY_FAULT_WIRE_LENGTH = 21 };

typedef enum {
    SAFETY_CLEAR_NONE = 0,
    SAFETY_CLEAR_ACCEPTED = 1,
    SAFETY_CLEAR_REFUSED = 2,
    SAFETY_CLEAR_NO_FAULT = 3
} SafetyClearResult;

typedef struct {
    uint32_t faults;
    uint16_t generation;
    uint8_t state;
    uint8_t clear_result;
} SafetyFaultFrame;

void SafetyFaultWire_Encode(const SafetyFaultFrame *frame,
                            char out[SAFETY_FAULT_WIRE_LENGTH]);
/* Accepts the 20-character line body after CommandShell removed newline. */
bool SafetyFaultWire_Decode(const char *body, size_t length,
                            SafetyFaultFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
