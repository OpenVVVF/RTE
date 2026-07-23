#pragma once

/* ============================================================================
 * RTE codegen top-level state container.
 *
 * RTECodeEmitter looks for // RTE_EMIT: <domain> state markers and replaces
 * them with a forward declaration plus an #include of the generated domain
 * header (e.g. generated/domain_app_loop_generated.h).
 *
 * Only C++ files should include this header.  C files (main.c, HAL callbacks
 * left in .c, etc.) do not need access to AppState.
 * ============================================================================ */

// RTE_EMIT: app_loop state
// RTE_EMIT: tim_isr state
// RTE_EMIT: adc_isr state

/**
 * @brief Aggregates per-domain state for all RTE-generated timing domains.
 *
 * The exact layout matches the domains declared in the NodeAPI graph.  The
 * default state variable name is `appState` and can be overridden with
 * RTECodeEmitter's --state-variable flag.
 */
struct AppState {
    struct AppAppLoopState app_loop;
    struct AppTimIsrState  tim_isr;
    struct AppAdcIsrState  adc_isr;
};

/**
 * @brief Global RTE state variable.
 *
 * Accessed from the main loop and from ISRs.  Individual domain structs are
 * owned by the generated code; the base image only declares and passes them
 * to App<Domain>Init / App<Domain>Step.
 */
extern struct AppState appState;
