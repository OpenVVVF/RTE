#pragma once

/**
 * @brief Runtime parameter descriptor for RTE-generated code (HostSim stub).
 *
 * Generated domain files may emit a table of these so the base image can
 * get/set node parameters by name at runtime. HostSim does not implement a
 * parameter shell yet; the struct must exist for codegen compatibility.
 */
struct RteParamDesc {
    const char* name;
    void (*set)(void* state, float value);
    float (*get)(const void* state);
};
