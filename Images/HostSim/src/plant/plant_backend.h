#pragma once

#include "../motor_model.h"

#include <memory>
#include <string>

namespace hostsim {

class IPlant {
public:
    virtual ~IPlant() = default;

    virtual void SetParams(const MotorParams& params) = 0;
    virtual void Reset() = 0;
    virtual void Step(float duty_u_pct, float duty_v_pct, float duty_w_pct, float dt_s) = 0;

    virtual const MotorState& State() const = 0;
    virtual float ThetaElectricalDeg() const = 0;
    virtual float OmegaElectricalRadPerSec() const = 0;
};

std::unique_ptr<IPlant> CreatePlantBackend(const std::string& type);

} // namespace hostsim
