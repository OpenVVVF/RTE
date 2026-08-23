#include "Inverter/Calibration/CalKvStore.h"

#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"

#include <cmath>

namespace Inverter {
namespace CalKvStore {

namespace {

/* Motor.* key namespace.  NOTE: RteParamStore keys are limited to 32 chars. */
constexpr const char* KEY_POLES          = "Motor.Poles";
constexpr const char* KEY_ENC_CYCLES     = "Motor.Encoder.SinCos.CyclesRev";
constexpr const char* KEY_ENC_SIN_MIN    = "Motor.Encoder.SinCos.SinMin";
constexpr const char* KEY_ENC_SIN_MAX    = "Motor.Encoder.SinCos.SinMax";
constexpr const char* KEY_ENC_COS_MIN    = "Motor.Encoder.SinCos.CosMin";
constexpr const char* KEY_ENC_COS_MAX    = "Motor.Encoder.SinCos.CosMax";
constexpr const char* KEY_ENC_OFFSETRAD  = "Motor.Encoder.SinCos.OffsetDeg";
constexpr const char* KEY_ENC_SIGN       = "Motor.Encoder.SinCos.Sign";
constexpr const char* KEY_ENC_FIT_CS     = "Motor.Encoder.SinCos.Fit.Cs";
constexpr const char* KEY_ENC_FIT_CC     = "Motor.Encoder.SinCos.Fit.Cc";
constexpr const char* KEY_ENC_FIT_AS     = "Motor.Encoder.SinCos.Fit.As";
constexpr const char* KEY_ENC_FIT_AC     = "Motor.Encoder.SinCos.Fit.Ac";
constexpr const char* KEY_ENC_FIT_PHI    = "Motor.Encoder.SinCos.Fit.Phi";
constexpr const char* KEY_ENC_FIT_VALID  = "Motor.Encoder.SinCos.Fit.Valid";
constexpr const char* KEY_ENC_BREAK_MOD  = "Motor.Encoder.SinCos.BreakMod";
constexpr const char* KEY_RES_UV         = "Motor.Resistance.Uv";
constexpr const char* KEY_RES_UW         = "Motor.Resistance.Uw";
constexpr const char* KEY_RES_VW         = "Motor.Resistance.Vw";
constexpr const char* KEY_RES_AVG        = "Motor.Resistance.Avg";
constexpr const char* KEY_IND_LD         = "Motor.PMSM.Inductance.Ld";
constexpr const char* KEY_IND_LQ         = "Motor.PMSM.Inductance.Lq";
constexpr const char* KEY_FLUX_WB        = "Motor.PMSM.FluxLinkage.Wb";
constexpr const char* KEY_IND_SIGMA_LS   = "Motor.Induction.SigmaLs";
constexpr const char* KEY_IND_TAU_R      = "Motor.Induction.TauR";
constexpr const char* KEY_IND_LM         = "Motor.Induction.Lm";
constexpr const char* KEY_IND_LR         = "Motor.Induction.Lr";
constexpr const char* KEY_IND_RR         = "Motor.Induction.Rr";
constexpr const char* KEY_IND_L_LEAK     = "Motor.Induction.LLeak";
constexpr const char* KEY_MOTOR_TYPE     = "Motor.Type";
constexpr const char* KEY_PHASE_SWAP     = "Motor.PhaseSwap";
constexpr const char* KEY_ENCODER_TYPE   = "Motor.Encoder.Type";

constexpr float MOTOR_TYPE_PMSM     = static_cast<float>(MotorType::PmsmIpm);
constexpr float MOTOR_TYPE_INDUCTION= static_cast<float>(MotorType::Induction);
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

/** Read the stored motor type, falling back to PMSM if unset or invalid. */
MotorType storedMotorType() {
    if (!RteParamStore::isReady()) return MotorType::PmsmIpm;
    float v = 0.0f;
    if (!RteParamStore::get(KEY_MOTOR_TYPE, &v)) return MotorType::PmsmIpm;
    if (v < 0.0f || v > static_cast<float>(MotorType::SlipRing)) return MotorType::PmsmIpm;
    return static_cast<MotorType>(v);
}

void savePoleResults(float poles, float encoderCyclesPerRev) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_POLES, poles);
    setIfValid(KEY_ENC_CYCLES, encoderCyclesPerRev);
}

void saveEncoderResults(float offsetMechDeg, float sign, float cyclesPerRev,
                        float poles) {
    if (!RteParamStore::isReady()) return;
    /* The offset is stored as electrical degrees (the graph ElecAngle formula:
     * theta_elec = OffsetDeg*deg2rad + sign * encoder_angle * poles/2).  The
     * calibrator measures mechanical degrees, so scale by pole pairs. */
    setIfValid(KEY_ENC_OFFSETRAD, offsetMechDeg * poles * 0.5f);
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

void saveInductionResults(float sigmaLsHenry, float rotorTauMs,
                          float lmHenry, float lrHenry,
                          float rrOhm, float lLeakHenry) {
    if (!RteParamStore::isReady()) return;
    RteParamStore::set(KEY_MOTOR_TYPE, MOTOR_TYPE_INDUCTION);
    setIfValid(KEY_IND_SIGMA_LS, sigmaLsHenry);
    setIfValid(KEY_IND_TAU_R, rotorTauMs);
    setIfValid(KEY_IND_LM, lmHenry);
    setIfValid(KEY_IND_LR, lrHenry);
    setIfValid(KEY_IND_RR, rrOhm);
    setIfValid(KEY_IND_L_LEAK, lLeakHenry);
}

void saveFluxResults(float fluxWb) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_FLUX_WB, fluxWb);
}

