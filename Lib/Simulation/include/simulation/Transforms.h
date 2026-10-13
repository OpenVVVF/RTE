#pragma once

#include <cmath>

namespace simulation {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kInvSqrt3 = 0.5773502691896258f;
inline constexpr float kSqrt3 = 1.7320508075688772f;

/** RTE graph convention (math.clarke / math.park / math.inverse_park). */
struct AlphaBeta {
    float alpha = 0.0f;
    float beta = 0.0f;
};

struct Dq {
    float d = 0.0f;
    float q = 0.0f;
};

struct Abc {
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
};

inline void clarkeAbcToAlphaBeta(float ia, float ib, float ic, AlphaBeta& out) {
    out.alpha = ia;
    out.beta = (ib - ic) * kInvSqrt3;
}

inline void parkAlphaBetaToDq(const AlphaBeta& in, float theta, Dq& out) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    out.d = in.alpha * c + in.beta * s;
    out.q = -in.alpha * s + in.beta * c;
}

inline void inverseParkDqToAlphaBeta(const Dq& in, float theta, AlphaBeta& out) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    out.alpha = in.d * c - in.q * s;
    out.beta = in.d * s + in.q * c;
}

inline void inverseClarkeAlphaBetaToAbc(const AlphaBeta& in, Abc& out) {
    out.a = in.alpha;
    out.b = -0.5f * in.alpha + 0.5f * kSqrt3 * in.beta;
    out.c = -0.5f * in.alpha - 0.5f * kSqrt3 * in.beta;
}

inline float wrapAngle0TwoPi(float rad) {
    rad = std::fmod(rad, kTwoPi);
    if (rad < 0.0f) {
        rad += kTwoPi;
    }
    return rad;
}

}  // namespace simulation
