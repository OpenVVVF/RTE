#pragma once

/**
 * @brief Runtime parameter descriptor for RTE-generated code.
 *
 * Generated domain files emit a table of these so the base image can
 * get/set node parameters by name at runtime.
 */
struct RteParamDesc {
    const char* name;
    void (*set)(void* state, float value);
    float (*get)(const void* state);
};
