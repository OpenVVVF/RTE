#include "Inverter/Calibration/CalKvStore.h"

#include "Inverter/Drivers/Storage/RteParamStore.h"

#include <cmath>

namespace Inverter {
namespace CalKvStore {

namespace {

/* Motor.* key namespace.  NOTE: RteParamStore keys are limited to 32 chars. */
constexpr const char* KEY_POLES          = "Motor.Poles";
constexpr const char* KEY_ENC_CYCLES     = "Motor.Encoder.SinCos.CyclesRev";
constexpr const char* KEY_ENC_BREAKMOD   = "Motor.Encoder.SinCos.BreakMod";
constexpr const char* KEY_ENC_OFFSETRAD  = "Motor.Encoder.SinCos.OffsetRad";
constexpr const char* KEY_ENC_SIGN       = "Motor.Encoder.SinCos.Sign";
constexpr const char* KEY_RES_UV         = "Motor.Resistance.Uv";
constexpr const char* KEY_RES_UW         = "Motor.Resistance.Uw";
constexpr const char* KEY_RES_VW         = "Motor.Resistance.Vw";
constexpr const char* KEY_RES_AVG        = "Motor.Resistance.Avg";
constexpr const char* KEY_IND_LD         = "Motor.PMSM.Inductance.Ld";
constexpr const char* KEY_IND_LQ         = "Motor.PMSM.Inductance.Lq";
constexpr const char* KEY_FLUX_WB        = "Motor.PMSM.FluxLinkage.Wb";
constexpr const char* KEY_MOTOR_TYPE     = "Motor.Type";
constexpr const char* KEY_ENCODER_TYPE   = "Motor.Encoder.Type";

constexpr float MOTOR_TYPE_PMSM    = 1.0f;
constexpr float ENCODER_TYPE_SINCOS = 1.0f;

void setIfValid(const char* key, float value) {
    if (std::isfinite(value)) {
        RteParamStore::set(key, value);
    }
}

} // namespace

void ensureBaseInfo() {
    if (!RteParamStore::isReady()) return;
    float dummy = 0.0f;
    if (!RteParamStore::get(KEY_MOTOR_TYPE, &dummy)) {
        RteParamStore::set(KEY_MOTOR_TYPE, MOTOR_TYPE_PMSM);
    }
    if (!RteParamStore::get(KEY_ENCODER_TYPE, &dummy)) {
        RteParamStore::set(KEY_ENCODER_TYPE, ENCODER_TYPE_SINCOS);
    }
}

void savePoleResults(float poles, float encoderCyclesPerRev, float breakawayMod) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_POLES, poles);
    setIfValid(KEY_ENC_CYCLES, encoderCyclesPerRev);
    setIfValid(KEY_ENC_BREAKMOD, breakawayMod);
}

void saveEncoderResults(float offsetMechDeg, float sign, float cyclesPerRev,
                        float poles) {
    if (!RteParamStore::isReady()) return;
    (void)poles;
    /* The offset is stored as electrical radians directly (same convention as
     * the base-image FocController and the graph ElecAngle formula:
     * theta_elec = offset_rad + sign * encoder_angle * pole_pairs). */
    constexpr float DEG_TO_RAD = 3.14159265358979f / 180.0f;
    setIfValid(KEY_ENC_OFFSETRAD, offsetMechDeg * DEG_TO_RAD);
    setIfValid(KEY_ENC_SIGN, sign);
    setIfValid(KEY_ENC_CYCLES, cyclesPerRev);
}

void saveResistanceResults(float uv, float uw, float vw, float avg) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_RES_UV, uv);
    setIfValid(KEY_RES_UW, uw);
    setIfValid(KEY_RES_VW, vw);
    setIfValid(KEY_RES_AVG, avg);
}

void saveInductanceResults(float ldHenry, float lqHenry) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_IND_LD, ldHenry);
    setIfValid(KEY_IND_LQ, lqHenry);
}

void saveFluxResults(float fluxWb) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_FLUX_WB, fluxWb);
}

bool flush() {
    return RteParamStore::isReady() && RteParamStore::flush();
}

} // namespace CalKvStore
} // namespace Inverter
