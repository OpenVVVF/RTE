#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "simulation/Transforms.h"

namespace simulation {

/** Eight valid two-level inverter switching states (upper switch ON = 1). */
enum class SwitchingState : std::uint8_t {
    S000 = 0,  // 000 null
    S100 = 1,  // 100
    S110 = 2,  // 110
    S010 = 3,  // 010
    S011 = 4,  // 011
    S001 = 5,  // 001
    S101 = 6,  // 101
    S111 = 7,  // 111 null
};

struct SwitchCommand {
    bool sa = false;
    bool sb = false;
    bool sc = false;
};

inline SwitchCommand switchingStateToCommand(SwitchingState state) {
    switch (state) {
        case SwitchingState::S000: return {false, false, false};
        case SwitchingState::S100: return {true, false, false};
        case SwitchingState::S110: return {true, true, false};
        case SwitchingState::S010: return {false, true, false};
        case SwitchingState::S011: return {false, true, true};
        case SwitchingState::S001: return {false, false, true};
        case SwitchingState::S101: return {true, false, true};
        case SwitchingState::S111: return {true, true, true};
    }
    return {};
}

inline SwitchingState commandToSwitchingState(const SwitchCommand& cmd) {
    const int index = (cmd.sa ? 1 : 0) | ((cmd.sb ? 2 : 0)) | ((cmd.sc ? 4 : 0));
    return static_cast<SwitchingState>(index);
}

inline bool isValidSwitchingState(SwitchingState state) {
    return static_cast<std::uint8_t>(state) <= 7U;
}

/**
 * Stationary-frame phase-to-neutral voltage from upper-switch states.
 * Zhang et al. (2017) eq. context / standard two-level VSI:
 *   v_alpha = (2/3) Vdc (Sa - (Sb+Sc)/2)
 *   v_beta  = (1/sqrt(3)) Vdc (Sb - Sc)
 *
 * RTE hardware path uses averaged SVPWM duties (math.svpwm), not discrete states.
 * This model is the FCS-MPCC plant interface used in this project.
 */
inline AlphaBeta voltageAlphaBetaFromSwitches(float vdc, const SwitchCommand& cmd) {
    if (!(vdc > 0.0f) || !std::isfinite(vdc)) {
        return {};
    }
    const float sa = cmd.sa ? 1.0f : 0.0f;
    const float sb = cmd.sb ? 1.0f : 0.0f;
    const float sc = cmd.sc ? 1.0f : 0.0f;
    AlphaBeta out;
    out.alpha = (2.0f / 3.0f) * vdc * (sa - 0.5f * (sb + sc));
    out.beta = (vdc / kSqrt3) * (sb - sc);
    return out;
}

inline AlphaBeta voltageAlphaBetaFromState(float vdc, SwitchingState state) {
    return voltageAlphaBetaFromSwitches(vdc, switchingStateToCommand(state));
}

inline int countSwitchTransitions(const SwitchCommand& prev, const SwitchCommand& next) {
    return (prev.sa != next.sa ? 1 : 0) + (prev.sb != next.sb ? 1 : 0) + (prev.sc != next.sc ? 1 : 0);
}

inline constexpr std::array<SwitchingState, 8> kAllSwitchingStates = {
    SwitchingState::S000, SwitchingState::S100, SwitchingState::S110, SwitchingState::S010,
    SwitchingState::S011, SwitchingState::S001, SwitchingState::S101, SwitchingState::S111,
};

}  // namespace simulation
