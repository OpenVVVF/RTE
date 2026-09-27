#include "safety_heartbeat.h"

/* The H7 changes PD8 every 20 ms. Allow 250 ms for interrupt jitter while
 * still removing gate power on a stalled heartbeat. */
enum { HEARTBEAT_TIMEOUT_MS = 250 };

void SafetyHeartbeat_Init(SafetyHeartbeat *heartbeat, bool level,
                          uint32_t now_ms)
{
    *heartbeat = (SafetyHeartbeat){
        .last_edge_ms = now_ms,
        .previous_level = level,
        .initialized = true
    };
}

bool SafetyHeartbeat_Update(SafetyHeartbeat *heartbeat, bool level,
                            uint32_t now_ms)
{
    if (!heartbeat->initialized) {
        SafetyHeartbeat_Init(heartbeat, level, now_ms);
        return false;
    }
    if (level != heartbeat->previous_level) {
        heartbeat->previous_level = level;
        heartbeat->last_edge_ms = now_ms;
        if (heartbeat->edge_count < 2u) {
            ++heartbeat->edge_count;
        }
    }
    if ((uint32_t)(now_ms - heartbeat->last_edge_ms) > HEARTBEAT_TIMEOUT_MS) {
        heartbeat->edge_count = 0u;
    }
    return heartbeat->edge_count >= 2u;
}
