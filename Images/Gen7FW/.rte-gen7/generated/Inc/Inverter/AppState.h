#pragma once
#include "../../generated/domain_vsense_generated.h"
#include "../../generated/domain_adc_isr_generated.h"
#include "../../generated/domain_tim_isr_generated.h"
#include "../../generated/domain_app_loop_generated.h"

/* ============================================================================
 * RTE codegen top-level state container.
 *
 * RTECodeEmitter looks for // RTE_EMIT: <domain> state markers and replaces
 * them with a forward declaration in namespace app (e.g.
 *   namespace app { struct AppLoopState; }
 * ).
 *
 * The tool also adds an #include for the generated domain header at the top of
 * this file (e.g. #include "../../generated/domain_app_loop_generated.h").
 *
 * This header is intended for C++ files only.  The generated structs live in
 * namespace app and are named <DomainTitle>State (app_loop -> AppLoopState).
 * ============================================================================ */

namespace app {
    struct AppLoopState;
}
namespace app {
    struct TimIsrState;
}
namespace app {
    struct AdcIsrState;
}
namespace app {
    struct VsenseState;
}

/**
 * @brief Aggregates per-domain state for all RTE-generated timing domains.
 *
 * Member names must exactly match the timing-domain names in the NodeAPI graph.
 * The default global state variable name is appState and can be overridden with
 * RTECodeEmitter's --state-variable flag.
 */
struct AppState {
    app::AppLoopState app_loop;
    app::TimIsrState  tim_isr;
    app::AdcIsrState  adc_isr;
    app::VsenseState  vsense;
};

/**
 * @brief Global RTE state variable.
 *
 * Referenced by app::<DomainTitle>Init/Step calls inserted at // RTE_EMIT
 * markers.  Individual domain structs are owned by the generated code.
 */
extern AppState appState;

