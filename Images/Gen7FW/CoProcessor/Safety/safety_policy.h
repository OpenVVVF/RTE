#ifndef COPROCESSOR_SAFETY_POLICY_H
#define COPROCESSOR_SAFETY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* The policy has no HAL dependency so its transitions can be tested on a host.
 * Times are monotonic milliseconds; subtraction deliberately handles wrap. */
typedef enum {
    SAFETY_INHIBITED,
    SAFETY_POWERING,
    SAFETY_ARMED,
    SAFETY_FAULT_LATCHED
} SafetyState;

typedef enum {
    SAFETY_FAULT_NONE = 0,
    SAFETY_FAULT_GATE_DRIVER = 1u << 0,
    SAFETY_FAULT_POWER_STUCK_ON = 1u << 1,
    SAFETY_FAULT_POWER_NO_FEEDBACK = 1u << 2,
    SAFETY_FAULT_MAIN_LOST = 1u << 4,
    SAFETY_FAULT_INTERNAL = 1u << 5
} SafetyFault;

typedef struct {
    bool own_power_feedback;
    bool main_power_feedback;
    bool gate_fault;    /* Active-low /FAULT pin interpreted by the adapter. */
    bool main_alive;    /* Changing PD8/PB9 heartbeat observed by G474. */
} SafetyInputs;

typedef struct {
    SafetyState state;
    uint32_t faults;
    uint32_t powering_since_ms;
    bool post_passed;
} SafetyPolicy;

/* A clear request is accepted only with the second power switch open, no
 * active gate fault, and a live main processor. Clearing leaves power off;
 * ordinary arming checks still apply on subsequent updates. */
void SafetyPolicy_Init(SafetyPolicy *policy);
void SafetyPolicy_PostPassed(SafetyPolicy *policy);
bool SafetyPolicy_RequestArm(SafetyPolicy *policy, SafetyInputs inputs,
                             uint32_t now_ms);
void SafetyPolicy_Update(SafetyPolicy *policy, SafetyInputs inputs,
                         uint32_t now_ms);
void SafetyPolicy_Trip(SafetyPolicy *policy, SafetyFault fault);
void SafetyPolicy_Disarm(SafetyPolicy *policy);
bool SafetyPolicy_TryClear(SafetyPolicy *policy, SafetyInputs inputs);
bool SafetyPolicy_PowerEnabled(const SafetyPolicy *policy);

#endif
