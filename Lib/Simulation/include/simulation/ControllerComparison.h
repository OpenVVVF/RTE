#pragma once

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "simulation/FocController.h"
#include "simulation/MpccController.h"
#include "simulation/PmsmPlant.h"

namespace simulation {

enum class ControllerType { MPCC, FOC };

struct ComparisonScenario {
    std::string name = "comparison";
    double duration = 0.1;
    float ts = 100e-6f;
    float vdc = 540.0f;
    float id_ref = 0.0f;
    float iq_ref = 10.0f;
    MPCCMode mpcc_mode = MPCCMode::ConventionalOneStep;
    /** Load torque [Nm] as a function of time [s]. */
    std::function<float(double)> load_torque = [](double) { return 0.0f; };
};

struct ComparisonSample {
    double time = 0.0;
    std::string controller;
    float ia = 0.0f;
    float ib = 0.0f;
    float ic = 0.0f;
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float omega_m = 0.0f;
    float torque_em = 0.0f;
    float load_torque = 0.0f;
    float valpha = 0.0f;
    float vbeta = 0.0f;
};

class ControllerComparison {
public:
    explicit ControllerComparison(PmsmParameters motor) : motor_(motor) {}

    std::vector<ComparisonSample> run(ControllerType type, const ComparisonScenario& scenario) {
        PmsmPlant plant(motor_);
        plant.reset();

        MpccController mpcc([&] {
            MpccParameters p;
            p.motor = motor_;
            p.ts = scenario.ts;
            p.mode = scenario.mpcc_mode;
            return p;
        }());

        FocController foc([&] {
            FocParameters p;
            p.motor = motor_;
            p.ts = scenario.ts;
            return p;
        }());
        foc.reset();

        std::vector<ComparisonSample> samples;
        const int steps = static_cast<int>(scenario.duration / scenario.ts);
        const char* label = (type == ControllerType::MPCC) ? "MPCC" : "FOC";

        for (int k = 0; k < steps; ++k) {
            const double t = k * scenario.ts;
            const auto& st = plant.state();
            const float load = scenario.load_torque(t);

            float valpha = 0.0f;
            float vbeta = 0.0f;

            if (type == ControllerType::MPCC) {
                MpccInputs in;
                in.id = st.id;
                in.iq = st.iq;
                in.id_ref = scenario.id_ref;
                in.iq_ref = scenario.iq_ref;
                in.theta_e = st.theta_e;
                in.omega_e = st.omega_e;
                in.vdc = scenario.vdc;
                in.enable = true;
                const MpccOutputs out = mpcc.update(in);
                valpha = out.valpha;
                vbeta = out.vbeta;
            } else {
                FocInputs in;
                in.id = st.id;
                in.iq = st.iq;
                in.id_ref = scenario.id_ref;
                in.iq_ref = scenario.iq_ref;
                in.theta_e = st.theta_e;
                in.omega_e = st.omega_e;
                in.vdc = scenario.vdc;
                in.enable = true;
                const FocOutputs out = foc.update(in);
                valpha = out.valpha;
                vbeta = out.vbeta;
            }

            plant.stepAlphaBeta(valpha, vbeta, load, scenario.ts);

            ComparisonSample s;
            s.time = t;
            s.controller = label;
            s.ia = plant.state().ia;
            s.ib = plant.state().ib;
            s.ic = plant.state().ic;
            s.id = plant.state().id;
            s.iq = plant.state().iq;
            s.id_ref = scenario.id_ref;
            s.iq_ref = scenario.iq_ref;
            s.omega_m = plant.state().omega_m;
            s.torque_em = plant.state().torque_em;
            s.load_torque = load;
            s.valpha = valpha;
            s.vbeta = vbeta;
            samples.push_back(s);
        }
        return samples;
    }

    static void writeCsv(const std::string& path, const std::vector<ComparisonSample>& samples) {
        std::ofstream out(path);
        out << "time,controller,ia,ib,ic,id,iq,id_reference,iq_reference,mechanical_speed,"
               "electromagnetic_torque,load_torque,v_alpha,v_beta\n";
        for (const auto& s : samples) {
            out << s.time << ',' << s.controller << ',' << s.ia << ',' << s.ib << ',' << s.ic << ','
                << s.id << ',' << s.iq << ',' << s.id_ref << ',' << s.iq_ref << ',' << s.omega_m << ','
                << s.torque_em << ',' << s.load_torque << ',' << s.valpha << ',' << s.vbeta << '\n';
        }
    }

private:
    PmsmParameters motor_;
};

}  // namespace simulation