void saveEncoderBounds(uint16_t sinMin, uint16_t sinMax,
                       uint16_t cosMin, uint16_t cosMax) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_ENC_SIN_MIN, static_cast<float>(sinMin));
    setIfValid(KEY_ENC_SIN_MAX, static_cast<float>(sinMax));
    setIfValid(KEY_ENC_COS_MIN, static_cast<float>(cosMin));
    setIfValid(KEY_ENC_COS_MAX, static_cast<float>(cosMax));
}

bool loadEncoderBounds() {
    if (!RteParamStore::isReady()) return false;
    float sMin, sMax, cMin, cMax;
    if (!RteParamStore::get(KEY_ENC_SIN_MIN, &sMin) ||
        !RteParamStore::get(KEY_ENC_SIN_MAX, &sMax) ||
        !RteParamStore::get(KEY_ENC_COS_MIN, &cMin) ||
        !RteParamStore::get(KEY_ENC_COS_MAX, &cMax)) {
        return false;
    }
    encoderADC().setLearnedBounds(static_cast<uint16_t>(sMin),
                                  static_cast<uint16_t>(sMax),
                                  static_cast<uint16_t>(cMin),
                                  static_cast<uint16_t>(cMax));
    return true;
}

void saveBreakaway(float breakawayMod) {
    if (!RteParamStore::isReady()) return;
    setIfValid(KEY_ENC_BREAK_MOD, breakawayMod);
}

void saveEncoderFit(const EncoderADC::SinCosFit& fit) {
    if (!RteParamStore::isReady()) return;
    if (!fit.valid) return;  /* Keep any previously saved fit; don't erase it. */
    setIfValid(KEY_ENC_FIT_VALID, 1.0f);
    setIfValid(KEY_ENC_FIT_CS,  fit.center_sin);
    setIfValid(KEY_ENC_FIT_CC,  fit.center_cos);
    setIfValid(KEY_ENC_FIT_AS,  fit.amp_sin);
    setIfValid(KEY_ENC_FIT_AC,  fit.amp_cos);
    setIfValid(KEY_ENC_FIT_PHI, fit.phase_err);
}

