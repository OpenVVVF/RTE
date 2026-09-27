#include "safety_policy.h"

/* A missing power-switch feedback after enable is a startup fault. */
enum { POWER_STARTUP_TIMEOUT_MS = 150 };

void SafetyPolicy_Init(SafetyPolicy *policy)
{
    *policy = (SafetyPolicy){ .state = SAFETY_INHIBITED };
}

void SafetyPolicy_PostPassed(SafetyPolicy *policy)
{
    if (policy->state == SAFETY_INHIBITED && policy->faults == 0u) {
        policy->post_passed = true;
    }
}

void SafetyPolicy_Trip(SafetyPolicy *policy, SafetyFault fault)
{
    policy->faults |= (uint32_t)fault;
    policy->state = SAFETY_FAULT_LATCHED;
    policy->post_passed = false;
}

bool SafetyPolicy_RequestArm(SafetyPolicy *policy, SafetyInputs inputs,
                             uint32_t now_ms)
{
    if (policy->state != SAFETY_INHIBITED || !policy->post_passed ||
        !inputs.main_alive || !inputs.main_power_feedback || inputs.gate_fault) {
        return false;
    }
    if (inputs.own_power_feedback) {
        SafetyPolicy_Trip(policy, SAFETY_FAULT_POWER_STUCK_ON);
        return false;
    }
    policy->powering_since_ms = now_ms;
    policy->state = SAFETY_POWERING;
    return true;
}

void SafetyPolicy_Update(SafetyPolicy *policy, SafetyInputs inputs,
                         uint32_t now_ms)
{
    if (policy->state == SAFETY_FAULT_LATCHED) {
        return;
    }
    if (policy->state == SAFETY_INHIBITED) {
        if (inputs.own_power_feedback) {
            SafetyPolicy_Trip(policy, SAFETY_FAULT_POWER_STUCK_ON);
        }
        return;
    }
    if (!inputs.main_alive) {
        SafetyPolicy_Trip(policy, SAFETY_FAULT_MAIN_LOST);
    } else if (inputs.gate_fault) {
        SafetyPolicy_Trip(policy, SAFETY_FAULT_GATE_DRIVER);
    } else if (!inputs.main_power_feedback) {
        SafetyPolicy_Trip(policy, SAFETY_FAULT_POWER_NO_FEEDBACK);
    } else if (policy->state == SAFETY_ARMED) {
        if (!inputs.own_power_feedback) {
            SafetyPolicy_Trip(policy, SAFETY_FAULT_POWER_NO_FEEDBACK);
        }
    } else if (inputs.own_power_feedback) {
        policy->state = SAFETY_ARMED;
    } else if ((uint32_t)(now_ms - policy->powering_since_ms) >=
               POWER_STARTUP_TIMEOUT_MS) {
        SafetyPolicy_Trip(policy, SAFETY_FAULT_POWER_NO_FEEDBACK);
    }
}

void SafetyPolicy_Disarm(SafetyPolicy *policy)
{
    if (policy->state != SAFETY_FAULT_LATCHED) {
        policy->state = SAFETY_INHIBITED;
        policy->post_passed = false;
    }
}

bool SafetyPolicy_TryClear(SafetyPolicy *policy, SafetyInputs inputs)
{
    if (policy->state != SAFETY_FAULT_LATCHED ||
        inputs.own_power_feedback || inputs.gate_fault ||
        !inputs.main_alive) {
        return false;
    }
    SafetyPolicy_Init(policy);
    return true;
}

bool SafetyPolicy_PowerEnabled(const SafetyPolicy *policy)
{
    return policy->state == SAFETY_POWERING || policy->state == SAFETY_ARMED;
}
