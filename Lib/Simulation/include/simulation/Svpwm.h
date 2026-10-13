#pragma once

#include <algorithm>
#include <cmath>

#include "simulation/Transforms.h"

namespace simulation {

/** Averaged SVPWM — matches RTE `math.svpwm/inline.cpp` convention. */
inline AlphaBeta svpwmAlphaBeta(float valpha_ref, float vbeta_ref, float vdc) {
    AlphaBeta out{};
    if (!(vdc > 0.0f)) {
        return out;
    }

    const float v_max_linear = (vdc / kSqrt3) * 0.95f;
    float valpha = valpha_ref;
    float vbeta = vbeta_ref;
    const float v_sq = valpha * valpha + vbeta * vbeta;
    if (v_sq > v_max_linear * v_max_linear && v_sq > 1e-12f) {
        const float scale = v_max_linear / std::sqrt(v_sq);
        valpha *= scale;
        vbeta *= scale;
    }

    out.alpha = valpha;
    out.beta = vbeta;
    return out;
}

}  // namespace simulation
