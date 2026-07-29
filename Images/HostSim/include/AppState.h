#pragma once

/* ============================================================================
 * RTE codegen top-level state container (HostSim base image).
 *
 * RTECodeEmitter looks for // RTE_EMIT: <domain> state markers and replaces
 * them with a forward declaration in namespace app plus an #include for the
 * generated domain header.
 * ============================================================================ */

// RTE_EMIT: app_loop state
// RTE_EMIT: tim_isr state
// RTE_EMIT: adc_isr state

#if !__has_include("../generated/domain_app_loop_generated.h")
namespace app { struct AppLoopState {}; }
#endif
#if !__has_include("../generated/domain_tim_isr_generated.h")
namespace app { struct TimIsrState {}; }
#endif
#if !__has_include("../generated/domain_adc_isr_generated.h")
namespace app { struct AdcIsrState {}; }
#endif

struct AppState {
    app::AppLoopState app_loop;
    app::TimIsrState  tim_isr;
    app::AdcIsrState  adc_isr;
};

extern AppState appState;
