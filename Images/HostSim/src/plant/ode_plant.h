#pragma once

#include "plant_backend.h"

namespace hostsim {

class OdePlant : public IPlant {
public:
    OdePlant() = default;
    ~OdePlant() override = default;

    void SetParams(const MotorParams& params) override { motor_.SetParams(params); }
    void Reset() override { motor_.Reset(); }
    void Step(float duty_u_pct, float duty_v_pct, float duty_w_pct, float dt_s) override {
        motor_.Step(duty_u_pct, duty_v_pct, duty_w_pct, dt_s);
    }

    const MotorState& State() const override { return motor_.State(); }
    float ThetaElectricalDeg() const override { return motor_.ThetaElectricalDeg(); }
    float OmegaElectricalRadPerSec() const override { return motor_.OmegaElectricalRadPerSec(); }

    MotorModel& Model() { return motor_; }
    const MotorModel& Model() const { return motor_; }

private:
    MotorModel motor_{};
};

} // namespace hostsim
