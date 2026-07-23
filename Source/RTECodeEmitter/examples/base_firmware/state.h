#pragma once

// RTECodeEmitter will add forward declarations and includes here.
// RTE_EMIT: app_loop state
// RTE_EMIT: tim_isr state

struct AppState {
    // These member names must match the timing-domain names in the graph.
    app::AppLoopState app_loop;
    app::TimIsrState tim_isr;
};
