#include "safety_heartbeat.h"
#include "safety_policy.h"

#include <assert.h>
#include <stdint.h>

/* The H7 starts its SysTick heartbeat before its long current-sensor warmup.
 * PWR1 rises later; PWR2 feedback then follows the G474 enable. The gate
 * drivers stay in reset during warmup, so READY is deliberately not an arm
 * condition. This reproduces the normal boot ordering across both MCUs. */
int main(void)
{
    SafetyHeartbeat heartbeat;
    SafetyPolicy policy;
    SafetyHeartbeat_Init(&heartbeat, false, 0u);
    SafetyPolicy_Init(&policy);

    bool heartbeat_level = false;
    for (uint32_t now_ms = 0u; now_ms <= 4000u; now_ms += 10u) {
        if (now_ms != 0u && now_ms % 20u == 0u) {
            heartbeat_level = !heartbeat_level;
        }
        const SafetyInputs inputs = {
            .own_power_feedback = now_ms >= 540u,
            .main_power_feedback = now_ms >= 500u,
            .gate_fault = false,
            .main_alive = SafetyHeartbeat_Update(&heartbeat,
                                                  heartbeat_level, now_ms)
        };

        SafetyPolicy_Update(&policy, inputs, now_ms);
        if (policy.state == SAFETY_INHIBITED && !inputs.own_power_feedback) {
            SafetyPolicy_PostPassed(&policy);
            (void)SafetyPolicy_RequestArm(&policy, inputs, now_ms);
        }

        if (now_ms < 500u) {
            assert(policy.state == SAFETY_INHIBITED);
            assert(!SafetyPolicy_PowerEnabled(&policy));
        }
        if (now_ms >= 540u) {
            assert(policy.state == SAFETY_ARMED);
            assert(SafetyPolicy_PowerEnabled(&policy));
        }
        assert(policy.faults == 0u);
    }
    return 0;
}
