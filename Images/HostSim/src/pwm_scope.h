#pragma once

namespace hostsim {

/* Triangle-carrier PWM scope model: converts slow-updating duty commands into
 * switched gate signals and phase voltages for oscilloscope-style comparison.
 * Runs in parallel with the averaged-duty motor plant. */
class PwmScope {
public:
    void SetCarrierHz(float hz) { carrier_hz_ = hz > 0.0f ? hz : 0.0f; }
    float CarrierHz() const { return carrier_hz_; }

    void SetVdc(float vdc) { vdc_ = vdc > 0.0f ? vdc : 0.0f; }

    void SetDuties(float duty_u_pct, float duty_v_pct, float duty_w_pct);

    /* Advance the carrier over dt_s (sub-steps internally for accuracy). */
    void AdvanceInterval(float dt_s);

    float GateU() const { return gate_u_; }
    float GateV() const { return gate_v_; }
    float GateW() const { return gate_w_; }

    /* Phase voltage to DC- (0 or Vdc with high-side switch model). */
    float VoltageU() const { return v_u_; }
    float VoltageV() const { return v_v_; }
    float VoltageW() const { return v_w_; }

    /* Line-line voltages (scope CH1-CH2 style). */
    float VoltageUV() const { return v_uv_; }
    float VoltageVW() const { return v_vw_; }
    float VoltageWU() const { return v_wu_; }

private:
    void AdvanceOnce(float dt_s);

    float carrier_hz_ = 2000.0f;
    float vdc_ = 48.0f;
    float duty_u_ = 0.0f;
    float duty_v_ = 0.0f;
    float duty_w_ = 0.0f;

    float phase_ = 0.0f; /* 0..1 within one PWM period */
    float gate_u_ = 0.0f;
    float gate_v_ = 0.0f;
    float gate_w_ = 0.0f;
    float v_u_ = 0.0f;
    float v_v_ = 0.0f;
    float v_w_ = 0.0f;
    float v_uv_ = 0.0f;
    float v_vw_ = 0.0f;
    float v_wu_ = 0.0f;
};

PwmScope& GlobalPwmScope();

} // namespace hostsim
