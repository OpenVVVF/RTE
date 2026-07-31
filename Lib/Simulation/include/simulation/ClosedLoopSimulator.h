#pragma once

#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "simulation/MpccController.h"
#include "simulation/PmsmPlant.h"
#include "simulation/TwoLevelInverter.h"
#include "simulation/Transforms.h"

namespace simulation {

struct SimulationSample {
    double time = 0.0;
    float ia = 0.0f;
    float ib = 0.0f;
    float ic = 0.0f;
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float omega_m = 0.0f;
    float omega_e = 0.0f;
    float theta_m = 0.0f;
    float theta_e = 0.0f;
    float torque_em = 0.0f;
    float load_torque = 0.0f;
    float vdc = 0.0f;
    bool sa = false;
    bool sb = false;
    bool sc = false;
    int switching_state = 0;
    float valpha = 0.0f;
    float vbeta = 0.0f;
    float vd = 0.0f;
    float vq = 0.0f;
    float predicted_id = 0.0f;
    float predicted_iq = 0.0f;
    float cost = 0.0f;
    double controller_exec_us = 0.0;
};

struct Scenario {
    std::string name;
    double duration = 0.1;
    float ts = 100e-6f;
    float vdc = 540.0f;
    float load_torque = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 10.0f;
    bool use_speed_loop = false;
    float speed_ref = 0.0f;
    float speed_kp = 0.5f;
    float speed_ki = 20.0f;
    MPCCMode mode = MPCCMode::ConventionalOneStep;
    float param_scale_rs = 1.0f;
    float param_scale_ld = 1.0f;
    float param_scale_lq = 1.0f;
    float param_scale_psi = 1.0f;
};

class ClosedLoopSimulator {
public:
    ClosedLoopSimulator(PmsmParameters plant_params, MpccParameters ctrl_params)
        : plant_(plant_params), controller_(ctrl_params) {}

    std::vector<SimulationSample> run(const Scenario& scenario) {
        MpccParameters ctrl = controller_.parameters();
        ctrl.mode = scenario.mode;
        ctrl.ts = scenario.ts;
        controller_.parameters() = ctrl;

        PmsmParameters plant_params = plant_.parameters();
        plant_params.rs *= scenario.param_scale_rs;
        plant_params.ld *= scenario.param_scale_ld;
        plant_params.lq *= scenario.param_scale_lq;
        plant_params.psi_f *= scenario.param_scale_psi;
        plant_ = PmsmPlant(plant_params);
        controller_ = MpccController(ctrl);
        plant_.reset();

        std::vector<SimulationSample> samples;
        const int steps = static_cast<int>(scenario.duration / scenario.ts);
        float speed_integral = 0.0f;

        for (int k = 0; k < steps; ++k) {
            const double t = k * scenario.ts;
            const auto& st = plant_.state();

            float id_ref = scenario.id_ref;
            float iq_ref = scenario.iq_ref;

            if (scenario.use_speed_loop) {
                const float speed_error = scenario.speed_ref - st.omega_m;
                speed_integral += speed_error * scenario.ts;
                iq_ref = scenario.speed_kp * speed_error + scenario.speed_ki * speed_integral;
            }

            MpccInputs in;
            in.id = st.id;
            in.iq = st.iq;
            in.id_ref = id_ref;
            in.iq_ref = iq_ref;
            in.theta_e = st.theta_e;
            in.omega_e = st.omega_e;
            in.vdc = scenario.vdc;
            in.enable = true;

            const auto t0 = std::chrono::steady_clock::now();
            const MpccOutputs out = controller_.update(in);
            const auto t1 = std::chrono::steady_clock::now();
            const double exec_us =
                std::chrono::duration<double, std::micro>(t1 - t0).count();

            const float valpha = out.valpha;
            const float vbeta = out.vbeta;
            plant_.stepAlphaBeta(valpha, vbeta, scenario.load_torque, scenario.ts);

            SimulationSample sample;
            sample.time = t;
            sample.ia = plant_.state().ia;
            sample.ib = plant_.state().ib;
            sample.ic = plant_.state().ic;
            sample.id = plant_.state().id;
            sample.iq = plant_.state().iq;
            sample.id_ref = id_ref;
            sample.iq_ref = iq_ref;
            sample.omega_m = plant_.state().omega_m;
            sample.omega_e = plant_.state().omega_e;
            sample.theta_m = plant_.state().theta_m;
            sample.theta_e = plant_.state().theta_e;
            sample.torque_em = plant_.state().torque_em;
            sample.load_torque = scenario.load_torque;
            sample.vdc = scenario.vdc;
            sample.sa = out.sa;
            sample.sb = out.sb;
            sample.sc = out.sc;
            sample.switching_state = static_cast<int>(out.switching_state);
            sample.valpha = out.valpha;
            sample.vbeta = out.vbeta;
            sample.vd = out.vd;
            sample.vq = out.vq;
            sample.predicted_id = out.predicted_id;
            sample.predicted_iq = out.predicted_iq;
            sample.cost = out.min_cost;
            sample.controller_exec_us = exec_us;
            samples.push_back(sample);
        }
        return samples;
    }

    static void writeCsv(const std::string& path, const std::vector<SimulationSample>& samples) {
        std::ofstream out(path);
        out << "time,ia,ib,ic,id,iq,id_reference,iq_reference,mechanical_speed,electrical_speed,"
               "mechanical_angle,electrical_angle,electromagnetic_torque,load_torque,dc_link_voltage,"
               "Sa,Sb,Sc,switching_state,v_alpha,v_beta,v_d,v_q,predicted_id,predicted_iq,cost,"
               "controller_execution_time\n";
        for (const auto& s : samples) {
            out << s.time << ',' << s.ia << ',' << s.ib << ',' << s.ic << ',' << s.id << ',' << s.iq << ','
                << s.id_ref << ',' << s.iq_ref << ',' << s.omega_m << ',' << s.omega_e << ',' << s.theta_m << ','
                << s.theta_e << ',' << s.torque_em << ',' << s.load_torque << ',' << s.vdc << ',' << (s.sa ? 1 : 0)
                << ',' << (s.sb ? 1 : 0) << ',' << (s.sc ? 1 : 0) << ',' << s.switching_state << ',' << s.valpha
                << ',' << s.vbeta << ',' << s.vd << ',' << s.vq << ',' << s.predicted_id << ',' << s.predicted_iq
                << ',' << s.cost << ',' << s.controller_exec_us << '\n';
        }
    }

private:
    PmsmPlant plant_;
    MpccController controller_;
};

}  // namespace simulation
