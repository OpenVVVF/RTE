#ifndef COPROCESSOR_SAFETY_HEARTBEAT_H
#define COPROCESSOR_SAFETY_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

/* PB9 receives main H7 PD8. A static level, including a floating pin held by
 * the input pull-down, cannot authorize gate power. Time uses wrap-safe math. */
typedef struct {
    uint32_t last_edge_ms;
    uint8_t edge_count;
    bool previous_level;
    bool initialized;
} SafetyHeartbeat;

void SafetyHeartbeat_Init(SafetyHeartbeat *heartbeat, bool level,
                          uint32_t now_ms);
bool SafetyHeartbeat_Update(SafetyHeartbeat *heartbeat, bool level,
                            uint32_t now_ms);

#endif
