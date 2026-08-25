#pragma once

#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Storage/MotorConfigStore.h"

#include <cstdint>

namespace Inverter {
namespace CalKvStore {

/**
 * @brief Bridge between calibration results and the RTE KV store.
 *
 * Each save function writes that stage's results as RteParamStore keys in the
 * Motor.* namespace (RAM cache only).  Call flush() once when a calibration
 * run finishes to persist everything to FRAM.  Graph config nodes pick the
 * values up at boot (or via 'config load' style re-init).
 */

/** Create Motor.Type (1=PMSM) and Motor.Encoder.Type (1=SinCos) if absent. */
void ensureBaseInfo();

/** Read the stored motor type, falling back to PMSM if unset or invalid. */
MotorType storedMotorType();

void savePoleResults(float poles, float encoderCyclesPerRev);

/** offsetMechDeg is converted to electrical degrees (x pole pairs) and stored
 *  under Motor.Encoder.SinCos.OffsetDeg. */
void saveEncoderResults(float offsetMechDeg, float sign, float cyclesPerRev,
                        float poles);

void saveResistanceResults(float uv, float uw, float vw, float avg);

void saveInductanceResults(float ldHenry, float lqHenry);

void saveInductionResults(float sigmaLsHenry, float rotorTauMs,
                          float lmHenry, float lrHenry,
                          float rrOhm, float lLeakHenry);

void saveFluxResults(float fluxWb);

/** Persist learned encoder sin/cos envelope bounds. */
void saveEncoderBounds(uint16_t sinMin, uint16_t sinMax,
                       uint16_t cosMin, uint16_t cosMax);

/** Restore saved bounds into the decoder.  Returns false if not stored. */
bool loadEncoderBounds();

/** Persist measured breakaway modulation. */
void saveBreakaway(float breakawayMod);

/** Persist a fitted sin/cos ellipse correction. */
void saveEncoderFit(const EncoderADC::SinCosFit& fit);

/** Restore a saved sin/cos ellipse correction into the decoder. */
bool loadEncoderFit();

/** Populate the RAM MotorCalibration struct from KV store values (angle
 * offset/sign, poles, resistance) so legacy-FOC-driven code uses the real
 * calibrated values instead of the debug defaults (wrong sign/offset).
 * OffsetDeg (electrical) is converted back to mechanical degrees. */
bool loadMotorCalibration();

/** Persist the RAM cache to FRAM. */
bool flush();

} // namespace CalKvStore
} // namespace Inverter
