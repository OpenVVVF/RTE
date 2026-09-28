#include "safety_policy.h"

#include <assert.h>
#include <stdint.h>

static SafetyInputs healthy(void)
{
    return (SafetyInputs){
        .own_power_feedback = false,
        .main_power_feedback = true,
        .gate_fault = false,
        .main_alive = true
    };
}

static SafetyPolicy powering(uint32_t now_ms)
{
    SafetyPolicy policy;
    SafetyPolicy_Init(&policy);
    SafetyPolicy_PostPassed(&policy);
    assert(SafetyPolicy_RequestArm(&policy, healthy(), now_ms));
    assert(policy.state == SAFETY_POWERING);
    assert(SafetyPolicy_PowerEnabled(&policy));
    return policy;
}

static void test_boot_is_inhibited(void)
{
    SafetyPolicy policy;
    SafetyPolicy_Init(&policy);
    assert(policy.state == SAFETY_INHIBITED);
    assert(!SafetyPolicy_PowerEnabled(&policy));
    assert(!SafetyPolicy_RequestArm(&policy, healthy(), 0));
    SafetyPolicy_Update(&policy, healthy(), 1000);
    assert(policy.state == SAFETY_INHIBITED);
}

static void test_arm_requires_independent_conditions(void)
{
    SafetyPolicy policy;
    SafetyInputs inputs = healthy();
    SafetyPolicy_Init(&policy);
    SafetyPolicy_PostPassed(&policy);
    inputs.main_alive = false;
    assert(!SafetyPolicy_RequestArm(&policy, inputs, 1));
    inputs = healthy();
    inputs.main_power_feedback = false;
    assert(!SafetyPolicy_RequestArm(&policy, inputs, 1));
    inputs = healthy();
    inputs.gate_fault = true;
    assert(!SafetyPolicy_RequestArm(&policy, inputs, 1));
    inputs = healthy();
    inputs.own_power_feedback = true;
    assert(!SafetyPolicy_RequestArm(&policy, inputs, 1));
    assert(policy.state == SAFETY_FAULT_LATCHED);
    assert(policy.faults & SAFETY_FAULT_POWER_STUCK_ON);
    assert(policy.trip_reason == SAFETY_TRIP_POWER_STUCK_ON);
}

static void test_power_up_and_loss_of_feedback(void)
{
    SafetyPolicy policy = powering(10);
    SafetyInputs inputs = healthy();
    SafetyPolicy_Update(&policy, inputs, 159);
    assert(policy.state == SAFETY_POWERING);
    inputs.own_power_feedback = true;
    SafetyPolicy_Update(&policy, inputs, 160);
    assert(policy.state == SAFETY_ARMED);
    inputs.own_power_feedback = false;
    SafetyPolicy_Update(&policy, inputs, 161);
    assert(policy.state == SAFETY_FAULT_LATCHED);
    assert(policy.faults & SAFETY_FAULT_POWER_NO_FEEDBACK);
    assert(policy.trip_reason == SAFETY_TRIP_OWN_POWER_FEEDBACK_LOST);
    assert(policy.trip_inputs == 0x0au);
    assert(!SafetyPolicy_PowerEnabled(&policy));
}

static void test_startup_timeout_and_wrap(void)
{
    SafetyPolicy policy = powering(UINT32_MAX - 10u);
    SafetyPolicy_Update(&policy, healthy(), 138u);
    assert(policy.state == SAFETY_POWERING);
    SafetyPolicy_Update(&policy, healthy(), 139u);
    assert(policy.state == SAFETY_FAULT_LATCHED);
    assert(policy.faults & SAFETY_FAULT_POWER_NO_FEEDBACK);
    assert(policy.trip_reason == SAFETY_TRIP_OWN_POWER_START_TIMEOUT);

    policy = powering(0);
    SafetyInputs inputs = healthy();
    inputs.own_power_feedback = true;
    SafetyPolicy_Update(&policy, inputs, 150);
    assert(policy.state == SAFETY_ARMED);
}