bool loadEncoderFit() {
    if (!RteParamStore::isReady()) return false;
    float valid_f = 0.0f;
    if (!RteParamStore::get(KEY_ENC_FIT_VALID, &valid_f) || valid_f < 0.5f) {
        return false;
    }
    EncoderADC::SinCosFit fit;
    fit.valid = true;
    if (!RteParamStore::get(KEY_ENC_FIT_CS,  &fit.center_sin) ||
        !RteParamStore::get(KEY_ENC_FIT_CC,  &fit.center_cos) ||
        !RteParamStore::get(KEY_ENC_FIT_AS,  &fit.amp_sin) ||
        !RteParamStore::get(KEY_ENC_FIT_AC,  &fit.amp_cos) ||
        !RteParamStore::get(KEY_ENC_FIT_PHI, &fit.phase_err)) {
        return false;
    }
    fit.phase_err_sin = sinf(fit.phase_err);
    fit.phase_err_cos = cosf(fit.phase_err);
    encoderADC().applyFit(fit);
    return true;
}

bool loadMotorCalibration() {
    if (!RteParamStore::isReady()) return false;
    MotorCalibration& mc = motorCalibration();

    /* Motor type drives calibration branching and must always be refreshed,
     * even for encoderless motors that have no poles/encoder data yet. */
    mc.motor_type = storedMotorType();

    /* Phase-wire swap is independent of encoder presence; apply it from the
     * KV store whenever it has been configured. */
    float phase_swap = 0.0f;
    if (RteParamStore::get(KEY_PHASE_SWAP, &phase_swap)) {
        if (phase_swap >= 0.0f && phase_swap <= 3.0f) {
            mc.phase_swap = static_cast<PhaseSwap>(static_cast<uint8_t>(phase_swap));
        }
    }

    /* Encoder-dependent fields are only loaded when a stored encoder profile
     * exists.  Encoderless induction setups skip these. */
    float poles = 0.0f, cycles = 0.0f, offset_deg = 0.0f, sign = 0.0f;
    bool have_encoder =
        RteParamStore::get("Motor.Poles", &poles) && poles > 0.0f &&
        RteParamStore::get("Motor.Encoder.SinCos.CyclesRev", &cycles) && cycles > 0.0f &&
        RteParamStore::get("Motor.Encoder.SinCos.OffsetDeg", &offset_deg) &&
        RteParamStore::get("Motor.Encoder.SinCos.Sign", &sign);
    if (have_encoder) {
        mc.pole_count = poles;
        mc.encoder_cycles_per_rev = cycles;
        /* KV stores electrical degrees; the struct wants mechanical. */
        mc.encoder_offset_deg = offset_deg / (poles * 0.5f);
        mc.encoder_sign = (sign >= 0.0f) ? 1.0f : -1.0f;
    }

    float r = 0.0f;
    if (RteParamStore::get("Motor.Resistance.Avg", &r) && r > 0.0f) {
        mc.r_phase_uv = r;
        mc.r_phase_uw = r;
        mc.r_phase_vw = r;
        mc.r_phase_avg = r;
    }

    float v = 0.0f;
    if (RteParamStore::get(KEY_IND_SIGMA_LS, &v) && v > 0.0f) mc.sigma_ls_henry = v;
    if (RteParamStore::get(KEY_IND_TAU_R, &v) && v > 0.0f) mc.rotor_time_constant_ms = v;
    if (RteParamStore::get(KEY_IND_LM, &v) && v > 0.0f) mc.lm_henry = v;
    if (RteParamStore::get(KEY_IND_LR, &v) && v > 0.0f) mc.lr_henry = v;
    if (RteParamStore::get(KEY_IND_RR, &v) && v > 0.0f) mc.rr_ohm = v;
    if (RteParamStore::get(KEY_IND_L_LEAK, &v) && v > 0.0f) mc.l_leak_henry = v;

    /* Mark valid for FOC when the encoder prerequisites are present; the
     * resistance stage of an encoderless induction run will set valid=true
     * itself once it has measured R. */
    if (have_encoder) {
        mc.valid = true;
    }
    return true;
}

bool flush() {
    return RteParamStore::isReady() && RteParamStore::flush();
}

} // namespace CalKvStore
} // namespace Inverter
