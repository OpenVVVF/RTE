#include "safety_heartbeat.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    SafetyHeartbeat heartbeat = {0};
    assert(!SafetyHeartbeat_Update(&heartbeat, false, 0));
    assert(!SafetyHeartbeat_Update(&heartbeat, false, 20));
    assert(!SafetyHeartbeat_Update(&heartbeat, true, 40));
    assert(SafetyHeartbeat_Update(&heartbeat, false, 60));
    assert(SafetyHeartbeat_Update(&heartbeat, false, 310));
    assert(!SafetyHeartbeat_Update(&heartbeat, false, 311));
    assert(!SafetyHeartbeat_Update(&heartbeat, true, 330));
    assert(SafetyHeartbeat_Update(&heartbeat, false, 350));

    SafetyHeartbeat_Init(&heartbeat, false, UINT32_MAX - 10u);
    assert(!SafetyHeartbeat_Update(&heartbeat, true, UINT32_MAX));
    assert(SafetyHeartbeat_Update(&heartbeat, false, 10));
    assert(!SafetyHeartbeat_Update(&heartbeat, false, 270));
    return 0;
}