static void test_faults_latch_and_disarm(void)
{
    SafetyPolicy policy = powering(0);
    SafetyInputs inputs = healthy();
    inputs.gate_fault = true;
    SafetyPolicy_Update(&policy, inputs, 1);
    assert(policy.faults & SAFETY_FAULT_GATE_DRIVER);
    assert(policy.trip_reason == SAFETY_TRIP_GATE_DRIVER);
    SafetyPolicy_Update(&policy, healthy(), 2);
    SafetyPolicy_Disarm(&policy);
    SafetyPolicy_PostPassed(&policy);
    assert(!SafetyPolicy_RequestArm(&policy, healthy(), 3));

    policy = powering(0);
    inputs = healthy();
    inputs.main_alive = false;
    SafetyPolicy_Update(&policy, inputs, 1);
    assert(policy.faults & SAFETY_FAULT_MAIN_LOST);
    assert(policy.trip_reason == SAFETY_TRIP_MAIN_HEARTBEAT_LOST);

    policy = powering(0);
    inputs = healthy();
    inputs.main_power_feedback = false;
    SafetyPolicy_Update(&policy, inputs, 1);
    assert(policy.faults & SAFETY_FAULT_POWER_NO_FEEDBACK);
    assert(policy.trip_reason == SAFETY_TRIP_MAIN_POWER_FEEDBACK_LOST);

    policy = powering(0);
    inputs = healthy();
    inputs.own_power_feedback = true;
    SafetyPolicy_Update(&policy, inputs, 1);
    SafetyPolicy_Update(&policy, inputs, 2);
    assert(policy.state == SAFETY_ARMED);

    SafetyPolicy_Init(&policy);
    SafetyPolicy_Update(&policy, inputs, 0);
    assert(policy.faults & SAFETY_FAULT_POWER_STUCK_ON);

    SafetyPolicy_Init(&policy);
    SafetyPolicy_PostPassed(&policy);
    SafetyPolicy_Disarm(&policy);
    assert(!SafetyPolicy_RequestArm(&policy, healthy(), 0));

    policy = powering(0);
    SafetyPolicy_Disarm(&policy);
    assert(policy.state == SAFETY_INHIBITED);
    assert(!SafetyPolicy_PowerEnabled(&policy));
    assert(!SafetyPolicy_RequestArm(&policy, healthy(), 1));

    policy = powering(0);
    SafetyPolicy_Trip(&policy, SAFETY_FAULT_INTERNAL);
    assert(policy.state == SAFETY_FAULT_LATCHED);
    assert(policy.faults & SAFETY_FAULT_INTERNAL);
    assert(!SafetyPolicy_PowerEnabled(&policy));
}

static void test_clear_requires_live_safe_inputs(void)
{
    SafetyPolicy policy = powering(0);
    SafetyInputs inputs = healthy();
    SafetyPolicy_Trip(&policy, SAFETY_FAULT_GATE_DRIVER);
    inputs.main_alive = false;
    assert(!SafetyPolicy_TryClear(&policy, inputs));
    inputs.main_alive = true;
    inputs.gate_fault = true;
    assert(!SafetyPolicy_TryClear(&policy, inputs));
    inputs.gate_fault = false;
    inputs.own_power_feedback = true;
    assert(!SafetyPolicy_TryClear(&policy, inputs));
    inputs.own_power_feedback = false;
    assert(SafetyPolicy_TryClear(&policy, inputs));
    assert(policy.state == SAFETY_INHIBITED);
    assert(policy.faults == 0u);
    assert(!SafetyPolicy_PowerEnabled(&policy));
    assert(!SafetyPolicy_TryClear(&policy, inputs));
}

int main(void)
{
    test_boot_is_inhibited();
    test_arm_requires_independent_conditions();
    test_power_up_and_loss_of_feedback();
    test_startup_timeout_and_wrap();
    test_faults_latch_and_disarm();
    test_clear_requires_live_safe_inputs();
    return 0;
}
